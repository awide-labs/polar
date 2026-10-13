#!/usr/bin/perl
# 007_parallel_replay_for_all_recovery.pl
#	  Test parallel replay for all recovery cases.
#
# Copyright (c) 2025, Alibaba Group Holding Limited
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
#	  src/test/modules/test_logindex/t/007_parallel_replay_for_all_recovery.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $regress_db = 'postgres';

sub parallel_replay_check
{
	my ($node, $check_name, $enable_parallel_replay) = @_;

	# TODO: dummy check for now
	ok(1, "succeed to do parallel replay check");
}

our $start_id = 0;
our $insert_count = 10000;

sub insert_and_fetch_flush_lsn
{
	my ($node) = @_;
	my $flush_lsn;
	my $end_id = $start_id + $insert_count;

	$node->safe_psql('postgres', 'checkpoint;');
	$node->safe_psql('postgres',
		"insert into test select generate_series($start_id, $end_id);");
	$flush_lsn =
	  $node->safe_psql('postgres', 'select pg_current_wal_flush_lsn();');

	print("insert ids: [$start_id, $end_id] and get flush lsn $flush_lsn\n");
	$start_id += $insert_count + 1;

	return $flush_lsn;
}

sub wait_bg_replayed_lsn_die
{
	my ($node, $flush_lsn, $line) = @_;

	my $bg_replayed_lsn =
	  $node->safe_psql('postgres', 'select polar_replica_bg_replay_lsn();');
	my $msg =
	  "Timed out while waiting for bg_replayed_lsn at $line: bg_replayed_lsn $bg_replayed_lsn, flush_lsn $flush_lsn\n";

	diag $msg;
	die $msg;
}

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;

my $node_replica = PostgreSQL::Test::Cluster->new('replica');
$node_replica->polar_init_replica($node_primary);

my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->polar_init_standby($node_primary);

# don't wanna auto checkpoint
$node_primary->append_conf(
	'postgresql.conf', qq[
	checkpoint_timeout=3600
	max_wal_size=128GB
	synchronous_commit=on
]);

$node_replica->append_conf(
	'postgresql.conf', qq[
	checkpoint_timeout=3600
	max_wal_size=128GB
	synchronous_commit=on
]);

$node_standby->append_conf(
	'postgresql.conf', qq[
	checkpoint_timeout=3600
	max_wal_size=128GB
	synchronous_commit=on
]);

# parallel replay mode at begining of test
$node_primary->append_conf(
	'postgresql.conf', qq[
	polar_primary_parallel_replay_mode='disable'
	polar_pitr_parallel_replay_mode='disable'
	polar_standby_parallel_replay_mode='disable'
]);

$node_replica->append_conf(
	'postgresql.conf', qq[
	polar_primary_parallel_replay_mode='disable'
	polar_pitr_parallel_replay_mode='disable'
	polar_standby_parallel_replay_mode='disable'
]);

$node_standby->append_conf(
	'postgresql.conf', qq[
	polar_primary_parallel_replay_mode='disable'
	polar_pitr_parallel_replay_mode='disable'
	polar_standby_parallel_replay_mode='disable'
]);

$node_primary->start;
$node_primary->polar_create_slot($node_replica->name);
$node_primary->polar_create_slot($node_standby->name);

$node_replica->start;

$node_standby->start;
$node_standby->polar_drop_all_slots;

$node_primary->safe_psql('postgres', 'create extension polar_monitor;');
$node_primary->safe_psql('postgres',
	'create table test(id int primary key);');
$node_primary->safe_psql('postgres', 'checkpoint;');

my ($cur_flush_lsn, $cur_bg_replayed_lsn);

$PostgreSQL::Test::Utils::timeout_default = 10;    # for debug

## (1) check parallel replay for standby
# check disabled parallel replay
$cur_flush_lsn = insert_and_fetch_flush_lsn($node_primary);
$cur_bg_replayed_lsn = $node_standby->safe_psql('postgres',
	'select polar_replica_bg_replay_lsn();');
parallel_replay_check($node_standby,
	'check standby recovery without parallel replay at line ' . __LINE__, 0);

# check enabled parallel replay for normal recovery
$node_primary->safe_psql('postgres', 'checkpoint;');
$node_primary->wait_for_catchup($node_standby);

$node_standby->append_conf('postgresql.conf',
	qq[ polar_standby_parallel_replay_mode='recovery' ]);
$node_standby->restart;
$node_standby->polar_wait_for_startup(30);

$cur_flush_lsn = insert_and_fetch_flush_lsn($node_primary);

$node_standby->poll_query_until('postgres',
	"select polar_replica_bg_replay_lsn() >= '$cur_flush_lsn'::pg_lsn;")
  or wait_bg_replayed_lsn_die($node_standby, $cur_flush_lsn, __LINE__);

parallel_replay_check(
	$node_standby,
	'check standby recovery with parallel replay for normal recovery at line '
	  . __LINE__,
	1);

# check enabled parallel replay for instant recovery
$node_standby->append_conf('postgresql.conf',
	qq[ polar_standby_parallel_replay_mode='instant_recovery' ]);
$node_standby->restart;
$node_standby->polar_wait_for_startup(30);

$node_standby->poll_query_until('postgres',
	"select polar_replica_bg_replay_lsn() >= '$cur_flush_lsn'::pg_lsn;")
  or wait_bg_replayed_lsn_die($node_standby, $cur_flush_lsn, __LINE__);

parallel_replay_check(
	$node_standby,
	'check standby recovery with parallel replay for instant recovery at line '
	  . __LINE__,
	1);

# check enabled parallel replay for async instant recovery
$node_standby->append_conf('postgresql.conf',
	qq[ polar_standby_parallel_replay_mode='async_instant_recovery' ]);
$node_standby->restart;
$node_standby->polar_wait_for_startup(30);

$node_standby->poll_query_until('postgres',
	"select polar_replica_bg_replay_lsn() >= '$cur_flush_lsn'::pg_lsn;")
  or wait_bg_replayed_lsn_die($node_standby, $cur_flush_lsn, __LINE__);

parallel_replay_check(
	$node_standby,
	'check standby recovery with parallel replay for async instant recovery at line '
	  . __LINE__,
	1);

## (2) check parallel replay for primary
# check disabled parallel replay
insert_and_fetch_flush_lsn($node_primary);
$node_primary->stop('i');
$node_primary->start;
$node_primary->polar_wait_for_startup(30);

parallel_replay_check($node_primary,
	'check primary without parallel replay at line ' . __LINE__, 0);

# check enabled parallel replay in recovery mode
insert_and_fetch_flush_lsn($node_primary);
$node_primary->stop('i');
$node_primary->append_conf('postgresql.conf',
	qq[ polar_primary_parallel_replay_mode='recovery' ]);
$node_primary->start;
$node_primary->polar_wait_for_startup(30);

parallel_replay_check($node_primary,
	'check primary with parallel replay in recovery mode at ' . __LINE__, 0);

# check enable parallel replay in instant recovery mode
$cur_flush_lsn = insert_and_fetch_flush_lsn($node_primary);
$node_primary->stop('i');
$node_primary->append_conf('postgresql.conf',
	qq[ polar_primary_parallel_replay_mode='instant_recovery' ]);
$node_primary->start;

$node_primary->poll_query_until('postgres',
	"select polar_replica_bg_replay_lsn() >= '$cur_flush_lsn'::pg_lsn;")
  or wait_bg_replayed_lsn_die($node_primary, $cur_flush_lsn, __LINE__);

parallel_replay_check(
	$node_primary,
	'check primary with parallel replay in instant recovery mode at line '
	  . __LINE__,
	1);

# check enable parallel replay in async instant recovery mode
$cur_flush_lsn = insert_and_fetch_flush_lsn($node_primary);
$node_primary->stop('i');
$node_primary->append_conf('postgresql.conf',
	qq[ polar_primary_parallel_replay_mode='async_instant_recovery' ]);
$node_primary->start;

$node_primary->poll_query_until('postgres',
	"select polar_replica_bg_replay_lsn() >= '$cur_flush_lsn'::pg_lsn;")
  or wait_bg_replayed_lsn_die($node_primary, $cur_flush_lsn, __LINE__);

parallel_replay_check(
	$node_primary,
	'check primary with parallel replay in async instant recovery mode at line '
	  . __LINE__,
	1);

## (3) check parallel replay for PITR
my $pitr_name = "self_defined_pitr";
my $archive_path = $node_primary->archive_dir;
mkdir $archive_path;
mkdir $node_primary->backup_dir;
my $backup_path = $node_primary->backup_dir;
mkdir $backup_path;
my $polar_data_dir = "$backup_path/$pitr_name/polar_shared_data";
mkdir $polar_data_dir;

# take polar backup
$node_primary->polar_backup($pitr_name, $polar_data_dir);
# remove all slots
rmdir "$polar_data_dir/pg_replslot/*";

# shutdown primary and config archive command
$node_primary->stop;
# prepare for pitr
$node_primary->append_conf('postgresql.conf', 'archive_mode=on');
$node_primary->append_conf('postgresql.conf', 'archive_timeout=300');
$node_primary->append_conf('postgresql.conf',
	"archive_command = 'cp -i %p $archive_path/%f'");
$node_primary->start;

# insert some data and switch wal
$cur_flush_lsn = insert_and_fetch_flush_lsn($node_primary);
$node_primary->safe_psql('postgres', 'select pg_switch_wal();');

# check disabled parallel replay
my $node_pitr_1 = PostgreSQL::Test::Cluster->new('pitr1');
$node_pitr_1->init_from_backup(
	$node_primary, $pitr_name,
	has_restoring => 1,
	standby => 0);
$node_pitr_1->append_conf(
	'postgresql.conf', qq[
	recovery_target_lsn = '$cur_flush_lsn'
	recovery_target_inclusive = off
	recovery_target_action = promote
	polar_datadir='file-dio://polar_shared_data'
	polar_primary_parallel_replay_mode='disable'
	polar_pitr_parallel_replay_mode='disable'
	polar_standby_parallel_replay_mode='disable'
]);
PostgreSQL::Test::Utils::system_or_bail('rm', '-f',
	$node_pitr_1->data_dir . '/polar_node_static.conf');

$node_pitr_1->start;
$node_pitr_1->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
  or die "Timed out while waiting for PITR of pitr1 to complete";
$node_pitr_1->polar_drop_all_slots;

parallel_replay_check($node_pitr_1,
	'check PITR without parallel replay at line ' . __LINE__, 0);

$node_pitr_1->stop;

# check enabled parallel replay in recovery mode
my $node_pitr_2 = PostgreSQL::Test::Cluster->new('pitr2');
$node_pitr_2->init_from_backup(
	$node_primary, $pitr_name,
	has_restoring => 1,
	standby => 0);
$node_pitr_2->append_conf(
	'postgresql.conf', qq[
	recovery_target_lsn = '$cur_flush_lsn'
	recovery_target_inclusive = off
	recovery_target_action = promote
	polar_datadir='file-dio://polar_shared_data'
	polar_primary_parallel_replay_mode='disable'
	polar_pitr_parallel_replay_mode='recovery'
	polar_standby_parallel_replay_mode='disable'
]);
PostgreSQL::Test::Utils::system_or_bail('rm', '-f',
	$node_pitr_2->data_dir . '/polar_node_static.conf');

$node_pitr_2->start;
$node_pitr_2->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
  or die "Timed out while waiting for PITR of pitr2 to complete";
$node_pitr_2->polar_drop_all_slots;

parallel_replay_check($node_pitr_2,
	'check PITR with parallel replay in recovery mode at line ' . __LINE__,
	1);

$node_pitr_2->stop;

# check enabled parallel replay in instant recovery mode
my $node_pitr_3 = PostgreSQL::Test::Cluster->new('pitr3');
$node_pitr_3->init_from_backup(
	$node_primary, $pitr_name,
	has_restoring => 1,
	standby => 0);
$node_pitr_3->append_conf(
	'postgresql.conf', qq[
	recovery_target_lsn = '$cur_flush_lsn'
	recovery_target_inclusive = off
	recovery_target_action = promote
	polar_datadir='file-dio://polar_shared_data'
	polar_primary_parallel_replay_mode='disable'
	polar_pitr_parallel_replay_mode='instant_recovery'
	polar_standby_parallel_replay_mode='disable'
]);
PostgreSQL::Test::Utils::system_or_bail('rm', '-f',
	$node_pitr_3->data_dir . '/polar_node_static.conf');

$node_pitr_3->start;
$node_pitr_3->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
  or die "Timed out while waiting for PITR of pitr3 to complete";
$node_pitr_3->polar_drop_all_slots;

parallel_replay_check(
	$node_pitr_3,
	'check PITR with parallel replay in instant recovery mode at line '
	  . __LINE__,
	1);

$node_pitr_3->stop;

# check enabled parallel replay in async instant recovery mode
my $node_pitr_4 = PostgreSQL::Test::Cluster->new('pitr4');
$node_pitr_4->init_from_backup(
	$node_primary, $pitr_name,
	has_restoring => 1,
	standby => 0);
$node_pitr_4->append_conf(
	'postgresql.conf', qq[
	recovery_target_lsn = '$cur_flush_lsn'
	recovery_target_inclusive = off
	recovery_target_action = promote
	polar_datadir='file-dio://polar_shared_data'
	polar_primary_parallel_replay_mode='disable'
	polar_pitr_parallel_replay_mode='async_instant_recovery'
	polar_standby_parallel_replay_mode='disable'
]);
PostgreSQL::Test::Utils::system_or_bail('rm', '-f',
	$node_pitr_4->data_dir . '/polar_node_static.conf');

$node_pitr_4->start;
$node_pitr_4->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
  or die "Timed out while waiting for PITR of pitr4 to complete";
$node_pitr_4->polar_drop_all_slots;

parallel_replay_check(
	$node_pitr_4,
	'check PITR with parallel replay in async instant recovery mode at line '
	  . __LINE__,
	1);

$node_pitr_4->stop;

## (4) check parallel replay for replica online promote
$cur_flush_lsn = insert_and_fetch_flush_lsn($node_primary);
$node_primary->wait_for_catchup($node_replica);
parallel_replay_check($node_replica,
	'check replica during normal recovery at line ' . __LINE__, 0);

$node_primary->stop('i');

$node_replica->promote;
$node_replica->polar_wait_for_startup(30);
$node_replica->polar_drop_slot($node_replica->name);

$node_replica->poll_query_until('postgres',
	"select polar_replica_bg_replay_lsn() >= '$cur_flush_lsn'::pg_lsn;")
  or wait_bg_replayed_lsn_die($node_replica, $cur_flush_lsn, __LINE__);

parallel_replay_check($node_replica,
	'check replica during online promote at line ' . __LINE__, 1);

$node_replica->stop;
$node_standby->stop;
done_testing();
