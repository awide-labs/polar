# 102_logindex_replica_standby_replay.pl
#	  Test case: one primary node, one replica node and one standby node.
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
#	  src/test/polar_consistency/t/102_logindex_replica_standby_replay.pl
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use PolarDB::DCRegression;

# skip data consistency check
if (!defined $ENV{DCCHECK_ALL} or $ENV{DCCHECK_ALL} ne 1)
{
	plan skip_all => 'skip because of disabled DCCHECK_ALL.';
}

$ENV{DCCheckEnvFile} = "$ENV{'PWD'}/dccheck_env";

# parse dccheck_schedule_list
my $schedule_list = "dccheck_schedule_list";
open my $dccheck_schedule, '<', $schedule_list
  or die "Failed to open $schedule_list: $!";
my @prepared_schedules = <$dccheck_schedule>;
close $dccheck_schedule;

my $schedule_count = @prepared_schedules;

foreach my $schedule (@prepared_schedules)
{
	my ($schedule_file, $sql_dir, $mode) = ("", "", "");
	$schedule =~ s/^\s+|\s+$//g;
	if ($schedule =~ qr/\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,\n]+)\s*/i)
	{
		$schedule_file = $1;
		$sql_dir = $2;
		$mode = $3;
	}
	print "test sqls for [$schedule]\n";

	my $node_primary = PostgreSQL::Test::Cluster->new('primary');
	$node_primary->polar_init_primary;

	my $node_replica1 = PostgreSQL::Test::Cluster->new('replica1');
	$node_replica1->polar_init_replica($node_primary);

	my $node_standby1 = PostgreSQL::Test::Cluster->new('standby1');
	$node_standby1->polar_init_standby($node_primary);

	$node_primary->start;
	$node_primary->polar_create_slot($node_replica1->name);
	$node_primary->polar_create_slot($node_standby1->name);
	$node_replica1->start;
	$node_standby1->start;
	$node_standby1->polar_drop_all_slots;

	my @replicas = ($node_replica1, $node_standby1);
	my $regress = DCRegression->create_new_test($node_primary);
	$regress->register_random_pg_command('restart: standby1 replica1');
	$regress->register_random_pg_command('reload: all');
	$regress->register_random_pg_command('polar_promote: standby1');
	$regress->register_random_pg_command('promote: replica1');
	$regress->register_random_pg_command('promote: standby1');
	print "random pg commands are: " . $regress->get_random_pg_command . "\n";
	my $start_time = time();
	my $failed = $regress->test($schedule_file, $sql_dir, \@replicas);
	ok($failed == 0, "PolarDB regression test for [$schedule]");

	if ($failed)
	{
		if (-e `printf polar_dump_core`)
		{
			print `ps -ef | grep postgres: | xargs -n 1 -P 0 gcore`;
		}
		sleep(86400);
		die "PolarDB regression test for [$schedule]";
	}
	my $cost_time = time() - $start_time;
	note "Test cost time $cost_time for [$schedule]\n";

	my @nodes = ($node_primary, $node_replica1, $node_standby1);
	$regress->shutdown_all_nodes(\@nodes);

	$node_primary->clean_node();
	$node_replica1->clean_node();
	$node_standby1->clean_node();
}
done_testing();
