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
use POSIX qw(ceil);
use IO::Pipe;

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
							   'encoding', 'euc_kr',
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

# Create an empty dccheck_env file.  Downstream tests open this file
# via DCRegression->new(), which dies if the file is missing.  The old
# workflow wrote "NAME=VALUE" lines captured from pg_regress log output,
# but parallel_schedule and polar_check_schedule never emitted any such
# lines, so the file has always been empty in practice.
open my $fd, '>', $ENV{DCCheckEnvFile}
  or die "Failed to open $ENV{DCCheckEnvFile}: $!";
close $fd;

my $cost_time = time() - $start_time;
note "Test cost time $cost_time for prepare sqls\n";

# --- Parallel worker execution ---
# Split large schedule files into chunks and process them with separate
# PG clusters in parallel, each handling both preparation phases.
# This significantly reduces wall time when one schedule dominates.

my $num_workers = int($ENV{DCCHECK_WORKERS} || 4);
my $worker_start_time = time();

# Build work items: keep small schedules as-is, split large ones into chunks
my @work_items = ();

foreach my $res (@prepare_results)
{
	my ($schedule_file, $sql_dir, $mode) = @{$res};
	next if ($mode eq "env");

	# Read parsed schedule preserving order of polar_guc / test directives.
	# Each polar_guc applies to all subsequent tests until overridden by
	# another polar_guc with the same key, so chunking must reconstruct the
	# active GUC state at the start of each chunk.
	open my $fh, '<', $schedule_file
	  or die "Failed to read $schedule_file: $!";
	my @items;    # ordered: { type => 'test'|'guc', line => ..., key => ... }
	my $total_tests = 0;
	while (my $line = <$fh>)
	{
		chomp $line;
		if ($line =~ /^\s*polar_guc:\s*([^\s=]+)/)
		{
			push @items, { type => 'guc', line => $line, key => $1 };
		}
		elsif ($line =~ /^test:/)
		{
			push @items, { type => 'test', line => $line };
			$total_tests++;
		}
	}
	close $fh;

	# Small schedules: process as a single work item without splitting
	if ($total_tests <= $num_workers)
	{
		push @work_items, $res;
		next;
	}

	# Large schedules: split into $num_workers chunks by test count
	my $chunk_size = ceil($total_tests / $num_workers);
	for (my $i = 0; $i < $num_workers; $i++)
	{
		my $start_test = $i * $chunk_size;
		last if $start_test >= $total_tests;
		my $end_test = $start_test + $chunk_size - 1;
		$end_test = $total_tests - 1 if $end_test >= $total_tests;

		my $chunk_file = "${schedule_file}_chunk_${i}";
		open my $out, '>', $chunk_file
		  or die "Failed to create $chunk_file: $!";

		# Walk the schedule and emit only the slice belonging to this chunk.
		# polar_guc lines set BEFORE the chunk start are accumulated into a
		# preamble (latest value per key wins) and emitted once before the
		# first in-range test. polar_guc lines WITHIN the chunk are emitted
		# in place to preserve ordering with subsequent tests.
		my %preamble;            # key -> latest guc line
		my @preamble_order;      # keys in order of first appearance
		my $seen = 0;            # number of test: lines processed so far
		my $preamble_emitted = 0;

		foreach my $it (@items)
		{
			if ($it->{type} eq 'test')
			{
				if ($seen >= $start_test && $seen <= $end_test)
				{
					if (!$preamble_emitted)
					{
						print $out "$preamble{$_}\n" for @preamble_order;
						$preamble_emitted = 1;
					}
					print $out "$it->{line}\n";
				}
				$seen++;
			}
			else
			{
				# polar_guc applies to tests at indices >= $seen
				next if $seen > $end_test;

				if ($seen <= $start_test)
				{
					push @preamble_order, $it->{key}
					  unless exists $preamble{$it->{key}};
					$preamble{$it->{key}} = $it->{line};
				}
				else
				{
					print $out "$it->{line}\n";
				}
			}
		}

		close $out;
		push @work_items, [$chunk_file, $sql_dir, $mode];
	}
}

note "Spawning " . scalar(@work_items)
  . " workers in batches of $num_workers"
  . " (max_live_children=$DCRegression::max_live_children)";

# Fork workers in batches of $num_workers to cap the number of
# simultaneously live PG cluster sets.  Each worker owns up to 2 PG
# instances (phase-1 primary; phase-2 primary + replica), so the peak
# concurrent instance count is 2 * $num_workers.
my $any_failed = 0;

for (
	my $batch_start = 0;
	$batch_start < scalar @work_items;
	$batch_start += $num_workers)
{
	my $batch_end = $batch_start + $num_workers - 1;
	$batch_end = $#work_items if $batch_end > $#work_items;

	my @batch_workers;

	for (my $wi = $batch_start; $wi <= $batch_end; $wi++)
	{
		my $pipe = IO::Pipe->new();
		my $pid = fork();
		die "fork() failed: $!" unless defined $pid;

		if ($pid == 0)
		{
			# ---- Child process ----
			$pipe->writer();
			$pipe->autoflush(1);

			# Redirect stdout/stderr to per-worker log to avoid TAP interference
			open(STDOUT, '>', "worker_${wi}.log")
			  or die "Cannot redirect STDOUT: $!";
			open(STDERR, '>&', \*STDOUT)
			  or die "Cannot redirect STDERR: $!";

			my ($schedule_file, $sql_dir, $mode) = @{$work_items[$wi]};
			my $failed = 0;

			eval {
				## Phase 1: single primary, no replicas
				print "Worker $wi: phase 1 for $schedule_file\n";

				my $node_p1 =
				  PostgreSQL::Test::Cluster->new("primary1_w${wi}");
				$node_p1->polar_init_primary;
				$node_p1->start;

				my @replicas = ();
				my $regress = DCRegression->create_new_test($node_p1);
				$regress->set_test_mode("prepare_testcase_phase_1");

				$failed =
				  $regress->test($schedule_file, $sql_dir, \@replicas);

				$node_p1->stop;
				$node_p1->clean_node();

				if ($failed)
				{
					if (-e `printf polar_dump_core`)
					{
						print
						  `ps -ef | grep postgres: | xargs -n 1 -P 0 gcore`;
					}
					die "phase 1 failed ($failed)";
				}

				## Phase 2: primary + synchronous replica
				print "Worker $wi: phase 2 for $schedule_file\n";

				my $node_p2 =
				  PostgreSQL::Test::Cluster->new("primary2_w${wi}");
				$node_p2->polar_init_primary;
				my $node_r1 =
				  PostgreSQL::Test::Cluster->new("replica1_w${wi}");
				$node_r1->polar_init_replica($node_p2);
				$node_p2->append_conf('postgresql.conf',
					"synchronous_standby_names='"
					  . $node_r1->name . "'");

				$node_p2->start;
				$node_p2->polar_create_slot($node_r1->name);
				$node_r1->start;

				@replicas = ($node_r1);
				$regress = DCRegression->create_new_test($node_p2);
				$regress->set_test_mode("prepare_testcase_phase_2");

				$failed =
				  $regress->test($schedule_file, $sql_dir, \@replicas);

				if ($failed)
				{
					if (-e `printf polar_dump_core`)
					{
						print
						  `ps -ef | grep postgres: | xargs -n 1 -P 0 gcore`;
					}
				}

				my @nodes = ($node_p2, $node_r1);
				$regress->shutdown_all_nodes(\@nodes);
				$node_p2->clean_node();
				$node_r1->clean_node();
			};

			if ($@)
			{
				print $pipe "FAIL:$@\n";
				POSIX::_exit(1);
			}

			print $pipe ($failed ? "FAIL:phase2:$failed" : "OK") . "\n";
			POSIX::_exit($failed ? 1 : 0);
		}

		# ---- Parent ----
		$pipe->reader();
		push @batch_workers,
		  {
			pid  => $pid,
			pipe => $pipe,
			item => $work_items[$wi],
			idx  => $wi
		  };
	}

	foreach my $w (@batch_workers)
	{
		waitpid($w->{pid}, 0);
		my $exit_code = $? >> 8;
		my $result = readline($w->{pipe}) // "NO_RESPONSE";
		chomp $result;

		my ($sched) = @{$w->{item}};
		my $worker_ok = ($exit_code == 0 && $result eq "OK");

		ok($worker_ok, "Worker $w->{idx}: prepare test cases for $sched");

		if (!$worker_ok)
		{
			$any_failed = 1;
			diag "Worker $w->{idx} FAILED for $sched"
			  . " (exit=$exit_code, result=$result)";
			diag "See worker_$w->{idx}.log for details";
		}
	}

	if ($any_failed)
	{
		diag "Worker logs preserved: worker_*.log";
		BAIL_OUT("Some workers failed");
	}
}

$cost_time = time() - $worker_start_time;
note "All workers completed in ${cost_time}s";

# Clean up worker logs on success
unlink glob "worker_*.log";

# save schedule file list to dccheck_schedule_list
PostgreSQL::Test::Utils::system_or_bail("rm", "-f", "$schedule_list");
open my $dccheck_schedule, '>', $schedule_list
  or die "Failed to open $schedule_list: $!";
foreach my $item (@work_items)
{
	my ($schedule_file, $sql_dir, $mode) = @{$item};
	print $dccheck_schedule "$schedule_file,$sql_dir,$mode\n";
}
close $dccheck_schedule;
done_testing();
