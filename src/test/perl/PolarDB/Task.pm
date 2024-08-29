# Task.pm
#	  A task manager for test.
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
#	  src/test/perl/PolarDB/Task.pm

package Task;

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use Test::More;
use Exporter 'import';
use POSIX ':sys_wait_h';
use feature qw(switch);
no if $] >= 5.018, warnings => qw( experimental::smartmatch );

# EXEC_UNTIL_FAIL, tests safety
# EXEC_UNTIL_SUCC + timeout(>0), tests liveness
use constant {
	EXEC_ONCE => 0,
	EXEC_FOREVER => 1,
	EXEC_UNTIL_FAIL => 2,
	EXEC_UNTIL_SUCC => 3,
};

our (@all_tasks);

our @EXPORT =
  qw(EXEC_ONCE EXEC_FOREVER EXEC_UNTIL_FAIL EXEC_UNTIL_SUCC start_new_task wait_all_task);

sub start_new_task
{
	my ($name, $nodes, $operation, $parameters, $strategy, $timeout) = @_;
	my $class = 'Task';
	my $task = {
		_name => $name,
		_status => 'init',
		_nodes => $nodes,
		_operation => $operation,
		_parameters => $parameters,
		_strategy => $strategy,
		_timeout => $timeout,
		_pids => [],
		_start_time => undef,
	};
	bless $task, $class;
	push(@all_tasks, $task);
	$task->start;
	return $task;
}

sub start
{
	my ($self) = @_;
	$self->{_start_time} = time();
	foreach my $node (@{ $self->{_nodes} })
	{
		$self->start_one_job($node);
	}
	$self->{_status} = 'running';
}

sub start_one_job
{
	my ($self, $node) = @_;
	my $pid = fork();
	if ($pid == 0)
	{
		given ($self->{_strategy})
		{
			when (EXEC_ONCE)
			{
				exit $self->{_operation}->($node, @{ $self->{_parameters} });
			}
			when (EXEC_FOREVER)
			{
				while (1)
				{
					$self->{_operation}->($node, @{ $self->{_parameters} });
					sleep(1);
				}
				exit -1;
			}
			when (EXEC_UNTIL_FAIL)
			{
				while ($self->{_operation}->($node, @{ $self->{_parameters} })
					== 0)
				{
					sleep(1);
				}
				exit 0;
			}
			when (EXEC_UNTIL_SUCC)
			{
				while ($self->{_operation}->($node, @{ $self->{_parameters} })
					!= 0)
				{
					sleep(1);
				}
				exit 0;
			}
			default
			{
				BAIL_OUT("Unknown execution strategy $self->{_strategy}");
			}
		}
		exit $self->{_operation}->($node, @{ $self->{_parameters} });
	}
	elsif ($pid > 0)
	{
		print("--->Created child process($pid)\n");
		push @{ $self->{_pids} }, $pid;
	}
}

sub check
{
	my ($self) = @_;
	my $ndone = 0;
	my $now = time();
	my $diff = $now - $self->{_start_time};
	if ($self->{_timeout} != 0 && $diff > abs($self->{_timeout}))
	{
		$self->{_status} = 'done';
		return $self->{_timeout} > 0 ? 'timeout' : 'done';
	}
	foreach my $pid (@{ $self->{_pids} })
	{
		my $ret = waitpid($pid, WNOHANG);
		my $code = $?;
		if ($ret == $pid)
		{
			print(
				"--->Child process($pid) is exited, return code is $code\n");
			@{ $self->{_pids} } = grep(!/$pid/, @{ $self->{_pids} });
			$code != 0 or next;
			$self->{_status} = 'done';
			return 'failed';
		}
	}
	if (@{ $self->{_pids} } == 0)
	{
		$self->{_status} = 'done';
		return 'done';
	}
	return 'running';
}

sub kill_all_task()
{
	foreach my $task (@all_tasks)
	{
		foreach my $pid (@{ $task->{_pids} })
		{
			kill(9, $pid);
		}
	}
	@all_tasks = ();
}

sub wait_all_task()
{
	my $ndone = 0;
	while (1)
	{
		foreach my $task (@all_tasks)
		{
			if ($task->{_status} eq "done")
			{
				next;
			}
			my $result = $task->check();
			if ($result eq "done")
			{
				print("--->Task($task->{_name}) is done\n");
				$ndone++;
				next;
			}
			if ($result eq "timeout" || $result eq "failed")
			{
				kill_all_task();
				BAIL_OUT(
					"Task execution failed for task '$task->{_name}' $result"
				);
			}
		}
		if ($ndone eq @all_tasks)
		{
			kill_all_task();
			print("--->All task is done\n");
			return 0;
		}
		sleep(1);
	}
}

1;
