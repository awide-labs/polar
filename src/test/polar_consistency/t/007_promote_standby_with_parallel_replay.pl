# 007_promote_standby_with_parallel_replay.pl
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
#         src/test/polar_consistency/t/007_promote_standby_with_parallel_replay.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

### test for data consistency after standby promote
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;

my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->polar_init_standby($node_primary);

$node_primary->append_conf('postgresql.conf',
	"synchronous_standby_names='" . $node_standby->name . "'");

$node_primary->append_conf('postgresql.conf', 'polar_csn_enable=on');
$node_primary->append_conf('postgresql.conf', 'wal_keep_size=10240MB');

$node_standby->append_conf('postgresql.conf', 'polar_csn_enable=on');
$node_standby->append_conf('postgresql.conf', 'wal_keep_size=10240MB');

$node_primary->start;
$node_primary->polar_create_slot($node_standby->name);

$node_standby->start;

# build test table
my $test_id = 0;
$node_primary->safe_psql('postgres', 'create table test(id int);');
my $insert_lsn = $node_primary->lsn('insert');
$node_primary->safe_psql('postgres', "insert into test values($test_id);");
$node_primary->wait_for_catchup($node_standby, 'flush', $insert_lsn);
# sigstop standby's checkpointer
$node_standby->stop_child('checkpointer');
# promote standby
$node_standby->promote;
# wait for new primary to startup
$node_standby->polar_wait_for_startup(30, 0);
# new primary is ok
my $result = $node_standby->safe_psql('postgres', "select 1");
chomp($result);
ok($result == 1, "standby is promoted");
# stop primary
$node_primary->stop;
# sigstop standby's background writer and parallel bgwriter
$node_standby->stop_child('background writer');
$node_standby->stop_child('parallel bgwriter');
# test crash recovery to data consistency
$test_id += 1;
$node_standby->safe_psql('postgres', "insert into test values($test_id);");
$node_standby->stop('i');
$node_standby->start;
# wait for new primary to startup, but check for readonly connection
$node_standby->polar_wait_for_startup(30, 0);
$result = $node_standby->safe_psql('postgres', 'select max(id) from test;');
ok($result == $test_id,
	"Data consistency check after crash recovery for New Primary");
$node_standby->stop;
# clean up all the nodes
$node_primary->clean_node;
$node_standby->clean_node;

### test for that checkpoint.redo and checkpoint record are at different timelines of wal
# test for --enable-fault-injector
if ($ENV{enable_fault_injector} eq 'no')
{
	ok(1, 'This script should run with faultinjector extension.');
	done_testing();
	exit(0);
}


my $node_primary1 = PostgreSQL::Test::Cluster->new('primary1');
$node_primary1->polar_init_primary;

my $node_standby1 = PostgreSQL::Test::Cluster->new('standby1');
$node_standby1->polar_init_standby($node_primary1);

$node_primary1->append_conf('postgresql.conf',
	"synchronous_standby_names='" . $node_standby1->name . "'");

$node_primary1->append_conf('postgresql.conf', 'polar_csn_enable=on');
$node_primary1->append_conf('postgresql.conf', 'wal_keep_size=10240MB');

$node_standby1->append_conf('postgresql.conf', 'polar_csn_enable=on');
$node_standby1->append_conf('postgresql.conf', 'wal_keep_size=10240MB');

$node_primary1->start;
$node_primary1->polar_create_slot($node_standby1->name);

$node_standby1->start;

# build test table
$test_id = 0;
$node_primary1->safe_psql('postgres', 'create table test(id int);');
$node_primary1->safe_psql('postgres', "insert into test values($test_id);");
# create faultinjector extension and enable 'test_use_incremental_checkpoint'
$node_primary1->safe_psql('postgres', 'create extension faultinjector;');
# init pgbench
$node_primary1->pgbench_init(scale => 10);
# generate checkpoint
$node_primary1->safe_psql('postgres', 'checkpoint;');
# sigstop standby1's background logindex writer to stop parallel replay
$node_standby1->stop_child('background logindex writer');
# insert new id before promote
$test_id += 1;
$node_primary1->safe_psql('postgres', "insert into test values($test_id);");
# switch to next wal segment and generate some wal records
$node_primary1->safe_psql('postgres', 'select pg_switch_wal();');
$node_primary1->pgbench_test(client => 10, job => 2, time => 2);
# generate checkpoint
$node_primary1->safe_psql('postgres', 'checkpoint;');
# waiting for standby2 to receive all the wal segments
sleep 10;
$node_primary1->stop;
# use fault injector to force make lazy checkpoint after promote
$result = $node_standby1->safe_psql('postgres',
	"select inject_fault('test_use_incremental_checkpoint', 'enable', '', '', 1, -1, -1);"
);
print "enable fault inject: $result\n";
$result = $node_standby1->safe_psql('postgres',
	"select inject_fault('test_use_incremental_checkpoint', 'status');");
print "fault inject status is $result\n";
# promote standby2
$node_standby1->promote;
# wait for new primary to startup
$node_standby1->polar_wait_for_startup(30, 1);
# right now, consistent lsn should be at old timeline.
# test backend replay logindex after promote
$result = $node_standby1->safe_psql('postgres', 'select max(id) from test;');
ok($result == $test_id,
	"Data consistency check after promote for New Primary");
# insert new id after promote
$test_id += 1;
$node_standby1->safe_psql('postgres', "insert into test values($test_id);");
# generate some wal records
$node_standby1->pgbench_test(client => 10, job => 2, time => 2);
# force to create lazy checkpoint whose redo lsn should be at old timeline.
# but finally, lazy checkpoint will be transformed to normal checkpoint.
## keep trying for at most 5 mins
my $timed_out = 0;
$node_standby1->safe_psql(
	'postgres', 'checkpoint',
	timeout => 60,
	timed_out => \$timed_out);
if ($timed_out != 0)
{
	# sigcont standby1's background logindex writer to finish parallel replay
	$node_standby1->resume_child('background logindex writer');
	# timeout is not expected
	$node_standby1->safe_psql('postgres', 'checkpoint', timeout => 60);
}
# crash recovery
$node_standby1->stop('i');
$node_standby1->start;
# wait for new primary to startup, but check for readonly connection
$node_standby1->polar_wait_for_startup(30, 0);
# new primary is ok
$result = $node_standby1->safe_psql('postgres', 'select max(id) from test;');
ok($result == $test_id,
	"Data consistency check after crash recovery for New Primary");
$node_standby1->stop;

done_testing();
