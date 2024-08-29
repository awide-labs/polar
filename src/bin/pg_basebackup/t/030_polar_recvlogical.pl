# 030_polar_recvlogical.pl
#
# Copyright (c) 2024, Alibaba Group Holding Limited
# Copyright (c) 2021, PostgreSQL Global Development Group
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
#	  src/bin/pg_basebackup/t/030_polar_recvlogical.pl
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

program_help_ok('pg_recvlogical');
program_version_ok('pg_recvlogical');
program_options_handling_ok('pg_recvlogical');

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->polar_init_primary;

$node->append_conf(
	'postgresql.conf', q{
wal_level = 'logical'
max_replication_slots = 4
max_wal_senders = 4
log_min_messages = 'debug1'
log_error_verbosity = verbose
});
$node->dump_info;
$node->start;
$node->command_fails(['pg_recvlogical'], 'pg_recvlogical needs a slot name');
$node->command_fails([ 'pg_recvlogical', '-S', 'test' ],
	'pg_recvlogical needs a database');
$node->command_fails([ 'pg_recvlogical', '-S', 'test', '-d', 'postgres' ],
	'pg_recvlogical needs an action');
$node->command_fails(
	[
		'pg_recvlogical', '-S',
		'test', '-d',
		$node->connstr('postgres'), '--start'
	],
	'no destination file');
$node->command_ok(
	[
		'pg_recvlogical', '-S',
		'test', '-d',
		$node->connstr('postgres'), '--create-slot'
	],
	'slot created');
my $slot = $node->slot('test');
isnt($slot->{'restart_lsn'}, '', 'restart lsn is defined for new slot');
$node->psql('postgres', 'CREATE TABLE test_table(x integer)');
$node->psql('postgres',
	'INSERT INTO test_table(x) SELECT y FROM generate_series(1, 10000) a(y);'
);
my $nextlsn =
  $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
chomp($nextlsn);
$node->command_ok(
	[
		'pg_recvlogical', '-S', 'test', '-d', $node->connstr('postgres'),
		'--start', '--endpos', "$nextlsn", '--no-loop', '-f', '-'
	],
	'replayed a transaction');
$node->stop;

done_testing();
