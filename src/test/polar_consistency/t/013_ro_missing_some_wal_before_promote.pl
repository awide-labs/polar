# 013_ro_missing_some_wal_before_promote.pl
#	  Test promote replica when it hasn't read all the WAL.
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
#	  src/test/polar_consistency/t/013_ro_missing_some_wal_before_promote.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_fault_injector} eq 'no')
{
	plan skip_all => 'Fault injector not supported by this build';
}

my $db = 'postgres';

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;
$node_primary->append_conf('postgresql.conf', 'synchronous_commit=on');

my $node_replica = PostgreSQL::Test::Cluster->new('replica1');
$node_replica->polar_init_replica($node_primary);
$node_replica->append_conf('postgresql.conf', 'logging_collector=off');

$node_primary->start;

$node_primary->safe_psql($db, 'create extension faultinjector;');
$node_primary->safe_psql($db, 'create table test(id int);');
$node_primary->safe_psql($db, 'insert into test values(1);');

$node_replica->start;

# use fault injector to force make lazy checkpoint after promote
my $result = $node_replica->safe_psql($db,
	"select inject_fault('test_wait_for_promote_signal', 'enable', '', '', 1, -1, -1);"
);
print "enable fault inject: $result\n";
$result = $node_replica->safe_psql($db,
	"select inject_fault('test_wait_for_promote_signal', 'status');");
print "fault inject status is $result\n";

my $pid = $node_replica->find_child('startup');
BAIL_OUT('startup process is gone!') if $pid eq 0;

my ($time, $out, $err);
for ($time = 0; $time < 60; $time = $time + 1)
{
	($out, $err) = $node_replica->run_command([ 'pstack', $pid ]);
	note "stdout: $out\nstderr: $err\n";
	# make sure that target process is stopped at right stack
	if ($out =~ /pg_usleep/)
	{
		note "startup process ($pid)'s stack is expected.\n";
		last;
	}
	sleep 1;
}

BAIL_OUT("start process ($pid)'s stack is unexpected.")
  if ($out !~ /pg_usleep/);

$node_primary->safe_psql($db, 'insert into test values(2);');
$node_primary->stop;

$node_replica->promote_async;

# wait for replica to shutdown
for ($time = 0; $time < 60; $time = $time + 1)
{
	if (kill(0, $node_replica->{_pid}) != 0)
	{
		sleep 1;
	}
	else
	{
		last;
	}
}

if (kill(0, $node_replica->{_pid}) != 0)
{
	BAIL_OUT("replica does not shutdown in $time seconds\n");
}
else
{
	print "replica shutdown after $time seconds\n";
}

my $panic_log = qr/is not the last valid record's EndRecPtr/;
my $log = PostgreSQL::Test::Utils::slurp_file($node_replica->logfile, 0);
like($log, $panic_log, "found panic log");

$node_replica->_update_pid(-1);
$node_replica->start;
# wait for replica to do crash recovery
$node_replica->polar_wait_for_startup(30);

$node_replica->promote;
# wait for new primary to startup
$node_replica->polar_wait_for_startup(30);

$result = $node_replica->safe_psql($db, "select count(*) from test;");
chomp($result);
ok($result == 2, "Check data consistency for promoted replica");

$node_replica->stop;
done_testing();
