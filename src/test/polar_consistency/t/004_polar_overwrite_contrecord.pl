# 004_polar_overwrite_contrecord.pl
#	  Tests for fake broken wal.
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
#	  src/test/polar_consistency/t/004_polar_overwrite_contrecord.pl
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IO::Pipe;

if ($ENV{enable_fault_injector} eq 'no')
{
	plan skip_all => 'Fault injector not supported by this build';
}

my $db = 'postgres';
my $timeout = 1000;

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;

my $node_replica = PostgreSQL::Test::Cluster->new('replica1');
$node_replica->polar_init_replica($node_primary);

$node_primary->start;

$node_primary->polar_create_slot($node_replica->name);
$node_replica->start;

my $result = $node_primary->safe_psql('postgres',
	"SELECT pg_logical_emit_message(true, 'test 025', repeat('xyzxz', 123456));"
);
print "result is $result\n";
$node_primary->safe_psql('postgres', 'create extension faultinjector;');
$result = $node_primary->safe_psql('postgres',
	"select inject_fault('fault_inject_for_first_contrecord', 'enable', '', '', 1, -1, -1);"
);
print "enable fault inject: $result\n";
$result = $node_primary->safe_psql('postgres',
	"select inject_fault('fault_inject_for_first_contrecord', 'status');");
print "fault inject status is $result\n";

my $pip = IO::Pipe->new;
my $pid = fork();
if ($pid == 0)
{
	$pip->writer;
	print "Child process($$) was forked.\n";
	$result = $node_primary->safe_psql('postgres',
		"SELECT pg_logical_emit_message(true, 'test 026', repeat('xyzxz', 123456));"
	);
	print $pip $result;
	exit;
}

$pip->reader;

# waiting for child process to execute sql
sleep 5;

$node_replica->stop;
$node_replica->start;

# reset fault inject
$result = $node_primary->safe_psql('postgres',
	"select inject_fault('fault_inject_for_first_contrecord', 'reset');");
print "reset fault inject: $result\n";

# waiting for child process to finish
sleep 5;

$result = "";
while (my $line = <$pip>)
{
	chomp $line;
	$result .= $line;
}
print "Child process says: $result\n";

$node_primary->safe_psql('postgres',
	"SELECT pg_logical_emit_message(true, 'test 027', repeat('xyzxz', 123456));"
);
# make sure that all wals have been flushed
$result = $node_primary->safe_psql('postgres', 'checkpoint;');
my $old_primary_lsn =
  $node_primary->safe_psql('postgres', 'select pg_current_wal_insert_lsn();');
print "old primary's current insert lsn is $old_primary_lsn\n";
# shutdown primary node immediately
$node_primary->stop('i');
# promote replica node
$node_replica->promote;
# wait for new primary to startup
$node_replica->polar_wait_for_startup(30);
$node_replica->polar_drop_all_slots;
my $new_primary_lsn =
  $node_replica->safe_psql('postgres', 'select pg_current_wal_insert_lsn();');
print "new primary's current insert lsn is $new_primary_lsn\n";
$result = $node_replica->safe_psql('postgres',
	"select '$old_primary_lsn'::pg_lsn <= '$new_primary_lsn'::pg_lsn;");
is($result, 't',
	"New primary's insert_lsn is bigger than old primary's insert_lsn.");
$node_replica->stop;
done_testing();
