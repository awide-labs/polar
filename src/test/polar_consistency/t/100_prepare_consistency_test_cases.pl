# 100_prepare_consistency_test_cases.pl
#	  prepare sqls for the next test cases.
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
#	  src/test/polar_consistency/t/100_prepare_consistency_test_cases.pl
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use PolarDB::DCRegression;

# No need to use pgbench while preparing sql files
$ENV{with_pgbench} = 0;
$ENV{DCCheckEnvFile} = "$ENV{'PWD'}/dccheck_env";

my $pwd = $ENV{'PWD'};
my $regress_dir = "$pwd/../regress/";
my $regress_sql_dir = "$regress_dir/sql/";
my $local_sql_dir = "./sql/";
my $schedule_list = "dccheck_schedule_list";

# skip data consistency check
if (!defined $ENV{DCCHECK_ALL} or $ENV{DCCHECK_ALL} ne 1)
{
	plan skip_all => 'skip because of disabled DCCHECK_ALL.';
}

# skip prepare ops if the dccheck schedule file exists
if (-f $schedule_list)
{
	print "Maybe you want to do 'make distclean' to delete schedule file\n";
	plan skip_all => "skip because $schedule_list file already exists.";
}

sub exclude_some_cases
{
	my ($all) = @_;
	my @excluded_test_cases = ('polar_cluster_settings', 'tablespace',
							   # skip following core tests due to their need
							   # for special locales, collations or encodings
							   'collate.icu.utf8', 'collate.linux.utf8',
							   'json_encoding', 'jsonpath_encoding',
							   'unicode');
	foreach my $case (@excluded_test_cases)
	{
		# \b ensures we match exact name, not a suffix (e.g. polar_tablespace)
		$all =~ s/\b$case\b//g;
	}
	# collapse double spaces left after removals
	$all =~ s/\s{2,}/ /g;
	$all =~ s/^\s+|\s+$//g;
	return $all;
}

# Parse a regress schedule file directly and produce a filtered schedule
# for DCRegression. This replaces the old approach of running pg_regress
# with --polar-prepare-sql just to capture its log output.
sub parse_schedule_file
{
	my ($source_schedule, $output_schedule) = @_;
	open my $INPUT, '<', $source_schedule
	  or die "Failed to open $source_schedule: $!";
	open my $OUTPUT, '>', $output_schedule
	  or die "Failed to open $output_schedule: $!";

	my $polar_dir = '';
	my $wrote_any = 0;

	while (my $line = <$INPUT>)
	{
		chomp $line;

		# Handle polar_dir directive: prefix test names with this directory
		if ($line =~ /^polar_dir:\s*(\S+)/)
		{
			$polar_dir = "$1/";
			next;
		}

		# Pass through polar_guc directives
		if ($line =~ /^\s*polar_guc:\s*/)
		{
			$line =~ s/^\s+|\s+$//g;
			print $OUTPUT "$line\n";
			next;
		}

		# Skip comments, empty lines, ignore directives
		next if ($line =~ /^\s*#/ || $line =~ /^\s*$/ || $line =~ /^ignore:/);

		# Handle test lines
		if ($line =~ /^test:\s*(.+)/)
		{
			my @tests = split(/\s+/, $1);

			# Add polar_dir prefix if set
			if ($polar_dir ne '')
			{
				@tests = map { "$polar_dir$_" } @tests;
			}

			# Apply exclusions
			my $test_line = exclude_some_cases(join(' ', @tests));
			next if $test_line eq '';

			print $OUTPUT "test: $test_line\n";
			$wrote_any = 1;
		}
	}

	close $INPUT;
	close $OUTPUT;
	return $wrote_any;
}

my $start_time = time();

# Copy SQL files directly from regress source directory instead of running
# pg_regress with --polar-prepare-sql. The old approach launched a full
# PostgreSQL instance and executed every regression test just to capture the
# test list from pg_regress log output -- an expensive operation that added
# several minutes of wall time (especially under DCCHECK_ALL=1 builds).
PostgreSQL::Test::Utils::system_or_bail("rm", "-rf", "$local_sql_dir");
PostgreSQL::Test::Utils::system_or_bail("cp", "-frp", "$regress_sql_dir",
	"$local_sql_dir");

# Parse schedule files directly to build the test list. This replaces
# the old handle_prepare_check_result() which parsed pg_regress log output.
my @source_schedules = (
	["$regress_dir/parallel_schedule", "parallel_schedule", "pg"],
	["$regress_dir/polar_check_schedule", "polar_check_schedule", "pg"],
);

my @prepare_results = ();
foreach my $sched (@source_schedules)
{
	my ($source, $output, $mode) = @{$sched};
	if (parse_schedule_file($source, $output))
	{
		push @prepare_results, [ $output, $local_sql_dir, $mode ];
	}
}
ok(@prepare_results > 0, "PolarDB regression prepare sqls.");

# create dccheck env file (empty -- the env variables previously captured
# from pg_regress output are not referenced by DCRegression)
open my $fd, '>', $ENV{DCCheckEnvFile}
  or die "Failed to open $ENV{DCCheckEnvFile}: $!";
close $fd;

my $cost_time = time() - $start_time;
note "Test cost time $cost_time for prepare sqls\n";

foreach my $res (@prepare_results)
{
	my ($schedule_file, $sql_dir, $mode) = @{$res};
	next if ($mode eq "env");

	my $failed;
	$start_time = time();

	print "prepare sqls for [@{$res}]\n";
	## prepared test case phase one
	my $node_primary = PostgreSQL::Test::Cluster->new('primary1');
	$node_primary->polar_init_primary;
	$node_primary->start;
	my @replicas = ();
	my $regress = DCRegression->create_new_test($node_primary);
	$regress->set_test_mode("prepare_testcase_phase_1");
	$failed = $regress->test($schedule_file, $sql_dir, \@replicas);
	ok( $failed == 0,
		"PolarDB regression prepare test cases phase one for schedule [@{$res}]\n"
	);

	if ($failed)
	{
		if (-e `printf polar_dump_core`)
		{
			print `ps -ef | grep postgres: | xargs -n 1 -P 0 gcore`;
		}
		die "Failed to prepare test cases phase one for schedule [@{$res}]\n";
	}

	$node_primary->stop;
	$node_primary->clean_node();

	## prepared test case phase two
	my $node_primary2 = PostgreSQL::Test::Cluster->new('primary2');
	$node_primary2->polar_init_primary;

	my $node_replica1 = PostgreSQL::Test::Cluster->new('replica1');
	$node_replica1->polar_init_replica($node_primary2);

	$node_primary2->append_conf('postgresql.conf',
		"synchronous_standby_names='" . $node_replica1->name . "'");

	$node_primary2->start;
	$node_primary2->polar_create_slot($node_replica1->name);
	$node_replica1->start;

	@replicas = ($node_replica1);
	$regress = DCRegression->create_new_test($node_primary2);
	$regress->set_test_mode("prepare_testcase_phase_2");
	$failed = $regress->test($schedule_file, $sql_dir, \@replicas);

	ok( $failed == 0,
		"PolarDB regression prepare test cases phase two for schedule [@{$res}]\n"
	);

	if ($failed)
	{
		if (-e `printf polar_dump_core`)
		{
			print `ps -ef | grep postgres: | xargs -n 1 -P 0 gcore`;
		}
		die "Failed to prepare test cases phase two for schedule [@{$res}]\n";
	}

	my @nodes = ($node_primary2, $node_replica1);
	$regress->shutdown_all_nodes(\@nodes);
	$node_primary2->clean_node();
	$node_replica1->clean_node();

	$cost_time = time() - $start_time;
	note "Test cost time $cost_time for schedule [@{$res}]\n";
}

# save schedule file list to dccheck_schedule_list
PostgreSQL::Test::Utils::system_or_bail("rm", "-f", "$schedule_list");
open my $dccheck_schedule, '>', $schedule_list
  or die "Failed to open $schedule_list: $!";
foreach my $schedule (@prepare_results)
{
	my ($schedule_file, $sql_dir, $mode) = @{$schedule};
	next if ($mode eq "env");

	print $dccheck_schedule "$schedule_file,$sql_dir,$mode\n";
}
close $dccheck_schedule;
done_testing();
