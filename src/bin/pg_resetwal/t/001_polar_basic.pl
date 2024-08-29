# 001_polar_basic.pl
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
#	  src/bin/pg_resetwal/t/001_polar_basic.pl

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan tests => 26;

program_help_ok('pg_resetwal');
program_version_ok('pg_resetwal');
program_options_handling_ok('pg_resetwal');

# create primary
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;
# create replica1
my $node_replica1 = PostgreSQL::Test::Cluster->new('replica1');
$node_replica1->polar_init_replica($node_primary);
# create standby1
my $node_standby1 = PostgreSQL::Test::Cluster->new('standby1');
$node_standby1->polar_init_standby($node_primary);
# primary config
$node_primary->append_conf('postgresql.conf', 'wal_sender_timeout=3600s');
# replica1 config
$node_replica1->append_conf('postgresql.conf', 'wal_receiver_timeout=3600s');
# standby1 config
$node_standby1->append_conf('postgresql.conf', 'wal_receiver_timeout=3600s');
# start primary
$node_primary->start;
# create slots needed by replica1 and standby1
$node_primary->polar_create_slot($node_replica1->name);
$node_primary->polar_create_slot($node_standby1->name);
# start replica1
$node_replica1->start;
# start standby1
$node_standby1->start;
$node_standby1->polar_drop_all_slots;

$node_primary->stop;
$node_replica1->stop;
$node_standby1->stop;

command_like([ 'pg_resetwal', '-n', $node_primary->data_dir ],
	qr/checkpoint/, 'pg_resetwal -n produces output');
command_like([ 'pg_resetwal', '-n', $node_replica1->data_dir ],
	qr/checkpoint/, 'pg_resetwal -n produces output');
command_like([ 'pg_resetwal', '-n', $node_standby1->data_dir ],
	qr/checkpoint/, 'pg_resetwal -n produces output');

# Permissions on PGDATA should be default
SKIP:
{
	skip "unix-style permissions not supported on Windows", 1
	  if ($windows_os);

	ok(check_mode_recursive($node_primary->data_dir, 0700, 0600),
		'check PGDATA permissions');
	ok(check_mode_recursive($node_replica1->data_dir, 0700, 0600),
		'check PGDATA permissions');
	ok(check_mode_recursive($node_standby1->data_dir, 0700, 0600),
		'check PGDATA permissions');
	ok(check_mode_recursive($node_primary->polar_get_datadir, 0700, 0600),
		'check PGDATA permissions');
	ok(check_mode_recursive($node_replica1->polar_get_datadir, 0700, 0600),
		'check PGDATA permissions');
	ok(check_mode_recursive($node_standby1->polar_get_datadir, 0700, 0600),
		'check PGDATA permissions');
}

command_like(
	[ 'pg_resetwal', $node_primary->data_dir ],
	qr/Write-ahead log reset/,
	'pg_resetwal -n produces output');

$node_primary->start;
# The controlfile at replica node may be already invalid.
system('rm -f ' . $node_replica1->data_dir . '/polar_replica_booted');
$node_replica1->start;

$node_primary->safe_psql("postgres",
	"create table t(t1 int primary key, t2 int);insert into t values (1, 1),(2, 3),(3, 3);select * from t;"
);
$node_replica1->safe_psql("postgres", "select pg_sleep(1);select * from t;");

$node_primary->stop;
$node_replica1->stop;
