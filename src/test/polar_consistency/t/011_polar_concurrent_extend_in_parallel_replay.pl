# 011_polar_concurrent_extend_in_parallel_replay.pl
#	  Test for parallel replay worker extend block concurrently
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
#	  src/test/polar_consistency/t/011_polar_concurrent_extend_in_parallel_replay.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use PolarDB::DCRegression;

# primary node
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;
$node_primary->append_conf('postgresql.conf', "restart_after_crash = false");
$node_primary->append_conf('postgresql.conf', "wal_keep_size=2048MB");

# replica
my $node_replica = PostgreSQL::Test::Cluster->new('replica1');
$node_replica->polar_init_replica($node_primary);
$node_replica->append_conf('postgresql.conf', "restart_after_crash = true");
#disable logging collector, need grep logfile later
$node_replica->append_conf('postgresql.conf', "Logging_collector = false");

$node_primary->append_conf('postgresql.conf',
	"synchronous_standby_names='" . $node_replica->name . "'");
$node_primary->start;
$node_primary->polar_create_slot($node_replica->name);
$node_replica->start;

my $wait_timeout = 30;
my $walpid =
  $node_replica->wait_walstreaming_establish_timeout($wait_timeout);
print "walreceiver pid of replica: $walpid\n";

# create table in rw
$node_primary->safe_psql('postgres', 'create table test(a int)');
$node_primary->safe_psql('postgres',
	'insert into test(a) select generate_series(1,5000000) as newval');

# modify recovery.conf to avoid start straming again
$node_replica->safe_psql('postgres',
	qq[alter system set primary_slot_name = 'test_slot';]);
$node_replica->safe_psql('postgres', 'select pg_reload_conf()');

$node_primary->safe_psql('postgres',
	qq[alter system set wal_sender_timeout = 3000;]);
$node_primary->safe_psql('postgres', 'select pg_reload_conf()');

# kill ro walreceiver to drop slot
print "ready to kill walreceiver: $walpid\n";
my @res = `kill -s 9 $walpid`;

# wait previous startup process exit
my $logdir = $node_replica->logfile;
print "replica logdir:$logdir\n";

print "wait for previous startup process exit\n";
my $found = 0;
do
{
	sleep 1;
	@res = `grep -rn "all server processes terminated" $logdir`;
	$found = @res;
	print "grep res:@res found:$found\n";
} while ($found == 0);

# wait ro crash recovery
$node_replica->polar_wait_for_startup($wait_timeout * 6);
print "stop replica startup process after crash recovery\n";
$node_replica->stop_child('startup');

my $slot_name = $node_replica->name;
$walpid = $node_primary->safe_psql('postgres',
	qq[select active_pid from pg_replication_slots where slot_name ='$slot_name';]
);
while ($walpid ne "")
{
	sleep 1;
	$walpid = $node_primary->safe_psql('postgres',
		qq[select active_pid from pg_replication_slots where slot_name ='$slot_name';]
	);
}
print "walsender pid: $walpid\n";
ok($walpid eq "", "walsender exit success");
$node_primary->polar_drop_slot($slot_name);

# create index to create fpi xlog
my $psql = $node_primary->psql_connect("postgres", $wait_timeout);
# find backend process
my $backend = $node_primary->find_child("idle");
print "primary backend pid: $backend\n";

$node_primary->psql_execute("begin;", $wait_timeout, $psql);
$node_primary->psql_execute("create index test_index on test(a);",
	$wait_timeout * 6, $psql);
my $primary_output =
  $node_primary->psql_execute("select pg_relation_filepath('test_index');",
	$wait_timeout, $psql);
my $filepath = $primary_output->{_stdout};
print "file path: $filepath\n";
my $pos = index($filepath, "/base");
my $end_pos = index($filepath, "\n");
my $index_file = substr($filepath, $pos, $end_pos - $pos);
print "test_index relfile path: $index_file\n";

# kill rw backend
print "ready to kill rw backend:$backend\n";
@res = `kill -s 9 $backend`;

# truncate index file
my $index_filepath = $node_primary->polar_get_datadir;
$index_filepath .= $index_file;
print "ready to truncate index file: $index_filepath\n";
@res = `truncate -s 0 $index_filepath`;

# promote ro
$node_replica->resume_child('startup');
$node_replica->promote;
$node_replica->polar_wait_for_startup($wait_timeout * 10);

my $msg = "polar_parallel_replay_sched";
$pos = $node_replica->wait_for_log(qr/$msg/);
print "start parallel replay, pos:$pos\n";

# wait parallel replay end
my $replay_proc = 0;
do
{
	sleep 1;
	$replay_proc = $node_replica->find_child('polar_parallel_replay_sched');
} while ($replay_proc != 0);

@res = `grep -rn "unexpected data beyond EOF in block" $logdir`;
$found = @res;
print "grep res:@res found:$found\n";
ok($found == 0,
	"no EOF error when parallel replay worker extend block concurrently");

# force flush buffer
$node_replica->safe_psql('postgres',
	'alter system set polar_force_flush_buffer to true');
$node_replica->safe_psql('postgres', 'select pg_reload_conf()');

$node_primary->_update_pid(0);
$node_replica->stop;
done_testing();
