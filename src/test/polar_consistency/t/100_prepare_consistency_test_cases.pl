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
my $prepare_check_file = "$pwd/prepare_check.log";
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
	my @excluded_test_cases = ('polar_cluster_settings', 'tablespace');
	foreach my $case (@excluded_test_cases)
	{
		$all =~ s/[^\s]*$case//g;
	}
	$all =~ s/^\s+|\s+$//g;
	return $all;
}

sub handle_prepare_check_result
{
	my ($file) = @_;
	open my $INPUT, '<', $file or die "Failed to open $file: $!";
	my @all_lines = <$INPUT>;
	close $INPUT;
	my $schedule_pattern =
	  qr/PATH=.* .*\/src\/test\/regress\/pg_regress .* --polar-prepare-sql .* --schedule=.*\/([^\s]+)/i;
	my $ora_mode_pattern = qr/\s+--compatibility_mode=ora\s+/i;
	my $parallel_group_pattern =
	  qr/^parallel group \([0-9]+ tests\):\s+(.*)\s*/i;
	my $prepare_env_pattern = qr/^\s*prepare env\s*(.+)\s*=\s*(.+)\s*/i;
	my $single_test_pattern =
	  qr/^[^\s]+\s+[0-9]+\s+\-\s+([^\s]+)\s+[0-9]+\s+ms/i;
	my $guc_set_pattern = qr/^\s*polar_guc:\s*([^\s]+)\s*=\s*(.+)\s*/i;
	my @result = ();
	my $OUTPUT = undef;

	for my $line (@all_lines)
	{
		if ($line =~ $prepare_env_pattern)
		{
			print "found $1=$2\n";
			push @result, [ $1, $2, "env" ];
		}
		elsif ($line =~ $schedule_pattern)
		{
			my $schedule_file = $1;

			if ($line =~ $ora_mode_pattern)
			{
				push @result, [ $schedule_file, $local_sql_dir, "ora" ];
			}
			else
			{
				push @result, [ $schedule_file, $local_sql_dir, "pg" ];
			}

			if (defined $OUTPUT)
			{
				close $OUTPUT;
				$OUTPUT = undef;
			}
			open $OUTPUT, '>', $schedule_file
			  or die "Failed to open $schedule_file: $!";
		}
		elsif ($line =~ $parallel_group_pattern
			|| $line =~ $single_test_pattern)
		{
			my $test_cases = exclude_some_cases($1);
			if ($test_cases eq '')
			{
				next;
			}
			if (!defined $OUTPUT)
			{
				die "No opened schedule file!\n";
			}
			print $OUTPUT "test:$test_cases\n";
		}
		elsif ($line =~ $guc_set_pattern)
		{
			$line =~ s/^\s+|\s+$//g;
			print $OUTPUT "$line\n";
		}
	}
	if (defined $OUTPUT)
	{
		close $OUTPUT;
	}
	return @result;
}

my $start_time = time();
my $regress_clean_cmd = "make clean -C $regress_dir";
my @all_prepare_modes = ("prepare-dccheck");
# POLAR_TODO ora
# my @all_prepare_modes = ("prepare-dccheck", "prepare-ora-dccheck");
srand();
my $random_prepare_mod = @all_prepare_modes[ int(rand(@all_prepare_modes)) ];
my $prepare_cmd =
  "make $random_prepare_mod -C $regress_dir > $prepare_check_file";
PostgreSQL::Test::Utils::system_or_bail($regress_clean_cmd);
PostgreSQL::Test::Utils::system_or_bail("rm", "-f", "$prepare_check_file");
PostgreSQL::Test::Utils::system_or_bail($prepare_cmd);
PostgreSQL::Test::Utils::system_or_bail("rm", "-rf", "$local_sql_dir");
PostgreSQL::Test::Utils::system_or_bail("cp", "-frp", "$regress_sql_dir",
	"$local_sql_dir");

my @prepare_results = handle_prepare_check_result($prepare_check_file);
ok(@prepare_results > 0, "PolarDB regression prepare sqls.");

# create dccheck env file
open my $fd, '>', $ENV{DCCheckEnvFile}
  or die "Failed to open $ENV{DCCheckEnvFile}: $!";
foreach my $res (@prepare_results)
{
	my ($env_name, $env_value, $mode) = @{$res};
	print $fd "$env_name=$env_value\n" if ($mode eq "env");
}
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
	# POLAR_TODO ora
	# $node_primary->polar_init(1, extra => ["--compatibility_mode=$mode"]);
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
		sleep(864000);
		die "Failed to prepare test cases phase one for schedule [@{$res}]\n";
	}

	$node_primary->stop;
	$node_primary->clean_node();

	## prepared test case phase two
	my $node_primary2 = PostgreSQL::Test::Cluster->new('primary2');
	$node_primary2->polar_init_primary;
	# POLAR_TODO ora
	# $node_primary2->polar_init(1, extra => ["--compatibility_mode=$mode"]);

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
		sleep(864000);
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
