# 011_polar_xlog_queue.pl
#	  polar xlog queue consume test
#
# Copyright (c) 2023, Alibaba Group Holding Limited
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
#	  src/test/polar_pl/t/011_polar_xlog_queue.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $regress_db = 'postgres';

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;

my $node_replica = PostgreSQL::Test::Cluster->new('replica');
$node_replica->polar_init_replica($node_primary);

# set the polar_xlog_queue_buffers and wal_buffers to 1MB.
$node_primary->append_conf(
	'postgresql.conf', q[
		polar_xlog_queue_buffers = 1MB
		wal_buffers = 32
		wal_writer_flush_after = 32
	]
);
$node_replica->append_conf(
	'postgresql.conf', q[
		polar_xlog_queue_buffers = 1MB
		wal_buffers = 32
		wal_writer_flush_after = 32
	]
);

$node_primary->start;
$node_primary->polar_create_slot($node_replica->name);
$node_replica->start;

### 1
# find logindex saver process, replica should *not* have this process right now.
my $ro_saver_pid = $node_replica->find_child("polar logindex saver");
print "ro_saver_pid pid: $ro_saver_pid\n";
is($ro_saver_pid, "0", "test ro has not saver process.");

# create polar_monitor extension.
$node_primary->safe_psql($regress_db, "create extension polar_monitor");
my $res =
  $node_primary->safe_psql($regress_db, "show polar_xlog_queue_buffers");

### 2
# the xlog queue buffers should be 1MB.
print "xlog queue buffers size is $res \n.";
is($res, "1MB", "test xlog queue size.");

### 3
# insert data which related wal-meta large than 1MB.
$node_primary->safe_psql($regress_db, q[create table test(a int);]);
$node_primary->safe_psql($regress_db,
	q[insert into test(a) select generate_series(1,100000);]);

# The xlog queue's min used pread may have no chance to be updated
# while RO's replication streaming is working well and no more WAL
# records would be created. So the xlog_queue_free_ratio of function
# xlog_queue_stat_info() may has no point here. Just make sure the
# free retio is bigger than zero.
$res = $node_primary->safe_psql($regress_db,
	"select xlog_queue_free_ratio from xlog_queue_stat_info();");
print "xlog queue free result is $res\n.";
ok(($res + 0) > 0.0, "test xlog queue free result.");

### 4
# promote the replica and demote primary
$node_primary->stop;
$node_replica->promote;
$node_replica->polar_wait_for_startup(60, 0);
$node_replica->polar_drop_all_slots;

# add a standby and set it's root to replica
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->polar_init_standby($node_replica);
$node_replica->polar_create_slot($node_standby->name);

$node_standby->append_conf(
	'postgresql.conf', q[
		polar_xlog_queue_buffers = 1MB
		wal_buffers = 32
		wal_writer_flush_after = 32
	]
);

$node_standby->start;
$node_replica->wait_for_catchup($node_standby);

# find logindex saver process, rreplica should *have* this process right now.
$ro_saver_pid = $node_replica->find_child("polar logindex saver");
print "ro_saver_pid pid: $ro_saver_pid\n";
isnt($ro_saver_pid, "0", "test ro has saver process after promote.");
$node_replica->safe_psql($regress_db,
	q[insert into test(a) select generate_series(100001,200000);]);

$node_replica->safe_psql($regress_db, q[select pg_sleep(1);]);

### 5
# the xlog queue should be successful consumed when replica promote.
$res = $node_replica->safe_psql($regress_db,
	"select xlog_queue_free_ratio from xlog_queue_stat_info();");
print "xlog queue free result is $res\n.";
ok(($res + 0) > 0.05, "test xlog queue free result.");

### 6
# find logindex saver process, standby should *not* have this process right now.
my $standby_saver_pid = $node_standby->find_child("polar logindex saver");
print "standby_saver_pid pid: $standby_saver_pid\n";
is($standby_saver_pid, "0", "test standby have not saver process.");

### 7
# promote standby and close replica.
$node_replica->stop;
$node_standby->promote;
$node_standby->polar_wait_for_startup(60, 0);
$node_standby->polar_drop_all_slots;

# find logindex saver process, standby should *have* this process right now.
$standby_saver_pid = $node_standby->find_child("polar logindex saver");
print "standby_saver_pid pid: $standby_saver_pid\n";
isnt($standby_saver_pid, "0",
	"test standby has saver process after promote.");
$node_standby->safe_psql($regress_db,
	q[insert into test(a) select generate_series(200001,300000);]);

$node_standby->safe_psql($regress_db, q[select pg_sleep(1);]);

### 8
# the xlog queue should be successful consumed when standby promote.
$res = $node_standby->safe_psql($regress_db,
	"select xlog_queue_free_ratio from xlog_queue_stat_info();");
print "xlog queue free result is $res\n.";
ok(($res + 0) > 0.05, "test xlog queue free result.");

$node_standby->stop;
done_testing();
