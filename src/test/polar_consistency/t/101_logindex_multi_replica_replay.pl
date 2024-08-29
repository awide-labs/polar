# 101_logindex_multi_replica_replay.pl
#	  Test case: one primary nodes and multi replica nodes
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
#	  src/test/polar_consistency/t/101_logindex_multi_replica_replay.pl
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

srand();

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
	# $node_primary->polar_init(1, extra => ["--compatibility_mode=$mode"]);
	$node_primary->start;

	my @all_nodes = ();
	push @all_nodes, $node_primary;

	# build more replicas
	my @replicas = ();
	my $replica_count = int(rand(3));
	$replica_count = 1 if ($replica_count == 0);
	for (my $i = 1; $i <= $replica_count; $i += 1)
	{
		my $node_replica = PostgreSQL::Test::Cluster->new("replica$i");
		$node_replica->polar_init_replica($node_primary);
		$node_primary->polar_create_slot($node_replica->name);
		$node_replica->start;
		push @replicas, $node_replica;
	}
	push @all_nodes, @replicas;

	my $regress = DCRegression->create_new_test($node_primary);
	$regress->register_random_pg_command('reload: all');
	my $all_replica_names = '';
	foreach my $replica (@replicas)
	{
		$regress->register_random_pg_command('promote: ' . $replica->name);
		$all_replica_names .= ' ' . $replica->name;
	}
	$regress->register_random_pg_command("restart:$all_replica_names");
	$regress->register_random_pg_command('restart: primary');
	print "random pg commands are: " . $regress->get_random_pg_command . "\n";

	my $start_time = time();
	my $failed = $regress->test($schedule_file, $sql_dir, \@replicas);

	ok( $failed == 0,
		"PolarDB regression test for [$schedule] with $replica_count RO nodes"
	);

	if ($failed)
	{
		if (-e `printf polar_dump_core`)
		{
			print `ps -ef | grep postgres: | xargs -n 1 -P 0 gcore`;
		}
		sleep(864000);
		die
		  "PolarDB regression test for [$schedule] with $replica_count RO nodes";
	}
	my $cost_time = time() - $start_time;

	note "Test cost time $cost_time for [$schedule]\n";
	$regress->shutdown_all_nodes(\@all_nodes);

	foreach my $node (@all_nodes)
	{
		$node->clean_node();
	}
}
done_testing();
