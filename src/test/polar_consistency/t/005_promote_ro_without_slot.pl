# 005_promote_ro_without_slot.pl
#	  Test for buffer manager after online promote
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
#	  src/test/polar_consistency/t/005_promote_ro_without_slot.pl
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# skip data consistency check because of 30 mins pgbench
if (!defined $ENV{DCCHECK_ALL} or $ENV{DCCHECK_ALL} ne 1)
{
	plan skip_all => 'skip because of disabled DCCHECK_ALL.';
}

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary();

my $node_replica = PostgreSQL::Test::Cluster->new('replica1');
$node_replica->polar_init_replica($node_primary);

$node_primary->append_conf('postgresql.conf',
	"synchronous_standby_names='" . $node_replica->name . "'");

$node_primary->append_conf('postgresql.conf', 'polar_csn_enable=on');
$node_replica->append_conf('postgresql.conf', 'polar_csn_enable=on');

# start Primary and Replica
$node_primary->start;
$node_primary->polar_create_slot($node_replica->name);
$node_replica->start;

# take 30 seconds' pgbench test
$node_primary->pgbench_init(scale => 10);
$node_primary->pgbench_test(client => 10, job => 2, time => 30);
$node_primary->safe_psql('postgres', 'checkpoint');
# try to drop replica's slot while replica is alive
## sigstop replica's startup process in case of starting up another walreceiver
$node_replica->stop_child('startup');
## restart primary and drop replica's slot, then immediatly shutdown
$node_primary->stop('i');
$node_primary->start;
$node_primary->polar_drop_slot('replica1');
$node_primary->stop('i');
## sigcont replica's startup process
$node_replica->resume_child('startup');
# promote replica, timeout is not expected
$node_replica->promote;
# wait for new primary to startup
$node_replica->polar_wait_for_startup(30, 0);
# new primary is ok
my $result = $node_replica->safe_psql('postgres', "select 1");
chomp($result);
ok($result == 1, "Replica is promoted");
# take another 30 seconds' pgbench test
$node_replica->pgbench_test(client => 10, job => 2, time => 30);
# execute checkpoint
my $timed_out = 0;
$result = $node_replica->safe_psql(
	'postgres', 'checkpoint;select 1;',
	timeout => 120,
	timed_out => \$timed_out);
chomp($result);
print "result: $result\n";
ok($result == 1, "New Primary can flush buffer");
$node_replica->stop;
done_testing();
