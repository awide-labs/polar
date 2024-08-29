# 005_promote_replica_memtable_full.pl
#	  Test for Replica's memory table is full during online promote
#
# Copyright (c) 2022, Alibaba Group Holding Limited
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
#	  src/test/polar_consistency/t/005_promote_replica_memtable_full.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# skip data consistency check because of large pgbench
if (!defined $ENV{DCCHECK_ALL} or $ENV{DCCHECK_ALL} ne 1)
{
	plan skip_all => 'skip because of disabled DCCHECK_ALL.';
}

if ($ENV{enable_fault_injector} eq 'no')
{
	plan skip_all => 'Fault injector not supported by this build';
}

my $db = 'postgres';

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;

my $node_replica = PostgreSQL::Test::Cluster->new('replica1');
$node_replica->polar_init_replica($node_primary);

$node_primary->append_conf('postgresql.conf',
	"synchronous_standby_names='" . $node_replica->name . "'");

$node_primary->append_conf('postgresql.conf', 'polar_csn_enable=on');
$node_replica->append_conf('postgresql.conf', 'polar_csn_enable=on');
# make the memory table of Replica node lower enough
$node_replica->append_conf('postgresql.conf',
	'polar_logindex_mem_size = 32MB');

# start Primary and Replica
$node_primary->start;
$node_primary->polar_create_slot($node_replica->name);
$node_replica->start;

# create faultinjector extension
$node_primary->safe_psql('postgres', 'create extension faultinjector;');
# enable 'test_skip_write_logindex'
my $result = $node_primary->safe_psql('postgres',
	"select inject_fault('test_skip_write_logindex', 'enable', '', '', 1, -1, -1);"
);
print "enable fault inject: $result\n";
$result = $node_primary->safe_psql('postgres',
	"select inject_fault('test_skip_write_logindex', 'status');");
print "fault inject status is $result\n";
# enable 'test_ignore_ro_latency'
# in case the whole buffer pool is dirty after replica promotion with old replica's slot
$result = $node_primary->safe_psql('postgres',
	"select inject_fault('test_ignore_ro_latency', 'enable', '', '', 1, -1, -1);"
);
print "enable fault inject: $result\n";
$result = $node_primary->safe_psql('postgres',
	"select inject_fault('test_ignore_ro_latency', 'status');");
print "fault inject status is $result\n";

# sigstop the replica's logindex writer
$node_replica->stop_child('background logindex writer');
my $out_loop = 600;
my $in_loop = 30;
# presume that 5 mins' pgbench is OK enough
$node_primary->pgbench_init(scale => 100);
$node_primary->pgbench_test(client => 10, job => 2, time => 300, async => 1);
for (my $i = 0; $i < $out_loop; $i++)
{
	my $ret = 'f';
	my $receive_lsn = $node_replica->safe_psql('postgres',
		'select pg_last_wal_receive_lsn()');
	for (my $j = 0; $j < $in_loop and $ret ne 't'; $j++)
	{
		my $replay_lsn = $node_replica->safe_psql('postgres',
			'select pg_last_wal_replay_lsn()');
		$ret = $node_replica->safe_psql('postgres',
			"select '$replay_lsn'::pg_lsn >= '$receive_lsn'::pg_lsn");
		print "$j: receive_lsn is $receive_lsn, replay_lsn is $replay_lsn\n";
		sleep(1);
	}
	if ($ret eq 'f')
	{
		last;
	}
}
# stop primary immediately
$node_primary->stop('i');
# sigcont the replica's logindex writer
$node_replica->resume_child('background logindex writer');
# promote replica, timeout is not expected
$node_replica->promote();
# wait for new primary to startup
$node_replica->polar_wait_for_startup(30, 0);
# new primary is ok
$result = $node_replica->safe_psql('postgres', "select 1");
chomp($result);
ok($result == 1, "Replica is promoted");

$node_replica->polar_drop_all_slots();
$node_replica->stop;
done_testing();
