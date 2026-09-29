#!/usr/bin/perl

# 042_drop_buffers_use_rsc.pl
#
# Copyright (c) 2024, Alibaba Group Holding Limited
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# IDENTIFICATION
#	  src/test/polar_pl/t/042_drop_buffers_use_rsc.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use Test::More;
use Time::HiRes qw(gettimeofday);

my $round = 0;

sub insert_and_truncate
{
	my ($node, $clients) = @_;

	$node->pgbench(
		qq(--no-vacuum --client=$clients --transactions=20),
		0,
		[qr{actually processed}],
		[qr{^$}],
		'INSERT AND TRUNCATE',
		{
			qq(042_drop_buffers_use_rsc_$round) => qq(
				INSERT INTO test:client_id VALUES (1,box(point(1,2),point(2,3)),to_tsvector('hello'));
				TRUNCATE test:client_id;
			)
		});

	$round++;
}

sub create_empty_and_drop
{
	my ($node, $clients) = @_;

	$node->pgbench(
		qq(--no-vacuum --client=$clients --transactions=20),
		0,
		[qr{actually processed}],
		[qr{^$}],
		'CREATE EMPTY AND DROP',
		{
			qq(042_drop_buffers_use_rsc_$round) => qq(
				BEGIN;
				INSERT INTO test:client_id VALUES (1,box(point(1,2),point(2,3)),to_tsvector('hello'));
				DELETE FROM test:client_id;
				CREATE TABLE test_empty:client_id (id int);
				DROP TABLE test_empty:client_id;
				COMMIT;
			)
		});

	$round++;
}

sub smgr_truncate
{
	my ($node, $clients) = @_;

	$node->pgbench(
		qq(--no-vacuum --client=$clients --transactions=20),
		0,
		[qr{actually processed}],
		[qr{^$}],
		'SMGR TRUNCATE',
		{
			qq(042_drop_buffers_use_rsc_$round) => qq(
				BEGIN;
				CREATE TABLE test_smgr:client_id (id int);
				INSERT INTO test_smgr:client_id VALUES (1);
				TRUNCATE test_smgr:client_id;
				ROLLBACK;
			)
		});

	$round++;
}

# init primary
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;
$node_primary->append_conf('postgresql.conf', 'shared_buffers = 2GB');
$node_primary->append_conf('postgresql.conf',
	'polar_enable_rel_size_cache = on');
$node_primary->start;

# prepare tables
my $clients = 4;
my $clients_max = 64;
for my $i (0 .. $clients_max - 1)
{
	$node_primary->safe_psql('postgres',
		qq(CREATE TABLE test$i (id int, gistcol box, vec tsvector);));
	$node_primary->safe_psql('postgres',
		qq(CREATE INDEX ON test$i USING gist (gistcol);));
	$node_primary->safe_psql('postgres',
		qq(CREATE INDEX ON test$i USING gin (vec);));
}
$node_primary->safe_psql('postgres', 'CREATE EXTENSION polar_monitor;');
$node_primary->safe_psql('postgres', 'CHECKPOINT;');

my $time_full_scan;
my $time_hash_search;
my $count_full_scan;
my $count_full_scan_optimized;

#
# Primary crash recovery: measured by recovery time.
#

# stop checkpointer and perform some DDLs
$node_primary->stop_child('checkpointer');
insert_and_truncate($node_primary, $clients);

# calculate recovery time without optimization
$node_primary->stop('immediate');
$node_primary->append_conf('postgresql.conf',
	'polar_rsc_optimize_drop_buffers = off');
$time_full_scan = gettimeofday();
$node_primary->start;
$time_full_scan = gettimeofday() - $time_full_scan;
$count_full_scan = $node_primary->safe_psql('postgres',
	"SELECT drop_buffer_full_scan FROM polar_rsc_stat_counters()");

# stop checkpointer and perform some DDLs
$node_primary->stop_child('checkpointer');
insert_and_truncate($node_primary, $clients);

# calculate recovery time with optimization
$node_primary->stop('immediate');
$node_primary->append_conf('postgresql.conf',
	'polar_rsc_optimize_drop_buffers = on');
$time_hash_search = gettimeofday();
$node_primary->start;
$time_hash_search = gettimeofday() - $time_hash_search;
$count_full_scan_optimized = $node_primary->safe_psql('postgres',
	"SELECT drop_buffer_full_scan FROM polar_rsc_stat_counters()");

ok( $count_full_scan_optimized < $count_full_scan,
	"Crash recovery with RSC (INSERT/TRUNCATE) scans: $count_full_scan_optimized/$count_full_scan, uses $time_hash_search/$time_full_scan."
);

# stop checkpointer and perform some DDLs
$node_primary->stop_child('checkpointer');
create_empty_and_drop($node_primary, $clients);

# calculate recovery time without optimization
$node_primary->stop('immediate');
$node_primary->append_conf('postgresql.conf',
	'polar_rsc_optimize_drop_buffers = off');
$time_full_scan = gettimeofday();
$node_primary->start;
$time_full_scan = gettimeofday() - $time_full_scan;
$count_full_scan = $node_primary->safe_psql('postgres',
	"SELECT drop_buffer_full_scan FROM polar_rsc_stat_counters()");

# stop checkpointer and perform some DDLs
$node_primary->stop_child('checkpointer');
create_empty_and_drop($node_primary, $clients);

# calculate recovery time with optimization
$node_primary->stop('immediate');
$node_primary->append_conf('postgresql.conf',
	'polar_rsc_optimize_drop_buffers = on');
$time_hash_search = gettimeofday();
$node_primary->start;
$time_hash_search = gettimeofday() - $time_hash_search;
$count_full_scan_optimized = $node_primary->safe_psql('postgres',
	"SELECT drop_buffer_full_scan FROM polar_rsc_stat_counters()");

ok( $count_full_scan_optimized < $count_full_scan,
	"Crash recovery with RSC (CREATE/DROP) scans: $count_full_scan_optimized/$count_full_scan, uses $time_hash_search/$time_full_scan."
);

#
# Recovery: avoid extending at the middle of a relation fork.
#
# The relation may have already been extended before crash, but redo
# SMGR_CREATE record will initialize the length of relation fork to zero. It
# may cause later extensions not happen at the actual tail of the relation
# fork. Make sure RSC can avoid it from happening.
#
$node_primary->safe_psql('postgres', 'CREATE TABLE garbage (id int)');
$node_primary->safe_psql('postgres',
	'ALTER SYSTEM SET polar_recovery_bulk_extend_size TO 0');
$node_primary->safe_psql('postgres', 'SELECT pg_reload_conf()');
$node_primary->safe_psql('postgres', 'CHECKPOINT');
$node_primary->stop_child('checkpointer');
$node_primary->safe_psql(
	'postgres', qq[
	SET polar_heap_bulk_extend_size TO '4MB';
	TRUNCATE garbage;
	INSERT INTO garbage SELECT generate_series(1,116600);
]);
$node_primary->stop('immediate');
$node_primary->start;
my $startup_extends = $node_primary->safe_psql('postgres',
	qq(SELECT extends from pg_stat_io WHERE backend_type = 'startup' AND context = 'normal')
);
ok( $startup_extends == 0,
	"Crash recovery with RSC does not produce unexpected extends: $startup_extends."
);

$node_primary->stop('fast');

#
# Replica redo: measured by DDL latency.
#

# init replica
my $node_replica = PostgreSQL::Test::Cluster->new('replica');
$node_replica->polar_init_replica($node_primary);
$node_replica->append_conf('postgresql.conf', 'shared_buffers = 2GB');
$node_replica->append_conf('postgresql.conf',
	"polar_enable_rel_size_cache = on");
$node_replica->append_conf('postgresql.conf',
	"polar_enable_replica_rel_size_cache = on");

$node_primary->start;
$node_primary->polar_create_slot($node_replica->name,
	immediately_reserve => 'true');
$node_replica->start;

$node_replica->safe_psql('postgres',
	'ALTER SYSTEM SET polar_rsc_optimize_drop_buffers TO off');
$node_replica->safe_psql('postgres', 'SELECT pg_reload_conf()');

# perform some DDLs with replica, without optimization
$count_full_scan = $node_replica->safe_psql('postgres',
	"SELECT drop_buffer_full_scan FROM polar_rsc_stat_counters()");
$time_full_scan = gettimeofday();
insert_and_truncate($node_primary, $clients);
$time_full_scan = gettimeofday() - $time_full_scan;
$count_full_scan =
  $node_replica->safe_psql('postgres',
	"SELECT drop_buffer_full_scan FROM polar_rsc_stat_counters()") -
  $count_full_scan;

$node_replica->safe_psql('postgres',
	'ALTER SYSTEM SET polar_rsc_optimize_drop_buffers TO on');
$node_replica->safe_psql('postgres', 'SELECT pg_reload_conf()');

# perform some DDLs with replica, with optimization
$count_full_scan_optimized = $node_replica->safe_psql('postgres',
	"SELECT drop_buffer_full_scan FROM polar_rsc_stat_counters()");
$time_hash_search = gettimeofday();
insert_and_truncate($node_primary, $clients);
$time_hash_search = gettimeofday() - $time_hash_search;
$count_full_scan_optimized =
  $node_replica->safe_psql('postgres',
	"SELECT drop_buffer_full_scan FROM polar_rsc_stat_counters()") -
  $count_full_scan_optimized;

ok( $count_full_scan_optimized < $count_full_scan,
	"Replica redo with RSC (INSERT/TRUNCATE) scans: $count_full_scan_optimized/$count_full_scan, uses $time_hash_search/$time_full_scan."
);

$node_replica->safe_psql('postgres',
	'ALTER SYSTEM SET polar_rsc_optimize_drop_buffers TO off');
$node_replica->safe_psql('postgres', 'SELECT pg_reload_conf()');

# perform some DDLs with replica, without optimization
$count_full_scan = $node_replica->safe_psql('postgres',
	"SELECT drop_buffer_full_scan FROM polar_rsc_stat_counters()");
$time_full_scan = gettimeofday();
create_empty_and_drop($node_primary, $clients);
$time_full_scan = gettimeofday() - $time_full_scan;
$count_full_scan =
  $node_replica->safe_psql('postgres',
	"SELECT drop_buffer_full_scan FROM polar_rsc_stat_counters()") -
  $count_full_scan;

$node_replica->safe_psql('postgres',
	'ALTER SYSTEM SET polar_rsc_optimize_drop_buffers TO on');
$node_replica->safe_psql('postgres', 'SELECT pg_reload_conf()');

# perform some DDLs with replica, with optimization
$count_full_scan_optimized = $node_replica->safe_psql('postgres',
	"SELECT drop_buffer_full_scan FROM polar_rsc_stat_counters()");
$time_hash_search = gettimeofday();
create_empty_and_drop($node_primary, $clients);
$time_hash_search = gettimeofday() - $time_hash_search;
$count_full_scan_optimized =
  $node_replica->safe_psql('postgres',
	"SELECT drop_buffer_full_scan FROM polar_rsc_stat_counters()") -
  $count_full_scan_optimized;

ok( $count_full_scan_optimized < $count_full_scan,
	"Replica redo with RSC (CREATE/DROP) scans: $count_full_scan_optimized/$count_full_scan, uses $time_hash_search/$time_full_scan."
);

$node_replica->safe_psql('postgres',
	'ALTER SYSTEM SET polar_rsc_optimize_drop_buffers TO off');
$node_replica->safe_psql('postgres', 'SELECT pg_reload_conf()');

# perform some DDLs with replica, without optimization
$count_full_scan = $node_replica->safe_psql('postgres',
	"SELECT drop_buffer_full_scan FROM polar_rsc_stat_counters()");
$time_full_scan = gettimeofday();
smgr_truncate($node_primary, $clients);
$time_full_scan = gettimeofday() - $time_full_scan;
$count_full_scan =
  $node_replica->safe_psql('postgres',
	"SELECT drop_buffer_full_scan FROM polar_rsc_stat_counters()") -
  $count_full_scan;

$node_replica->safe_psql('postgres',
	'ALTER SYSTEM SET polar_rsc_optimize_drop_buffers TO on');
$node_replica->safe_psql('postgres', 'SELECT pg_reload_conf()');

# perform some DDLs with replica, with optimization
$count_full_scan_optimized = $node_replica->safe_psql('postgres',
	"SELECT drop_buffer_full_scan FROM polar_rsc_stat_counters()");
$time_hash_search = gettimeofday();
smgr_truncate($node_primary, $clients);
$time_hash_search = gettimeofday() - $time_hash_search;
$count_full_scan_optimized =
  $node_replica->safe_psql('postgres',
	"SELECT drop_buffer_full_scan FROM polar_rsc_stat_counters()") -
  $count_full_scan_optimized;

ok( $count_full_scan_optimized < $count_full_scan,
	"Replica redo with RSC (SMGR_TRUNCATE) scans: $count_full_scan_optimized/$count_full_scan, uses $time_hash_search/$time_full_scan."
);

$node_replica->stop('fast');

# make RSC small on replica, but do not expect any data file open call
$node_replica->append_conf('postgresql.conf',
	"polar_rsc_shared_relations = 128");
$node_replica->append_conf('postgresql.conf', "recovery_prefetch = off");
$node_replica->start;

my $replica_startup_pid = $node_replica->safe_psql('postgres',
	"SELECT pid FROM pg_stat_activity WHERE backend_type = 'startup';");

my $data_file_open_count = $node_replica->safe_psql('postgres',
	"SELECT SUM(open_count) FROM polar_stat_io_info() where filetype = 'DATA' AND pid = $replica_startup_pid;"
);
insert_and_truncate($node_primary, $clients_max);
$data_file_open_count = $node_replica->safe_psql('postgres',
	"SELECT SUM(open_count) FROM polar_stat_io_info() where filetype = 'DATA' AND pid = $replica_startup_pid;"
) - $data_file_open_count;

ok( $data_file_open_count == 0,
	"Replica redo with RSC eviction does not produce any data file open call"
);

$node_replica->stop('fast');
$node_primary->polar_drop_slot($node_replica->name);

#
# Standby redo: measured by replay LSN latency.
#

# init standby
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->polar_init_standby($node_primary);
$node_standby->append_conf('postgresql.conf', 'shared_buffers = 2GB');
$node_standby->append_conf('postgresql.conf',
	"polar_enable_rel_size_cache = on");
$node_standby->append_conf('postgresql.conf',
	"polar_enable_standby_rel_size_cache = on");

$node_primary->polar_create_slot($node_standby->name);
$node_standby->start;

$node_standby->safe_psql('postgres',
	'ALTER SYSTEM SET polar_rsc_optimize_drop_buffers TO off');
$node_standby->safe_psql('postgres', 'SELECT pg_reload_conf()');

# stop standby startup and perform some DDLs with standby
$node_standby->stop_child('startup');
insert_and_truncate($node_primary, $clients);

# resume standby startup and calculate replay time without optimization
$count_full_scan = $node_standby->safe_psql('postgres',
	"SELECT drop_buffer_full_scan FROM polar_rsc_stat_counters()");
$time_full_scan = gettimeofday();
$node_standby->resume_child('startup');
$node_primary->wait_for_catchup($node_standby, 'replay');
$time_full_scan = gettimeofday() - $time_full_scan;
$count_full_scan =
  $node_standby->safe_psql('postgres',
	"SELECT drop_buffer_full_scan FROM polar_rsc_stat_counters()") -
  $count_full_scan;

$node_standby->safe_psql('postgres',
	'ALTER SYSTEM SET polar_rsc_optimize_drop_buffers TO on');
$node_standby->safe_psql('postgres', 'SELECT pg_reload_conf()');

# stop standby startup and perform some DDLs with standby
$node_standby->stop_child('startup');
insert_and_truncate($node_primary, $clients);

# resume standby startup and calculate replay time with optimization
$count_full_scan_optimized = $node_standby->safe_psql('postgres',
	"SELECT drop_buffer_full_scan FROM polar_rsc_stat_counters()");
$time_hash_search = gettimeofday();
$node_standby->resume_child('startup');
$node_primary->wait_for_catchup($node_standby, 'replay');
$time_hash_search = gettimeofday() - $time_hash_search;
$count_full_scan_optimized =
  $node_standby->safe_psql('postgres',
	"SELECT drop_buffer_full_scan FROM polar_rsc_stat_counters()") -
  $count_full_scan_optimized;

ok( $count_full_scan_optimized < $count_full_scan,
	"Standby redo with RSC (INSERT/TRUNCATE) scans: $count_full_scan_optimized/$count_full_scan, uses $time_hash_search/$time_full_scan."
);

$node_standby->safe_psql('postgres',
	'ALTER SYSTEM SET polar_rsc_optimize_drop_buffers TO off');
$node_standby->safe_psql('postgres', 'SELECT pg_reload_conf()');

# stop standby startup and perform some DDLs with standby
$node_standby->stop_child('startup');
create_empty_and_drop($node_primary, $clients);

# resume standby startup and calculate replay time without optimization
$count_full_scan = $node_standby->safe_psql('postgres',
	"SELECT drop_buffer_full_scan FROM polar_rsc_stat_counters()");
$time_full_scan = gettimeofday();
$node_standby->resume_child('startup');
$node_primary->wait_for_catchup($node_standby, 'replay');
$time_full_scan = gettimeofday() - $time_full_scan;
$count_full_scan =
  $node_standby->safe_psql('postgres',
	"SELECT drop_buffer_full_scan FROM polar_rsc_stat_counters()") -
  $count_full_scan;

$node_standby->safe_psql('postgres',
	'ALTER SYSTEM SET polar_rsc_optimize_drop_buffers TO on');
$node_standby->safe_psql('postgres', 'SELECT pg_reload_conf()');

# stop standby startup and perform some DDLs with standby
$node_standby->stop_child('startup');
create_empty_and_drop($node_primary, $clients);

# resume standby startup and calculate replay time with optimization
$count_full_scan_optimized = $node_standby->safe_psql('postgres',
	"SELECT drop_buffer_full_scan FROM polar_rsc_stat_counters()");
$time_hash_search = gettimeofday();
$node_standby->resume_child('startup');
$node_primary->wait_for_catchup($node_standby, 'replay');
$time_hash_search = gettimeofday() - $time_hash_search;
$count_full_scan_optimized =
  $node_standby->safe_psql('postgres',
	"SELECT drop_buffer_full_scan FROM polar_rsc_stat_counters()") -
  $count_full_scan_optimized;

ok( $count_full_scan_optimized < $count_full_scan,
	"Standby redo with RSC (CREATE/DROP) scans: $count_full_scan_optimized/$count_full_scan, uses $time_hash_search/$time_full_scan."
);

#
# Trigger bulk extend on primary, but relation on standby is shorter.
# Trigger truncate on primary, but the relation is still longer than standby.
# The standby and the replica under the standby should not be affected.
#
$node_primary->safe_psql('postgres', 'SELECT pg_reload_conf()');
# avoid standby to perform recovery bulk extend
$node_standby->safe_psql('postgres',
	'ALTER SYSTEM SET polar_recovery_bulk_extend_size TO 0');
$node_standby->safe_psql('postgres', 'SELECT pg_reload_conf()');

my $node_replica1 = PostgreSQL::Test::Cluster->new('replica1');
$node_replica1->polar_init_replica($node_standby);
$node_replica1->append_conf('postgresql.conf',
	"polar_enable_rel_size_cache = on");
$node_replica1->append_conf('postgresql.conf',
	"polar_enable_replica_rel_size_cache = on");

$node_standby->polar_create_slot($node_replica1->name,
	immediately_reserve => 'true');
$node_replica1->start;

# make sure RSC entry is allocated on replica
$node_primary->safe_psql('postgres', 'CREATE TABLE t1 (a int)');
my $until_lsn;

$until_lsn =
  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_insert_lsn()");
$node_replica1->poll_query_until('postgres',
	"SELECT (pg_last_wal_replay_lsn() - '$until_lsn'::pg_lsn) >= 0")
  or die "replica never caught up";
$node_replica1->safe_psql('postgres', 'SELECT COUNT(*) FROM t1');

# make primary to have just bulk extended, leaving some pages at tail
$node_primary->safe_psql(
	'postgres', qq[
	SET polar_heap_bulk_extend_size TO '4MB';
	INSERT INTO t1 SELECT generate_series(1,116600);
]);
# make primary to truncate some pages but still leave some pages at tail
$node_primary->safe_psql(
	'postgres', qq[
	SET polar_heap_bulk_extend_size TO '1MB';
	VACUUM (TRUNCATE) t1;
]);
$until_lsn =
  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_insert_lsn()");
$node_replica1->poll_query_until('postgres',
	"SELECT (pg_last_wal_replay_lsn() - '$until_lsn'::pg_lsn) >= 0")
  or die "replica never caught up";

# standby does not have bulk extended pages
# replica does not access the truncated pages
my $res = $node_replica1->safe_psql('postgres', 'SELECT COUNT(*) FROM t1');
ok($res == 116600,
	"Replica under standby works correctly after primary truncate.");

$node_replica1->stop('fast');
$node_standby->safe_psql('postgres', 'CHECKPOINT;');
$node_standby->stop('fast');
$node_primary->polar_drop_slot($node_standby->name);

$node_primary->stop('fast');

done_testing();
