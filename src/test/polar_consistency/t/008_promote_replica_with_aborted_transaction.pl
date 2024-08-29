# 008_promote_replica_with_aborted_transaction.pl
#
# Copyright (c) 2023, Alibaba Group Holding Limited
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
#         src/test/polar_consistency/t/008_promote_replica_with_aborted_transaction.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $sql_dir = $ENV{PWD} . '/sql';
my $regress_db = 'postgres';

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;

my $node_replica1 = PostgreSQL::Test::Cluster->new('replica1');
$node_replica1->polar_init_replica($node_primary);

$node_primary->append_conf('postgresql.conf', 'synchronous_commit=on');
$node_replica1->append_conf('postgresql.conf', 'synchronous_commit=on');
$node_replica1->append_conf('postgresql.conf', 'logging_collector=on');

$node_primary->start;
$node_primary->polar_create_slot($node_replica1->name);
$node_replica1->start;

my $start_time = time();
my $timeout = 60;
$node_primary->psql_connect($regress_db, $timeout);
# begin an transaction
my $primary_output = $node_primary->psql_execute("begin;");
if ($primary_output->{_ret} != 0)
{
	die "# Unexpected ret with primary node! stderr is: "
	  . $primary_output->{_stderr} . "\n";
}

# sigstop checkpoint: No more checkpoint
$node_primary->stop_child("checkpointer");

# create one table and insert some data
my $data_sql = "
	create table test (id int, name char(128) default 'A');
	insert into test select generate_series(1,64);
";

$primary_output = $node_primary->psql_execute($data_sql);
if ($primary_output->{_ret} != 0)
{
	die "# Unexpected ret with primary node! stderr is: "
	  . $primary_output->{_stderr} . "\n";
}

# do a successful commit transaction to flush all the above wal records
$node_primary->safe_psql(
	$regress_db,
	"create table temp_test (id int);",
	timeout => $timeout);

# sigstop walwriter: do not flush the following abort record
$node_primary->stop_child("walwriter");

# abort the above transaction
$node_primary->psql_execute("abort;");

# close connection
$node_primary->psql_close;

# immediate shutdown primary: do not flush the above abort record
$node_primary->stop('i');

# promote replica: according to commit c5e21f046328,
# replica node will start to parallel replay at the last checkpoint's
# redo lsn during promoting. It makes sure that the start lsn
# of replaying wal is lower than the wal which creating table 'test'.
$node_replica1->promote();
$node_replica1->polar_wait_for_startup(60);

# find anything for logfile
my $data_dir = $node_replica1->data_dir();
my $grep_cmd =
  "grep -wnr \"polar assertion failed\" --include=*.log $data_dir";
my $grep_result = qx{$grep_cmd};
my $err_code = $?;
print "command: $grep_cmd,\nerror: $err_code,\nresult: $grep_result\n";
ok($grep_result eq "" && $err_code != 0, "POLAR_ASSERT_PANIC passed!");
# find any corefile
my $find_cmd = "find $data_dir -name core*";
my $find_result = qx{$find_cmd};
$err_code = $?;
print "command: $find_cmd,\nerror: $err_code,\nresult: $find_result\n";
ok($find_result eq "" && $err_code == 0, "No core file passed!");

$node_replica1->polar_wait_for_startup(30);
my $cost_time = time() - $start_time;
note "Test cost time $cost_time";

$node_replica1->polar_drop_all_slots;
$node_replica1->stop();
done_testing();
