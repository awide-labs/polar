# DCRegression.pm
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
#	  src/test/perl/PolarDB/DCRegression.pm

package DCRegression;
use strict;
use warnings;
use File::Basename;
use Test::More;
use IPC::Run;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;

use IO::Pipe;
use IO::Select;
use Time::HiRes qw( gettimeofday tv_interval time );
use POSIX       qw(strftime);


our @EXPORT = qw(
  create_new_test
);

our (
	@asynchronous_sql, @asynchronous_executor,
	@asynchronous_stdout_exp, @asynchronous_stderr_exp);

sub new
{
	my ($class, $node_primary, %params) = @_;
	srand;
	# use the same default database name with pg_regress
	$params{dbname} = 'regression'
	  if (!defined $params{dbname});
	my $testname = basename($0);
	$testname =~ s/\.[^.]+$//;
	my $test_log_path = $PostgreSQL::Test::Utils::log_path . "/$testname";
	mkdir $test_log_path;

	my $self = {
		_regress_db => $params{dbname},
		_node_primary => $node_primary,
		_test_log_path => $test_log_path,
		# execute some random pg commands between different test cases.
		# randome pg command could be registered by register_random_pg_command.
		_random_pg_command => '',
		_mode => 'default',
	};

	$node_primary->safe_psql(
		'postgres',
		'CREATE DATABASE ' . $params{dbname},
		timeout => 600);
	$node_primary->safe_psql(
		$params{dbname},
		'CREATE extension polar_monitor',
		timeout => 10);
	$node_primary->safe_psql($params{dbname},
		'CREATE TABLE IF NOT EXISTS replayed(val integer unique);');

	bless $self, $class;

	# set initial env
	if (defined $ENV{DCCheckEnvFile})
	{
		open my $fd, '<', $ENV{DCCheckEnvFile}
		  or die "failed to open $ENV{DCCheckEnvFile}: $!";
		foreach my $line (<$fd>)
		{
			my ($name, $value) = split('=', $line);
			$name =~ s/^\s+|\s+$//g;
			$value =~ s/^\s+|\s+$//g;
			$ENV{$name} = $value;
			note "set env '$name' to '$value'\n";
		}
		close $fd;
	}

	# turn off psql's pager
	$ENV{'PAGER'} = '';

	return $self;
}

sub regress_db
{
	my ($self) = @_;

	return $self->{_regress_db};
}

sub node_primary
{
	my ($self) = @_;

	return $self->{_node_primary};
}

sub test_log_path
{
	my ($self) = @_;

	return $self->{_test_log_path};
}

sub set_test_mode
{
	my ($self, $mode) = @_;
	$self->{_mode} = $mode;
}

sub get_test_mode
{
	my ($self) = @_;
	return $self->{_mode};
}

our $pg_command_split = "\n------\n";

sub register_random_pg_command
{
	my ($self, $pg_command) = @_;
	if ($self->{_random_pg_command} eq '')
	{
		$self->{_random_pg_command} = "$pg_command";
	}
	else
	{
		$self->{_random_pg_command} =
		  $self->{_random_pg_command} . "$pg_command_split$pg_command";
	}
}

sub get_random_pg_command
{
	my ($self) = @_;
	return $self->{_random_pg_command};
}

sub get_one_random_pg_command
{
	my ($self) = @_;
	my @pg_command_array =
	  split($pg_command_split, $self->{_random_pg_command});
	print "the length of pg_command is @pg_command_array\n";
	my $index = int(rand(@pg_command_array));
	print "get index: $index\n";
	return $pg_command_array[$index];
}

sub replay_check
{
	my ($self, $node_replica) = @_;
	my $node_primary = $self->node_primary;
	my $regress_db = $self->regress_db;
	my $root_node = $node_replica->polar_get_root_node;

	my $newval = $node_primary->safe_psql($regress_db,
		'INSERT INTO replayed(val) SELECT coalesce(max(val),0) + 1 AS newval FROM replayed RETURNING val'
	);

	$root_node->wait_for_catchup($node_replica, 'replay',
		$node_primary->lsn('insert'));
	if (!$node_replica->safe_psql(
			$regress_db, qq[SELECT 1 FROM replayed WHERE val = $newval]))
	{
		print $node_replica->name . " didn't replay primary value $newval\n";
		return 0;
	}

	return 1;
}

sub replay_check_all
{
	my ($self, $node_replica_ref, $table, $test_case) = @_;
	my $node_primary = $self->node_primary;
	my $regress_db = $self->regress_db;
	my $start_time = [gettimeofday];
	my $interval;

	if (!$table or $table eq '')
	{
		$table = "replayed";
	}

	# Replica or standby's replay lsn points to end+1 of the last record successfully
	# replayed. But Primary's insert lsn points to the begining position of the next
	# avaliable WAL record, excluding all WAL page headers. So the replay lsn of Replica
	# or standby may be lower than insert lsn of Primary, which could make wait_for_catchup
	# failed for timeout. Current solution is inserting one more data to push forward
	# replay lsn of Replica or standby.
	my $newval = $node_primary->safe_psql($regress_db,
		"INSERT INTO $table(val) SELECT coalesce(max(val),0) + 1 AS newval FROM $table RETURNING val;"
	);
	my $insert_lsn = $node_primary->lsn('insert');
	$node_primary->safe_psql($regress_db,
		"INSERT INTO $table(val) SELECT coalesce(max(val),0) + 1 AS newval FROM $table RETURNING val;"
	);
	$interval = tv_interval($start_time, [gettimeofday]);
	print
	  "replay_check_all($test_case): Inserting two values take $interval seconds.\n";
	$start_time = [gettimeofday];

	foreach my $node_replica (@{$node_replica_ref})
	{
		my $root_node = $node_replica->polar_get_root_node;
		if ($root_node->polar_get_node_type eq "standby")
		{
			$root_node->safe_psql($regress_db, qq[checkpoint],
				timeout => 600);
			print "Checkpoint on " . $root_node->name . "\n";
		}
		$root_node->wait_for_catchup($node_replica, 'replay', $insert_lsn);
		if (!$node_replica->safe_psql(
				$regress_db, qq[SELECT 1 FROM $table WHERE val = $newval]))
		{
			print $node_replica->name
			  . " didn't replay primary value $newval\n";
			return 0;
		}
		$interval = tv_interval($start_time, [gettimeofday]);
		print "replay_check_all($test_case): Replay check for '"
		  . $node_replica->name
		  . "' take $interval seconds.\n";
		$start_time = [gettimeofday];
	}
	return 1;
}

sub pgbnech_is_enabled
{
	my ($self) = @_;

	return (defined $ENV{'with_pgbench'} && $ENV{'with_pgbench'} eq 1);
}

=pod
=item $self->wait_for_all_node($node_replica_ref)
Wait for all nodes to be ready to accept connections.
=cut

sub wait_for_all_node
{
	my ($self, $node_replica_ref) = @_;
	my $node_primary = $self->node_primary;
	my $wait_time = 30;

	$node_primary->polar_wait_for_startup($wait_time);
	foreach my $node_replica (@{$node_replica_ref})
	{
		$node_replica->polar_wait_for_startup($wait_time);
	}
}

=pod
=item $self->shutdown_all_nodes($node_all_ref)
Shutdown all nodes in a specific order which Replica nodes will be shutdown at the end.
=cut

sub shutdown_all_nodes
{
	my ($self, $node_all_ref) = @_;
	foreach my $node (@{$node_all_ref})
	{
		if ($node->polar_get_node_type ne "replica")
		{
			$node->stop;
		}
	}
	foreach my $node (@{$node_all_ref})
	{
		if ($node->polar_get_node_type eq "replica")
		{
			$node->stop;
		}
	}
}

=pod
=item $self->adjust_replica_nodes
=cut

sub adjust_replica_nodes
{
	my ($self, $node_replica_ref, $old_primary, $new_primary) = @_;

	for (my $i = 0; $i < scalar(@{$node_replica_ref}); $i++)
	{
		if (${$node_replica_ref}[$i] eq $new_primary)
		{
			# put old primary into replica array
			${$node_replica_ref}[$i] = $old_primary;
			next;
		}

		# change other nodes' recovery conf
		my $target_primary = undef;
		my $node_replica = ${$node_replica_ref}[$i];
		if ($node_replica->polar_get_root_node eq $old_primary)
		{
			$target_primary = $new_primary;
		}
		elsif ($node_replica->polar_get_root_node eq $new_primary)
		{
			$target_primary = $old_primary;
		}
		else
		{
			next;
		}

		if ($node_replica->polar_get_node_type eq "replica")
		{
			$node_replica->polar_drop_recovery;
			$node_replica->polar_replica_set_recovery($target_primary);
		}
		elsif ($node_replica->polar_get_node_type eq "standby")
		{
			$node_replica->polar_drop_recovery;
			$node_replica->polar_standby_set_recovery($target_primary);
		}
	}
}

=pod
=item $self->polar_online_promote
Function mush be runing with enabled logindex.
=cut

sub polar_online_promote
{
	my ($self, $node_replica_ref, $name) = @_;
	my $new_primary = $self->node_primary;
	my $old_primary = $self->node_primary;
	foreach my $node_replica (@{$node_replica_ref})
	{
		if ($node_replica->name eq $name)
		{
			$new_primary = $node_replica;
			last;
		}
	}
	return if ($new_primary eq $old_primary);

	# make sure that replica has replayed all the logindex needed before primary stop.
	$self->replay_check_all($node_replica_ref, undef, "polar_online_promote");

	my $new_primary_origin_type = $new_primary->polar_get_node_type;
	# make sure to shutdown Primary during promoting Replica, but also standby
	$old_primary->stop;
	# option, it's better to shutdown other Replica/standby before changing their configs.
	foreach my $node_replica (@{$node_replica_ref})
	{
		if ($node_replica ne $new_primary)
		{
			$node_replica->stop;
		}
	}

	# promote Replica/standby
	$new_primary->promote;

	my $old_primary_name = $old_primary->name;
	$old_primary->polar_set_name($new_primary->name);
	if ($new_primary_origin_type eq "standby")
	{
		$old_primary->polar_standby_set_recovery($new_primary);
	}
	elsif ($new_primary_origin_type eq "replica")
	{
		$old_primary->polar_replica_set_recovery($new_primary);
	}
	else
	{
		die "Wrong node_type: $new_primary_origin_type\n";
	}

	# adjust replica nodes
	$self->adjust_replica_nodes($node_replica_ref, $old_primary,
		$new_primary);

	$new_primary->polar_set_name($old_primary_name);
	foreach my $node_replica (@{$node_replica_ref})
	{
		print "node("
		  . $node_replica->name
		  . ")'s root node is "
		  . $node_replica->polar_get_root_node->name . "\n";
	}
	$self->{_node_primary} = $new_primary;
	$old_primary->start;
	# delete and create slots during promote standby
	if ($new_primary_origin_type eq "standby")
	{
		# record old primary and new primary's slots
		my $old_primary_slots = $old_primary->safe_psql($self->regress_db,
			qq[select slot_name,slot_type from pg_replication_slots;]);
		my $new_primary_slots = $new_primary->safe_psql($self->regress_db,
			qq[select slot_name, slot_type from pg_replication_slots;]);
		# delete old primary's slot and create it for new primary
		foreach my $slot (split('\n', $old_primary_slots))
		{
			my ($name, $type) = split('\|', $slot);
			print "create new primary's $type slot [$name]\n";
			$new_primary->polar_create_slot($name, $type);
			print "drop old primary's $type slot [$name]\n";
			$old_primary->polar_drop_slot($name);
		}
		# delete new primary's slot and create it for old primary
		foreach my $slot (split('\n', $new_primary_slots))
		{
			my ($name, $type) = split('\|', $slot);
			print "new primary's $type slot is [" . $name . "]\n";
			$old_primary->polar_create_slot($name, $type);
			$new_primary->polar_drop_slot($name);
		}
	}
	# replay check for old_primary
	$self->replay_check($old_primary);
	# start the left nodes
	foreach my $node_replica (@{$node_replica_ref})
	{
		if ($node_replica ne $old_primary)
		{
			$node_replica->start;
		}
	}
}

sub polar_promote
{
	my ($self, $node_replica_ref, $name) = @_;
	my $new_primary = $self->node_primary;
	my $old_primary = $self->node_primary;
	foreach my $node_replica (@{$node_replica_ref})
	{
		if ($node_replica->name eq $name)
		{
			$new_primary = $node_replica;
			last;
		}
	}
	return
	  if ( $new_primary eq $old_primary
		or $new_primary->polar_get_node_type ne "standby");
	# make sure that standby should receive all the wal needed before stop.
	$self->replay_check_all($node_replica_ref, undef, "polar_promote");

	$self->pg_command("stop: all", $node_replica_ref);
	my $old_primary_name = $old_primary->name;
	$old_primary->polar_set_name($new_primary->name);
	$old_primary->polar_standby_set_recovery($new_primary);

	# adjust replica nodes
	$self->adjust_replica_nodes($node_replica_ref, $old_primary,
		$new_primary);

	$new_primary->polar_drop_recovery;
	$new_primary->polar_set_name($old_primary_name);
	foreach my $node_replica (@{$node_replica_ref})
	{
		print "node("
		  . $node_replica->name
		  . ")'s root node is "
		  . $node_replica->polar_get_root_node->name . "\n";
	}
	$self->{_node_primary} = $new_primary;
	$old_primary->start;
	$new_primary->start;
	# record old primary and new primary's slots
	my $old_primary_slots = $old_primary->safe_psql($self->regress_db,
		qq[select slot_name, slot_type from pg_replication_slots;]);
	my $new_primary_slots = $new_primary->safe_psql($self->regress_db,
		qq[select slot_name, slot_type from pg_replication_slots;]);
	# delete old primary's slot and create it for new primary
	foreach my $slot (split('\n', $old_primary_slots))
	{
		my ($name, $type) = split('\|', $slot);
		print "old primary's $type slot is [" . $name . "]\n";
		$new_primary->polar_create_slot($name, $type);
		$old_primary->polar_drop_slot($name);
	}
	# delete new primary's slot and create it for old primary
	foreach my $slot (split('\n', $new_primary_slots))
	{
		my ($name, $type) = split('\|', $slot);
		print "new primary's $type slot is [" . $name . "]\n";
		$old_primary->polar_create_slot($name, $type);
		$new_primary->polar_drop_slot($name);
	}
	# replay check for old_primary
	$self->replay_check($old_primary);
	# start the left nodes
	foreach my $node_replica (@{$node_replica_ref})
	{
		if ($node_replica ne $old_primary)
		{
			$node_replica->start;
		}
	}
}

sub pg_command
{
	my ($self, $line, $node_replica_ref) = @_;
	my @params = ();
	foreach my $param (split(':', $line))
	{
		$param =~ s/^\s+|\s+$//g;
		push(@params, $param);
	}
	return if (@params < 1);
	my $action = $params[0];
	return if ($action eq 'ignore');
	print "#### pg command: " . join(' ', @params) . "\n";
	if ($action eq 'sql')
	{
		shift(@params);
		return $self->sql(\@params, $node_replica_ref);
	}
	else
	{
		return $self->pg_control($action, $params[1], $node_replica_ref);
	}
}

sub sql
{
	my ($self, $params, $node_replica_ref) = @_;
	my $regress_db = $self->regress_db;
	my @nodes = ($self->node_primary);
	my $sql_command = "";
	print "### sql command: " . join(' ', @{$params}) . "\n";
	die "params is not expected: " . @{$params} . "\n"
	  if (@{$params} < 1 or @{$params} > 2);
	$sql_command = @{$params}[0];
	if (@{$params} == 2)
	{

		foreach my $node (split(' ', @{$params}[1]))
		{
			foreach my $node_replica (@{$node_replica_ref})
			{
				if (($node_replica->name eq $node) or ($node eq 'all'))
				{
					push(@nodes, $node_replica);
				}
			}
		}
	}
	for my $node (@nodes)
	{
		my $result = $node->safe_psql($regress_db, $sql_command);
		print "execute sql on node " . $node->name . ", result is $result\n";
	}
}

sub pg_control
{
	my ($self, $action, $params, $node_replica_ref) = @_;
	my $node_primary = $self->node_primary;
	my $regress_db = $self->regress_db;
	my @nodes = ();
	my @modes = ();

	if ($action eq 'polar_promote')
	{
		return $self->polar_promote($node_replica_ref, $params);
	}
	elsif ($action eq 'promote')
	{
		return $self->polar_online_promote($node_replica_ref, $params);
	}

	foreach my $param (split(' ', $params))
	{
		my ($node, $mode) = split('-', $param);
		if (($node_primary->name eq $node) or ($node eq 'all'))
		{
			push(@nodes, $node_primary);
			push(@modes, $mode);
		}
		foreach my $node_replica (@{$node_replica_ref})
		{
			if (($node_replica->name eq $node) or ($node eq 'all'))
			{
				push(@nodes, $node_replica);
				push(@modes, $mode);
			}
		}
	}
	for (my $i = 0; $i < scalar(@nodes); $i += 1)
	{
		my $node_type = $nodes[$i]->polar_get_node_type;
		my $root_node = $nodes[$i]->polar_get_root_node;

		# FIXME: primary's buffer pool could be full pf dirty
		# pages while stopping or restarting replica nodes.
		if (   $self->pgbnech_is_enabled
			&& ($action eq "restart" || $action eq "stop")
			&& $node_type eq 'replica'
			&& $root_node->polar_postmaster_is_alive)
		{
			note "skip $action for " . $nodes[$i]->name . " during pgbench";
			next;
		}

		if ($modes[$i])
		{
			$nodes[$i]->$action($modes[$i]);
		}
		else
		{
			$nodes[$i]->$action;
		}
	}
}

sub pgbench_worker_main
{
	my ($self, $node, $pipe, $database, $ppid) = @_;
	my ($total, $cmd_time, $cost_time) = (0, 0, 0);
	my $name = $node->name();
	my $regress_db = $self->regress_db;
	my $total_timeout = 3600;
	my $sfd = IO::Select->new($pipe);
	my $log_dir = $self->test_log_path;
	my $pgbench_log_file = "$log_dir/pgbench_worker_$$.log";
	my $pgbench_init_file = "$log_dir/pgbench_init_$$.sql";
	my @ready_pipes;
	my $request_shutdown = 0;
	note "start pgbench worker for $name!\n";

	open my $INIT, '+>', $pgbench_init_file
	  or die "failed to open $pgbench_init_file: $!";
	truncate($INIT, 0)
	  or die "failed to truncate $pgbench_init_file: $!";
	print $INIT "SET log_statement TO 'ddl';\n";
	close $INIT;

	while ($request_shutdown eq 0)
	{
		# node type would change after promote
		my $node_type = $node->polar_get_node_type();
		my $start_time = [gettimeofday];
		my $time_one_cycle = int(rand(20));
		my $timeout = int(rand(20));

		print "pgbench($total) for $node_type node $name\n";
		if ($node_type eq "primary")
		{
			$node->pgbench_test(
				dbname => $regress_db,
				client => 5,
				job => 5,
				time => $time_one_cycle,
				script => "tpcb-like",
				extra => "-f $pgbench_init_file &> $pgbench_log_file");
		}
		elsif ($node_type eq "replica" || $node_type eq "standby")
		{
			$node->pgbench_test(
				dbname => $regress_db,
				client => 5,
				job => 5,
				time => $time_one_cycle,
				script => "select-only",
				extra => "-f $pgbench_init_file &> $pgbench_log_file");
		}
		else
		{
			die "Unexpcted node type: $node_type!";
		}
		$total     += 1;
		$cmd_time  += $time_one_cycle;
		$cost_time += tv_interval($start_time, [gettimeofday]);

		if ((@ready_pipes = $sfd->can_read($timeout))
			&& @ready_pipes eq 1)
		{
			my $command = "";
			my $temp_pipe = $ready_pipes[0];
			die "Unexpected pipe: $temp_pipe, it should be $pipe!\n"
			  if $temp_pipe != $pipe;
			$command = <$pipe>;
			note "$$ receive command: '$command'\n";
			if (defined $command && $command =~ qr/shutdown/i)
			{
				$request_shutdown += 1;
				last;
			}
		}
		else
		{
			note "$$ receive nothing!\n";
		}
		$cost_time += $timeout;

		if ($cost_time > $total_timeout)
		{
			note "$$ has been running for too long!\n";
			last;
		}
		elsif (getppid() != $ppid)
		{
			note "$$ found parent process exited!\n";
			last;
		}

		if ($total % 10 == 0)
		{
			note "running pgbench worker($$) for $name: "
			  . "total count: $total, command time: $cmd_time s, cost time: $cost_time s\n";
		}
	}
	note "finish pgbench worker($$) for $name: "
	  . "total count: $total, command time: $cmd_time s, cost time: $cost_time s\n";
	exit;
}

sub start_pgbench_workers
{
	my ($self, $node_replica_ref) = @_;
	my $self_pid = $$;
	my $regress_db = $self->regress_db;
	my $node_primary = $self->node_primary;
	my @all_nodes = ($node_primary);
	my @pgbench_workers = ();

	if (!$self->pgbnech_is_enabled)
	{
		return;
	}

	# init pgbench
	$node_primary->pgbench_init(dbname => $regress_db);

	push @all_nodes, @{$node_replica_ref};
	foreach my $node (@all_nodes)
	{
		my $pipe = IO::Pipe->new();
		my $pid = fork();
		if ($pid == 0)
		{
			$pipe->reader;
			$self->pgbench_worker_main($node, $pipe, $regress_db, $self_pid);
			die "Never be here!";
		}
		elsif ($pid > 0)
		{
			$pipe->writer;
			note "--->Created pgbench worker process($pid) for "
			  . $node->name . "\n";
			push @pgbench_workers, ($node, $pid, $pipe);
		}
		else
		{
			die "Failed to fork pgbench worker!";
		}
	}
	return @pgbench_workers;
}

sub terminate_pgbench_workers
{
	my ($self, $pgbench_workers) = @_;

	if (!$self->pgbnech_is_enabled)
	{
		return;
	}

	die "Unexpected pgbench workers!" if (!defined $pgbench_workers);
	# send shutdown command
	for (my $index = 0; $index < scalar(@{$pgbench_workers}); $index += 3)
	{
		my $node = @{$pgbench_workers}[$index];
		my $pid = @{$pgbench_workers}[ $index + 1 ];
		my $pipe = @{$pgbench_workers}[ $index + 2 ];
		$pipe->printflush("shutdown\n");
		note "send shutdown command to pgbench worker for "
		  . $node->name . "\n";
	}
	# wait all workers to exit
	for (my $index = 0; $index < scalar(@{$pgbench_workers}); $index += 3)
	{
		my $node = @{$pgbench_workers}[$index];
		my $pid = @{$pgbench_workers}[ $index + 1 ];
		my $pipe = @{$pgbench_workers}[ $index + 2 ];
		note "wait pgbench worker $pid to exit for " . $node->name . "\n";
		my $wpid = waitpid($pid, 0);
		if ($wpid == $pid)
		{
			note "pgbench worker $pid has been exited!\n";
		}
		elsif ($wpid < 0)
		{
			note "pgbench worker $pid may have already exited!\n";
		}
		else
		{
			die "Unexpected waitpid's result for pgbench worker $pid\n";
		}
	}
}

our $max_live_children = 10;
our %DC_RES = (
	'SUCC' => "ok",
	'FAIL_STDERR' => "failed on stderr",
	'FAIL_STDOUT' => "failed on stdout",
	'FAIL_REPLAY_CHECK' => 'failed on replay check',);

sub test
{
	my ($self, $schedule_file, $sql_dir, $node_replica_ref) = @_;
	open my $schedule, '<', "$schedule_file"
	  or die "Could not open $schedule_file: $!";
	my $failed = 0;
	my $task_count = 0;
	my $regress_db = $self->regress_db;
	my @pgbench_workers = undef;

	# Do not clean data directory after error during test
	$ENV{'PG_TEST_NOCLEAN'} = 1;
	# Make sure that Replica/Standby nodes have replayed all necessary wal before test.
	$self->replay_check_all($node_replica_ref, undef, "test all");

	@pgbench_workers = $self->start_pgbench_workers($node_replica_ref);

	while (my $line = <$schedule>)
	{
		if ($line =~ qr/^test:\s*(.+)/i)
		{
			my @tasks = ();
			my $total_tasks = 0;
			my $all_tasks = $1;
			$all_tasks =~ s/^\s+|\s+$//g;
			foreach my $_task (split(' ', $all_tasks))
			{
				push @tasks, $_task;
				$total_tasks++;
			}
			$task_count += $total_tasks;
			my $left_tasks = $total_tasks;
			my $live_children = 0;
			my @pip_children = ();
			my @task_children = ();
			my $child_index = 0;
			while ($live_children > 0 or $left_tasks > 0)
			{

				while (($live_children >= $max_live_children)
					or ($left_tasks <= 0 and $live_children > 0))
				{
					print "--->waiting for child...\n";
					my $child_pid = wait();
					print "--->child($child_pid) was exited.\n";
					$live_children--;
				}
				if ($left_tasks > 0)
				{
					my $pip = IO::Pipe->new;
					my $pid = fork();
					my $res = $DC_RES{'SUCC'};
					if ($pid == 0)
					{
						$pip->writer;
						my $task = $tasks[$child_index];
						print
						  "my index is $child_index, my task is tasks[$child_index]:$task\n";
						if ($self->get_test_mode() eq 'default')
						{
							$res = $self->test_one_case($node_replica_ref,
								$sql_dir, $task);
						}
						elsif ($self->get_test_mode() eq
							'prepare_testcase_phase_1')
						{
							$res = $self->prepare_one_case_phase_1(
								$node_replica_ref, $sql_dir, $task);
						}
						elsif ($self->get_test_mode() eq
							'prepare_testcase_phase_2')
						{
							$res = $self->prepare_one_case_phase_2(
								$node_replica_ref, $sql_dir, $task);
						}
						else
						{
							BAIL_OUT("unsupported test mode");
						}
						print $pip $res;
						exit;
					}
					elsif ($pid > 0)
					{
						print "--->Created child process($pid)\n";
						$left_tasks--;
						$live_children++;
						$pip->reader;
						push @pip_children, $pip;
						push @task_children, $tasks[$child_index];
						$child_index++;
					}
				}
			}
			for (my $i = 0; $i < $child_index; $i++)
			{
				my $res = "";
				my $pip_child = $pip_children[$i];
				while (my $line = <$pip_child>)
				{
					chomp $line;
					$res .= $line;
				}
				print "--->child($i:$task_children[$i]) returns '$res'\n";
				$failed = 1 if ($res ne $DC_RES{'SUCC'});
			}
			if (    $failed eq 0
				and $task_count > int(rand(20))
				and $self->{_random_pg_command} ne '')
			{
				$task_count = 0;
				my $pg_command = $self->get_one_random_pg_command;
				print "Get one random pg command : $pg_command\n";
				$self->pg_command($pg_command, $node_replica_ref);
				# Sometimes, node will cost more time to accept connections after some pg_ctl commands.
				$self->wait_for_all_node($node_replica_ref);
			}
		}
		elsif ($line =~ qr/^\s*polar_guc:\s*([^\s]+)\s*=\s*(.+)\s*/i)
		{
			my $guc_name = $1;
			my $guc_value = $2;
			my @all_nodes = ($self->node_primary);
			my $sql =
			  "ALTER SYSTEM SET $guc_name to $guc_value;SELECT pg_reload_conf();";

			push @all_nodes, @{$node_replica_ref};
			foreach my $node (@all_nodes)
			{
				$node->safe_psql($regress_db, $sql);
			}
		}
		else
		{
			$self->pg_command($line, $node_replica_ref);
		}
		if ($failed ne 0)
		{
			last;
		}
	}

	close $schedule;
	undef $ENV{'PG_TEST_NOCLEAN'} unless $failed ne 0;

	# Make sure that Replica/Standby nodes have replayed all necessary wal before shutting down.
	if ($failed eq 0)
	{
		$self->replay_check_all($node_replica_ref, undef, "test all");
	}

	$self->terminate_pgbench_workers(\@pgbench_workers);

	return $failed;
}

our $global_timeout = 300;
our $polar_border =
  "-- POLAR_END";    # PolarDB end border of transaction sql block
our $polar_border_ro =
  "-- POLAR_END_RO";    # PolarDB end border of ro transaction sql block
our $polar_border_rw =
  "-- POLAR_END_RW";    # PolarDB end border of rw transaction sql block
our $polar_db_user_change =
  "-- POLAR_END_DB_USER_CHANGE";    # PolarDB end border of user/db change
our $polar_only_ro_commit =
  "-- POLAR_ONLY_RO_COMMIT";    # PolarDB end border of aborted transaction

sub is_sql_border
{
	my ($self, $sql) = @_;
	if (   $sql =~ qr/$polar_border_ro/i
		|| $sql =~ qr/$polar_border_rw/i
		|| $sql =~ qr/$polar_db_user_change/i
		|| $sql =~ qr/$polar_only_ro_commit/i)
	{
		return 1;
	}
	else
	{
		return 0;
	}
}

sub execute_sql
{
	my ($self, $test_name, $node, $sql, $regress_out) = @_;
	my $name = $node->name;
	my $output = $node->psql_execute(
		"\\pset tuples_only on\n\\pset format unaligned\n\\pset pager off\n"
		  . $sql);

	## reconnect and reexecute these sqls if conflict with recovery
	if (   $node->polar_get_node_type() ne "primary"
		&& $output->{_ret} ne 0
		&& $output->{_stderr} =~
		qr/\s*FATAL: .* terminating connection due to conflict with recovery\s*/i
	  )
	{
		note
		  "##($test_name) Recovery conflict found! Try to reconnect and reexecute sql!\n";
		$node->psql_reconnect();
		$output = $node->psql_execute($sql);
	}

	if ($output->{_ret} ne 0)
	{
		my $msg = "## $test_name Unexpected ret with $name node!"
		  . "stderr is: $output->{_stderr}\nsql is: $sql\n";
		note $msg;
		die $msg;
	}

	if (defined $regress_out)
	{
		print $regress_out "-----($name)stdout-----\n$output->{_stdout}\n"
		  . "-----($name)stderr-----\n$output->{_stderr}\n";
	}
	return $output;
}

sub fetch_temp_objs
{
	my ($self, $node) = @_;
	my @temp_objs_array;
	my $fetch_temp_obj_sql = "\\pset tuples_only on\n\\pset format unaligned\n
		select relname as obj from pg_sequence,pg_class where relpersistence != 'p' or seqrelid = pg_class.oid union
		select gid as obj from pg_prepared_xacts union
		select typname from pg_type as t,pg_namespace as n where typnamespace=n.oid and n.nspname like 'pg_\%temp\%' union
		select name as obj from pg_prepared_statements;";
	my $res = $self->execute_sql("$$", $node, $fetch_temp_obj_sql, undef);
	foreach my $obj (split('\n', $res->{_stdout}))
	{
		$obj =~ s/^\s+|\s+$//g;
		push @temp_objs_array, $obj;
	}
	return @temp_objs_array;
}

sub fetch_psql_vars
{
	my ($self, $node) = @_;
	my $fetch_var_sql = "\\set\n";
	my $res = $self->execute_sql("$$", $node, $fetch_var_sql, undef);
	my @var_array = split('\n', $res->{_stderr});
	my @var_name_list;
	foreach my $var (@var_array)
	{
		if ($var =~ qr/([^\s=]+)\s*=\s*/i)
		{
			push @var_name_list, $1;
		}
	}
	return @var_name_list;
}

sub fetch_random_obj_patterns
{
	# some special objects can not be controled by replica node.
	my @random_obj_patterns = (
		"temp_schema_name", "pg_my_temp_schema",
		"pg_temp\.[a-zA-Z0-9]+", "show [a-zA-Z0-9_]+;",
		"pg_stat_user_tables", "pg_cursors",
		"polar_node_type", "LAST_ERROR_MESSAGE",
		"LAST_ERROR_SQLSTATE", "pg_stat_get_[a-zA-Z_]+",
		"pg_stat_all_tables", "pg_stat_have_stats",
		"pg_stat_subscription_stats", "vac_option_tab_counts",
		"pg_stat_io", "check_estimated_rows");
	return @random_obj_patterns;
}

sub check_different_result
{
	my ($self, $sql, $primary_output, $replica_output, $temp_objs) = @_;
	my @primary_temp_output =
	  split('\n', $primary_output->{_stdout});
	my @replica_temp_output =
	  split('\n', $replica_output->{_stdout});
	my ($primary_output_index, $replica_output_index) = (0, 0);
	my $msg = "";
	my $failed = $DC_RES{'SUCC'};

	while ($primary_output_index < scalar @primary_temp_output
		|| $replica_output_index < scalar @replica_temp_output)
	{
		if (   $primary_output_index < @primary_temp_output
			&& $replica_output_index < @replica_temp_output
			&& $primary_temp_output[$primary_output_index] eq
			$replica_temp_output[$replica_output_index])
		{
			$primary_output_index++;
			$replica_output_index++;
		}
		else
		{
			my $matched = 0;
			foreach my $obj (@{$temp_objs})
			{
				if (   $primary_output_index < @primary_temp_output
					&& $primary_temp_output[$primary_output_index] =~
					qr/$obj/i)
				{
					$msg .=
					  "recheck pass: Primary output matched in temp object:$obj\n";
					$matched = 1;
					$primary_output_index++;
					last;
				}
			}
			if ($matched eq 0)
			{
				$msg .=
				  "Primary output doesn't matched in temp objects: @{$temp_objs}\n";
				last;
			}
		}
	}
	if (   $primary_output_index == scalar @primary_temp_output
		&& $replica_output_index == scalar @replica_temp_output)
	{
		$msg .= "RESULT EQUAL may not be FAILED\n";
	}
	else
	{
		my $matched = 0;
		my @random_obj_patterns;
		# temp objs could be inside sql, so recheck it
		foreach my $obj (@{$temp_objs})
		{
			if ($sql =~ qr/$obj/i)
			{
				$msg .=
				  "recheck pass: There are some uncertain sqls (contain temp objects $obj) to make the result different.\n";
				$matched = 1;
				last;
			}
		}
		if ($matched == 0)
		{
			# some special objects can not be controled by replica node, so recheck one more time for sql.
			@random_obj_patterns = $self->fetch_random_obj_patterns();
			foreach my $obj_pattern (@random_obj_patterns)
			{
				if ($sql =~ qr/$obj_pattern/i)
				{
					$msg .=
					  "recheck pass: There are some uncertain sqls (like $obj_pattern) to make the result different.\n";
					$matched = 1;
					last;
				}
			}
		}
		if ($matched == 0)
		{
			$failed = $DC_RES{'FAIL_STDOUT'};
			$msg .=
				"PANIC: RESULT EQUAL FAILED for different output!\nsql:$sql\n"
			  . "temp_objs:@{$temp_objs}\nrandom_objs:@random_obj_patterns\n";
		}
	}

	return ($failed, $msg);
}

sub check_results
{
	my ($self, $sql, $primary_output, $replica_output_array, $polar_tag,
		$temp_objs, $note_msg)
	  = @_;
	my $failed = $DC_RES{'SUCC'};

	foreach my $replica_output (@{$replica_output_array})
	{
		if ($polar_tag =~ $polar_border_rw)
		{
			if (!defined $replica_output->{_stderr}
				|| $replica_output->{_stderr} !~ qr/ERROR:/i)
			{
				$$note_msg .=
				  "PANIC: REPLICA_ERR FAILED for different errors!\n";
				$failed = $DC_RES{'FAIL_STDERR'};
			}
		}
		elsif ($polar_tag =~ $polar_border_ro
			|| $polar_tag =~ $polar_db_user_change)
		{
			if ($primary_output->{_stdout} ne $replica_output->{_stdout})
			{
				my $failed_msg;
				($failed, $failed_msg) =
				  $self->check_different_result($sql, $primary_output,
					$replica_output, $temp_objs);
				$$note_msg .= $failed_msg;
			}

			if ((      $primary_output->{_stderr} =~ qr/\s*ERROR:\s*/i
					&& $replica_output->{_stderr} !~ qr/\s*ERROR:\s*/i)
				|| (   $primary_output->{_stderr} !~ qr/\s*ERROR:\s*/i
					&& $replica_output->{_stderr} =~ qr/\s*ERROR:\s*/i))
			{
				$failed = $DC_RES{'FAIL_STDERR'};
				$$note_msg .=
				  "PANIC: RESULT EQUAL FAILED for different errors!\n";
				$$note_msg .=
				  "primary stderr: '" . $primary_output->{_stderr} . "'\n";
				$$note_msg .=
				  "replica stderr: '" . $replica_output->{_stderr} . "'\n";
			}
		}
		else
		{
			die "Wrong polar_tag: $polar_tag!";
		}

		if ($failed ne $DC_RES{'SUCC'})
		{
			last;
		}
	}

	return $failed;
}

sub save_diff_files
{
	my ($self, $node_primary, $node_replica_ref,
		$primary_output, $replica_output_array, $test_case, $sql)
	  = @_;
	my ($fname, $primary_fd, $replica_fd);
	my $diff_log_path = $PostgreSQL::Test::Utils::log_path;
	my $msg = "diff files:\n";

	$fname = "$diff_log_path/$test_case\_$$." . $node_primary->name;
	open $primary_fd, '+>', $fname
	  or die "failed to open $fname: $!";
	print $primary_fd "-----sql-----\n$sql\n"
	  . "-----stdout-----\n$primary_output->{_stdout}\n"
	  . "-----stderr-----\n$primary_output->{_stderr}\n";
	close $primary_fd;
	$msg .= "$fname\n";

	for (my $i = 0; $i < scalar @{$replica_output_array}; $i++)
	{
		my $node_replica = @{$node_replica_ref}[$i];
		my $replica_output = @{$replica_output_array}[$i];

		$fname = "$diff_log_path/$test_case\_$$." . $node_replica->name;
		open $replica_fd, '+>', $fname
		  or die "failed to open $fname: $!";
		print $replica_fd "-----sql-----\n$sql\n"
		  . "-----stdout-----\n$replica_output->{_stdout}\n"
		  . "-----stderr-----\n$replica_output->{_stderr}\n";
		close $replica_fd;
		$msg .= "$fname\n";
	}

	return $msg;
}

sub test_some_sql
{
	my ($self, $node_primary, $node_replica_ref, $test_case,
		$regress_db, $table, $sql, $line_num,
		$polar_tag, $regress_out) = @_;
	my $test_name = $test_case . "($line_num)";
	my $primary_output;
	my @replica_output_array;
	my @temp_objs;
	my ($start_seconds, $start_microseconds) = gettimeofday();
	my $start_time = localtime($start_seconds) . " " . $start_microseconds;
	my $note_msg = "$$:";
	$note_msg .= $test_name;

	if ($self->is_sql_border($polar_tag) eq 0)
	{
		die "POLAR_TAG: $polar_tag is incorrect\n";
	}

	print $regress_out "$test_name\n$sql";
	# replica try to change database or user before primary drop them.
	# make sure that database or user has already been created before
	# chnage to them.
	if ($polar_tag =~ $polar_db_user_change)
	{
		foreach my $node_replica (@{$node_replica_ref})
		{
			my $output =
			  $self->execute_sql($test_name, $node_replica, $sql,
				$regress_out);
			push @replica_output_array, $output;
		}
	}
	elsif ($polar_tag =~ $polar_only_ro_commit)
	{
		foreach my $node_replica (@{$node_replica_ref})
		{
			$self->execute_sql($test_name, $node_replica, "commit;",
				$regress_out);
		}
		return $DC_RES{'SUCC'};
	}

	# if we meet temp objects, we can recheck whether these objects' names are inside result or not.
	# But we mush fetch temp objects before primary node execute these sqls to avoid the effect of sqls.
	@temp_objs = $self->fetch_temp_objs($node_primary);
	# primary node execute sqls
	$primary_output =
	  $self->execute_sql($test_name, $node_primary, $sql, $regress_out);

	# replica node execute sql after replay_check_all because
	# it may use some objects created by primary node.
	if (   $polar_tag =~ $polar_border_rw
		&& $self->replay_check_all($node_replica_ref, $table, $test_case) ne
		1)
	{
		$note_msg .= "PANIC: replay check all failed!\n";
		note $note_msg;
		diag "\n$note_msg\n";
		return $DC_RES{'FAIL_REPLAY_CHECK'};
	}

	# replica nodes execute sqls
	if ($polar_tag !~ $polar_db_user_change)
	{
		foreach my $node_replica (@{$node_replica_ref})
		{
			my $output =
			  $self->execute_sql($test_name, $node_replica, $sql,
				$regress_out);
			push @replica_output_array, $output;
		}
	}

	# after all replica nodes succeed to execute these sqls
	my $res =
	  $self->check_results($sql, $primary_output, \@replica_output_array,
		$polar_tag, \@temp_objs, \$note_msg);

	my ($end_seconds, $end_microseconds) = gettimeofday();
	my $end_time = localtime($end_seconds) . " " . $end_microseconds;

	if ($res eq $DC_RES{'FAIL_STDOUT'})
	{
		$note_msg .= $self->save_diff_files($node_primary, $node_replica_ref,
			$primary_output, \@replica_output_array, $test_case, $sql);
		diag "\n$note_msg\n";
	}

	note "$note_msg [$start_time, $end_time]\n";
	return $res;
}

sub build_parent_dir
{
	my ($self, $file_path) = @_;
	$file_path =~ s/[^\/]+$//g;
	PostgreSQL::Test::Utils::system_or_bail("mkdir", "-p", "$file_path");
}

sub test_one_case
{
	my ($self, $node_replica_ref, $sql_dir, $test_case) = @_;
	my $sql_file = "$sql_dir/$test_case.sql";
	my $node_primary = $self->node_primary;
	my $test_log_path = $self->test_log_path;
	my $regress_out_file = "$test_log_path/$test_case.out";
	my $regress_db = $self->regress_db;

	$self->build_parent_dir($regress_out_file);
	open my $regress_out, '>', $regress_out_file
	  or die "Failed to open $regress_out_file";
	open my $INPUT, '<', $sql_file or die "Failed to open $sql_file: $!";
	my @all_sql = <$INPUT>;
	close $INPUT;
	my $line_num = 0;
	my $polar_tag;
	my $res = $DC_RES{'SUCC'};
	my $table = "replayed_$$";
	$table =~ s/[^a-zA-Z0-9]/\_/g;

	$node_primary->safe_psql($regress_db,
		"CREATE TABLE IF NOT EXISTS $table(val integer unique);");
	# connect db
	my $primary_temp_file =
	  "$test_log_path/$test_case\_$$\_" . $node_primary->name . ".out";
	$self->build_parent_dir($primary_temp_file);
	$node_primary->psql_connect($regress_db, $global_timeout,
		output_file => $primary_temp_file);
	foreach my $node_replica (@{$node_replica_ref})
	{
		my $replica_temp_file =
		  "$test_log_path/$test_case\_$$\_" . $node_replica->name . ".out";
		$self->build_parent_dir($replica_temp_file);
		$node_replica->psql_connect($regress_db, $global_timeout,
			output_file => $replica_temp_file);
	}

	# fetch sql and execute
	my $sql = "";
	for (my $index = 0; $index < @all_sql; $index++)
	{
		if ($self->is_sql_border($all_sql[$index]) eq 0)
		{
			$sql .= $all_sql[$index];
			next;
		}
		my $primary_stdout = '';
		my $primary_stderr = '';
		my $replica_stdout = '';
		my $replica_stderr = '';
		$res = $self->test_some_sql(
			$node_primary, $node_replica_ref, $test_case,
			$regress_db, $table, $sql,
			$index + 1, $all_sql[$index], $regress_out);
		if ($res ne $DC_RES{'SUCC'})
		{
			last;
		}
		$sql = "";
	}

	# close session
	if ($res eq $DC_RES{'SUCC'})
	{
		$node_primary->psql_close;
		foreach my $node_replica (@{$node_replica_ref})
		{
			$node_replica->psql_close;
		}
	}

	close $regress_out;
	return $res;
}

sub try_rewrite_sql_file
{
	my ($self, $src_file, $dst_file) = @_;
	my $rewrite = 0;
	open my $dst_sql_fd, '>>', $dst_file or die "Failed to open $dst_file";
	open my $src_sql_fd, '<', $src_file or die "Failed to open $src_file";
	my @src_sql_array = <$src_sql_fd>;
	close $src_sql_fd;
	foreach my $sql (@src_sql_array)
	{

		if ($sql =~ qr/\s*\\i\s+(.*)\.sql\s*/i)
		{
			my $target_file = $1 . ".sql";
			close $dst_sql_fd;
			$rewrite +=
			  $self->try_rewrite_sql_file($target_file, $dst_file) + 1;
			open $dst_sql_fd, '>>', $dst_file
			  or die "Failed to open $dst_file";
			next;
		}
		elsif ($sql =~
			qr/\s*update\s+pg_settings\s+set\s+setting\s*=\s*'(.+)'\s+where\s+name\s*=\s*'(.+)';/i
		  )
		{
			my $set_sql =
			  sprintf("SET %s TO '%s'; -- dccheck rewrite\n", $2, $1);
			$sql = $` . $set_sql . $';
			$rewrite += 1;
		}
		elsif ($sql =~ qr/^\s*\\o\s+/i
			|| $sql =~ qr/ALTER\s+.*\s+SET\s+TABLESPACE/i)
		{
			$sql = "-- dccheck rewrite: $sql";
			$rewrite += 1;
		}
		elsif ($sql !~ qr/copy .* from stdin\\;/i
			&& $sql !~ qr/copy .* to stdout\\;/i
			&& $sql =~ qr/\\;/i)
		{
			$sql =~ s/\\;/;\n/g;
			$rewrite += 1;
		}
		print $dst_sql_fd $sql;
	}
	close $dst_sql_fd;
	return $rewrite;
}

sub string_append
{
	my ($self, $src, $append, $delimiter) = @_;
	foreach my $str (split($delimiter, $append))
	{
		if ($$src =~ $str)
		{
			next;
		}
		if ($$src eq "")
		{
			$$src .= $str;
		}
		else
		{
			$$src .= "$delimiter$str";
		}
	}
}

# one sql block
our $block_end_status_pattern =
  qr/\s*\*+\(File:\s*([^\s]*),\s*line number:\s*([0-9]+),\s*PQTransactionStatus:\s*([0-9]+),\s*IFSTATE:\s*([0-9]+),\s*SQLSTATE:\s*([0-9A-Z]*),\s*CROSSTAB:\s*([0-9])\)\*+\s*/i;
# connect database command
our $connect_db_pattern =
  qr/\s*\*+\(File:\s*([^\s]*),\s*line number:\s*([0-9]+),\s*connect database command\)\*+\s*/i;

sub prepare_one_case_phase_1
{
	my ($self, $node_replica_ref, $sql_dir, $test_case) = @_;
	my $sql_input = "$sql_dir/$test_case.sql";
	my $sql_rewrite = "$sql_dir/$test_case.rewrite.sql";
	my $node_primary = $self->node_primary;
	my $regress_db = $self->regress_db;
	my $test_log_path = $self->test_log_path;
	my ($stderr, $stdout);

	if (defined $node_replica_ref && @{$node_replica_ref} > 0)
	{
		die
		  "($test_case,$$) Only one primary node could be needed to do this worker!("
		  . @{$node_replica_ref} . ")\n";
	}

	if (-e $sql_rewrite)
	{
		note "File $sql_input has rewrite: $sql_rewrite";
		return $DC_RES{'SUCC'};
	}

	my $rewrite = $self->try_rewrite_sql_file($sql_input, $sql_rewrite);
	note "try to rewrite $rewrite queries for file: $sql_input\n";

	## Add one more line to avoid interactive psql exit before pump,
	## Then, fetch all content from original input sql file.
	open my $src_sql_fd, '+>>', $sql_rewrite
	  or die "failed to open $sql_rewrite";
	print $src_sql_fd "\n;select 'dccheck end for $sql_input';\n";
	seek $src_sql_fd, 0, 0;
	my @src_sql_array = <$src_sql_fd>;
	close $src_sql_fd;

	# my $primary_stdout_file = "$test_log_path/$test_case\_$$.out";
	my $primary_stderr_file = "$test_log_path/$test_case\_$$.err";
	my $cur_trans_sql = "";
	my $cur_trans_sqlstate = "";
	my $src_sql_array_index = 0;
	my @dst_sql_array;
	# create psql session in single step mode for primary node
	$node_primary->psql_connect(
		$regress_db, 20,
		extra_params => [ '-f', $sql_rewrite ],
		single_step_mode => 1);

	$self->build_parent_dir($primary_stderr_file);
	open my $stderr_fd, '>', $primary_stderr_file
	  or die "failed to open $primary_stderr_file";

	while ($node_primary->psql_pumpable)
	{
		my ($cur_file, $cur_pos, $cur_txn_status, $cur_cstate, $cur_sqlstate,
			$cur_crosstab)
		  = ("", -1, -1, -1, "", -1);
		my ($db_user_chfile, $db_user_chpos) = (-1, -1);

		## first, keep sending '\n' to primary node to parse and execute some sql
		my $primary_ret = $node_primary->psql_step_forward;
		die
		  "($test_case,$$) psql_step_forward failed:\nstdout:\n$primary_ret->{_stdout}\nstderr:\n$primary_ret->{stderr}\n"
		  if ($primary_ret->{_ret} ne 0);

		if ($primary_ret->{_stderr} =~ $block_end_status_pattern)
		{
			$cur_file = $1;
			$cur_pos = $2;
			$cur_txn_status = $3;
			$cur_cstate = $4;
			$cur_sqlstate = $5;
			$cur_crosstab = $6;
		}

		# If some sqls is mixed with \c, we can't split it
		if (   $cur_cstate eq 0
			&& $primary_ret->{_stderr} =~ $connect_db_pattern)
		{
			$db_user_chfile = $1;
			$db_user_chpos = $2;
		}

		print $stderr_fd "$primary_ret->{_stdout}\n$primary_ret->{_stderr}\n";

		## first to handle $block_end_status_pattern
		for (; $src_sql_array_index < $cur_pos; $src_sql_array_index++)
		{
			$cur_trans_sql .= $src_sql_array[$src_sql_array_index];
		}
		# save sqlstate if any
		$self->string_append(\$cur_trans_sqlstate, $cur_sqlstate, ',');

		# get current PQTransactionStatus. Is this a new transaction?
		if (($cur_trans_sql ne "" || $cur_trans_sqlstate ne "")
			&& (($cur_txn_status == 0 && $cur_cstate == 0)
				|| !$node_primary->psql_pumpable))
		{
			my $last_line = "";
			if (   $cur_crosstab eq 1
				|| ($cur_trans_sql eq "" && $cur_trans_sqlstate ne "")
				|| $cur_trans_sql =~ qr/\s*\\gexec\s+/i
				|| $primary_ret->{_stder} =~
				qr/\s+there is no transaction in progress/i)
			{
				$last_line = pop(@dst_sql_array);
				if ($last_line =~ qr/$polar_border:([0-9A-Z,]+)/i)
				{
					$self->string_append(\$cur_trans_sqlstate, $1, ',');
				}
			}
			# save sql and mark tag
			if ($cur_trans_sql ne "")
			{
				if ($cur_trans_sql !~ qr/\n$/i)
				{
					$cur_trans_sql .= "\n";
				}
				push @dst_sql_array, $cur_trans_sql;
			}

			# Add one more sql 'ROLLBACK;' when current transaction
			# is still running at the end of sql file.
			if (!$node_primary->psql_pumpable && $cur_txn_status ne 0)
			{
				push @dst_sql_array, "ROLLBACK; -- Added by dccheck\n";
			}

			if ($cur_trans_sqlstate eq "")
			{
				push @dst_sql_array, "$polar_border\n";
			}
			else
			{
				push @dst_sql_array, "$polar_border:$cur_trans_sqlstate\n";
			}
			$cur_trans_sql = "";         # reset one trans buffer
			$cur_trans_sqlstate = "";    # reset one trans sqlstate
		}

		## second to handle $connect_db_pattern, add special border tag
		if ($db_user_chpos > 0 && $db_user_chpos > $cur_pos)
		{
			while ($src_sql_array_index < $db_user_chpos)
			{
				push @dst_sql_array, $src_sql_array[$src_sql_array_index];
				$src_sql_array_index += 1;
			}
			push @dst_sql_array, "$polar_db_user_change\n";
		}
	}
	close $stderr_fd;

	die "Maybe there's some sqls left in $sql_rewrite: "
	  . "$src_sql_array_index vs "
	  . @src_sql_array . "\n"
	  if ($src_sql_array_index < @src_sql_array);

	die "Unexpected result for $sql_rewrite during phase one: "
	  . @dst_sql_array . "!\n"
	  if (@dst_sql_array <= 0);

	open my $dst_sql_fd, '>', $sql_input or die "failed to open $sql_input";
	foreach my $line (@dst_sql_array)
	{
		print $dst_sql_fd $line;
	}
	close $dst_sql_fd;
	## close psql session and restore result
	$node_primary->psql_close;
	return $DC_RES{'SUCC'};
}

our @read_only_errorcodes = (
	'25006',   # E ERRCODE_READ_ONLY_SQL_TRANSACTION read_only_sql_transaction
	'0A000',   # E ERRCODE_FEATURE_NOT_SUPPORTED feature_not_supported
	'34000',   # E ERRCODE_INVALID_CURSOR_NAME invalid_cursor_name
);

our @errorcode_message_patterns = (
	# default errorcode: E ERRCODE_INTERNAL_ERROR internal_error
	[
		'XX000',
		[
			qr/\s*ERROR: .* cannot .* during recovery\s*/i,
			qr/\s*ERROR: .* ALTER SYSTEM FOR CLUSTER command only can set\s*/i
		]
	],
	# E ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE object_not_in_prerequisite_state
	[ '55000', [qr/\s*ERROR: .* recovery is in progress\s*/i] ],
	[ 'P0001', [qr/\s*ERROR: .* cannot .* in a read-only transaction\s*/i] ],
);

our @only_ro_error_patterns =
  ([ '25P02', [qr/\s*ERROR: .* current transaction is aborted\s*/i] ],);

our ($Origin, $ReadOnly, $ReadWrite, $ROCommit) = (0, 1, 2, 3);

sub is_trans_readonly
{
	my ($self, $errorcodes, $replica_stderr, $node_primary,
		$regress_db, $sql, $line_number)
	  = @_;
	my @errorcode_array = ($replica_stderr =~ /\s*ERROR:\s+([0-9A-Z]+):\s*/g);

	# don't need to send error sql to replica nodes
	if ($errorcodes ne "")
	{
		note
		  "Primary error sql!\nerrorcodes:$errorcodes\nerror_msg:$replica_stderr\nsql($line_number):\"$sql\"\n";
		return $ReadWrite;
	}

	foreach my $errcode (@errorcode_array)
	{
		foreach my $readonly_errcode (@read_only_errorcodes)
		{
			if ($errcode eq $readonly_errcode)
			{
				return $ReadWrite;
			}
		}
		foreach my $errcode_msg (@errorcode_message_patterns)
		{
			if ($errcode eq @$errcode_msg[0])
			{
				foreach my $pattern (@{ @$errcode_msg[1] })
				{
					if ($replica_stderr =~ $pattern)
					{
						note
						  "Maybe there's some special readonly error case! "
						  . "errorcode:$errcode, error_pattern:$pattern, error_msg:$replica_stderr\n";
						return $ReadWrite;
					}
				}
			}
		}
		foreach my $error_pattern (@only_ro_error_patterns)
		{
			if ($errorcodes eq "" && $errcode eq @$error_pattern[0])
			{
				foreach my $pattern (@{ @$error_pattern[1] })
				{
					if ($replica_stderr =~ $pattern)
					{
						note "Only RO error case!"
						  . "errorcode:$errcode, error_pattern:$pattern, error_msg:$replica_stderr\n";
						return $ROCommit;
					}
				}
			}
		}
	}

	if (@errorcode_array > 0 && $errorcodes eq "")
	{
		my @temp_objs = $self->fetch_temp_objs($node_primary);
		foreach my $obj (@temp_objs)
		{
			if ($replica_stderr =~ qr/$obj/i)
			{
				note
				  "Maybe there's some temp object $obj in replica_stderr of sql:$replica_stderr\n";
				return $ReadWrite;
			}
		}
		my @psql_vars = $self->fetch_psql_vars($node_primary);
		foreach my $var (@psql_vars)
		{
			if ($sql =~ qr/:$var/i)
			{
				note
				  "Maybe there's some psql var $var in sql($line_number):$sql\n";
				return $ReadWrite;
			}
		}
		my @random_obj_patterns = $self->fetch_random_obj_patterns();
		foreach my $obj_pattern (@random_obj_patterns)
		{
			if ($sql =~ qr/$obj_pattern/i)
			{
				note
				  "Maybe there's some random object $obj_pattern in sql($line_number):$sql\n";
				return $ReadWrite;
			}
		}
		die
		  "PANIC: only RO error:@errorcode_array\nerror_msg:$replica_stderr\ntemp_objs:@temp_objs\n"
		  . "psql_vars:@psql_vars\nrandom_objs:@random_obj_patterns\nsql($line_number):$sql\n";
	}
	return $ReadOnly;
}

my $polar_set_verbosity_pattern = qr/\\set\s+VERBOSITY\s+[a-zA-Z]+\s+/i;

sub prepare_one_case_phase_2
{
	my ($self, $node_replica_ref, $sql_dir, $test_case) = @_;
	my $sql_input = "$sql_dir/$test_case.sql";
	my $sql_backup = "$sql_dir/$test_case.backup.sql";
	my $sql_target = "$sql_dir/$test_case.target.sql";
	my $node_primary = $self->node_primary;
	my $regress_db = $self->regress_db;
	my $test_log_path = $self->test_log_path;
	my ($stderr, $stdout);

	if (scalar(@{$node_replica_ref}) < 1)
	{
		die
		  "($test_case,$$) At least one Primary node and one Replica node to do this work!("
		  . @{$node_replica_ref} . ")\n";
	}

	if (-e $sql_backup)
	{
		note "File $sql_input has backup: $sql_backup";
		return $DC_RES{'SUCC'};
	}

	my $node_replica = @{$node_replica_ref}[0];
	my $table = "replayed_$$";
	$table =~ s/[^a-zA-Z0-9]/\_/g;

	$node_primary->safe_psql($regress_db,
		"CREATE TABLE IF NOT EXISTS $table(val integer unique);");

	my ($primary_stderr, $replica_stderr) = ("", "");
	my ($primary_h, $replica_h);
	# create psql session in single step mode for primary node
	my $primary_temp_file = "$test_log_path/$test_case\_$$\_rw.out";
	my $primary_error_file = "$test_log_path/$test_case\_$$\_rw.stderr";
	$self->build_parent_dir($primary_temp_file);
	open my $primary_stderr_fd, '>', $primary_error_file
	  or die "Failed to open $primary_error_file!";
	$node_primary->psql_connect($regress_db, $global_timeout,
		output_file => $primary_temp_file);
	# create psql session for replica node
	my $replica_temp_file = "$test_log_path/$test_case\_$$\_ro.out";
	my $replica_error_file = "$test_log_path/$test_case\_$$\_ro.stderr";
	$self->build_parent_dir($replica_temp_file);
	open my $replica_stderr_fd, '>', $replica_error_file
	  or die "Failed to open $replica_error_file!";
	$node_replica->psql_connect($regress_db, $global_timeout,
		output_file => $replica_temp_file);
	## fetch all content from original input sql file
	open my $src_sql_fd, '<', $sql_input or die "failed to open $sql_input";
	my @src_sql_array = <$src_sql_fd>;
	close $src_sql_fd;
	my @dst_sql_array;
	my $cur_trans_sql = "";
	my $set_verbosity_sql = "\\set VERBOSITY verbose\n";
	my $exec_trans_sql = $set_verbosity_sql;
	## replay check all
	$self->replay_check_all($node_replica_ref, $table, $test_case);
	for (
		my $src_sql_array_index = 0;
		$src_sql_array_index < scalar(@src_sql_array);
		$src_sql_array_index++)
	{
		## first, fetch one trans sql from src input sql file
		if (   $src_sql_array[$src_sql_array_index] !~ qr/$polar_border/i
			&& $src_sql_array[$src_sql_array_index] !~
			qr/$polar_db_user_change/i)
		{
			$cur_trans_sql .= $src_sql_array[$src_sql_array_index];
			if ($src_sql_array[$src_sql_array_index] !~
				$polar_set_verbosity_pattern)
			{
				$exec_trans_sql .= $src_sql_array[$src_sql_array_index];
			}
			next;
		}

		## second, parse errorcodes if any
		my $errorcodes = "";
		if ($src_sql_array[$src_sql_array_index] =~
			qr/$polar_border:([0-9A-Z,]+)/i)
		{
			$errorcodes = $1;
		}

		# second, send sql to replica node
		my $replica_ret = $self->execute_sql(
			"$test_case,$$", $node_replica,
			$exec_trans_sql, $replica_stderr_fd);

		my $trans_state = $Origin;
		if ($src_sql_array[$src_sql_array_index] !~
			qr/$polar_db_user_change/i)
		{
			$trans_state =
			  $self->is_trans_readonly($errorcodes, $replica_ret->{_stderr},
				$node_primary, $regress_db,
				$exec_trans_sql, $src_sql_array_index);
			if ($trans_state eq $ROCommit)
			{
				$self->execute_sql(
					"$test_case,$$", $node_replica,
					"commit;", $replica_stderr_fd);
			}
		}

		# third, send sql to primary node
		my $primary_ret = $self->execute_sql(
			"$test_case,$$", $node_primary,
			$exec_trans_sql, $primary_stderr_fd);

		# fourth, write sql to target file and mark trans sql readonly or writable
		push @dst_sql_array, $cur_trans_sql;
		if ($trans_state eq $ReadOnly)
		{
			die "primary error sql should be ignored by replica nodes!"
			  if $errorcodes ne "";
			push @dst_sql_array, "$polar_border_ro\n";
		}
		elsif ($trans_state eq $ReadWrite || $trans_state eq $ROCommit)
		{
			my $rw_border = $polar_border_rw;
			$rw_border .= ":$errorcodes" if $errorcodes ne "";
			push @dst_sql_array, "$rw_border\n";
			push @dst_sql_array, "$polar_only_ro_commit\n"
			  if ($trans_state eq $ROCommit);
			# replay check all
			$self->replay_check_all($node_replica_ref, $table, $test_case);
		}
		elsif ($trans_state eq $Origin)
		{
			push @dst_sql_array, "$polar_db_user_change\n";
		}
		else
		{
			die "Wrong result: $trans_state!\n";
		}
		$cur_trans_sql = "";
		$exec_trans_sql = $set_verbosity_sql;
	}

	close $primary_stderr_fd;
	close $replica_stderr_fd;
	## close psql session
	$node_primary->psql_close;
	$node_replica->psql_close;

	die "Unexpected result for $sql_input during phase two!\n"
	  if (@dst_sql_array <= 0);
	# make writable or readonly sql block together.
	my $prev_sql_border_pos = -1;
	my $index = @dst_sql_array - 1;
	if ($self->is_sql_border($dst_sql_array[$index]) eq 0)
	{
		die "##($test_case,$$) Unexpected end: $dst_sql_array[$index]";
	}
	while ($index >= 0)
	{
		if ($self->is_sql_border($dst_sql_array[$index]) eq 1)
		{
			if ($prev_sql_border_pos >= 0)
			{
				my ($cur_border, $cur_errcode) =
				  split(':|\n', $dst_sql_array[$index]);
				my ($prev_border, $prev_errcode) =
				  split(':|\n', $dst_sql_array[$prev_sql_border_pos]);
				if ($cur_border eq $prev_border)
				{
					if ((defined $cur_errcode) && $cur_errcode ne "")
					{
						$prev_errcode = "" if (!defined $prev_errcode);
						$self->string_append(\$prev_errcode, $cur_errcode,
							',');
						$dst_sql_array[$prev_sql_border_pos] =
						  "$prev_border:$prev_errcode\n";
					}
					$dst_sql_array[$index] = undef;
				}
				else
				{
					$prev_sql_border_pos = $index;
				}
			}
			else
			{
				$prev_sql_border_pos = $index;
			}
		}
		$index--;
	}

	## write final $dst_sql_array into $sql_target
	open my $dst_sql_fd, '>', $sql_target or die "failed to open $sql_target";
	foreach my $line (@dst_sql_array)
	{
		if (defined $line)
		{
			print $dst_sql_fd $line;
		}
	}
	close $dst_sql_fd;

	## restore result
	PostgreSQL::Test::Utils::system_or_bail('mv', $sql_input, $sql_backup);
	PostgreSQL::Test::Utils::system_or_bail('mv', $sql_target, $sql_input);
	return $DC_RES{'SUCC'};
}

sub create_new_test
{
	my $class = 'DCRegression';
	$class = shift if 1 < scalar @_;
	my $node_primary = shift;

	my $test = $class->new($node_primary);
	$ENV{''} = 'parent';
	return $test;
}
1;
