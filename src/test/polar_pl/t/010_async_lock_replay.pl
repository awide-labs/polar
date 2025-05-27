#!/usr/bin/perl

# 010_async_lock_replay.pl
#	  Test PolarDB async lock replay in replica.
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
#	  src/test/polar_pl/t/010_async_lock_replay.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use PolarDB::Task;
use Test::More;

my $inf = 999999;
my $regress_db = "postgres";
my $lock_t1 = 'begin; truncate t1; end;';
my $lock_t2 = 'begin; truncate t2; end;';
my $lock_t1t2 = 'begin; truncate t1; truncate t2; end;';
my ($ret, $stdout, $stderr);

my $node_primary = PostgreSQL::Test::Cluster->new("primary");
$node_primary->polar_init_primary();

my $node_replica1 = PostgreSQL::Test::Cluster->new("replica1");
$node_replica1->polar_init_replica($node_primary);
$node_replica1->append_conf('postgresql.conf',
	"max_standby_streaming_delay = 1s");
$node_replica1->append_conf('postgresql.conf',
	"max_standby_archive_delay = 1s");

my $node_replica2 = PostgreSQL::Test::Cluster->new("replica2");
$node_replica2->polar_init_replica($node_primary);
$node_replica2->append_conf('postgresql.conf',
	"max_standby_streaming_delay = 2s");
$node_replica2->append_conf('postgresql.conf',
	"max_standby_archive_delay = 2s");

$node_primary->start;
$node_primary->polar_create_slot($node_replica1->name);
$node_primary->polar_create_slot($node_replica2->name);

$node_replica1->start;
$node_replica2->start;

$node_primary->safe_psql($regress_db, 'create extension polar_monitor');
$node_primary->safe_psql($regress_db, 'create table t1(id int)');
$node_primary->safe_psql($regress_db, 'create table t2(id int)');
$node_primary->wait_for_catchup($node_replica1, 'replay',
	$node_primary->lsn('insert'));
$node_primary->wait_for_catchup($node_replica2, 'replay',
	$node_primary->lsn('insert'));

Task::start_new_task(
	'slock t1',
	[$node_replica1],
	\&PostgreSQL::Test::Cluster::psql,
	[ $regress_db, "select * from t1, pg_sleep($inf)" ],
	Task::EXEC_UNTIL_FAIL,
	$inf);
sleep 1;
is($node_primary->psql($regress_db, $lock_t1), 0, "xlock t1");
Task::wait_all_task;

Task::start_new_task(
	'slock t1',
	[ $node_replica1, $node_replica2 ],
	\&PostgreSQL::Test::Cluster::psql,
	[ $regress_db, "select * from t1, pg_sleep($inf)" ],
	Task::EXEC_UNTIL_FAIL,
	$inf);
sleep 1;
is($node_primary->psql($regress_db, $lock_t1),
	0, "xlock t1 wait for two replica");
Task::wait_all_task;

Task::start_new_task(
	'slock t1',
	[$node_replica1],
	\&PostgreSQL::Test::Cluster::psql,
	[ $regress_db, "select * from t1, pg_sleep($inf)" ],
	Task::EXEC_UNTIL_FAIL,
	$inf);
Task::start_new_task(
	'slock t2',
	[$node_replica2],
	\&PostgreSQL::Test::Cluster::psql,
	[ $regress_db, "select * from t2, pg_sleep($inf)" ],
	Task::EXEC_UNTIL_FAIL,
	$inf);
sleep 1;
is($node_primary->psql($regress_db, $lock_t1t2),
	0, "xlock t1 t2 wait for two replica");
Task::wait_all_task;

Task::start_new_task(
	'slock t1',
	[$node_replica1],
	\&PostgreSQL::Test::Cluster::psql,
	[ $regress_db, "select * from t1, pg_sleep($inf)" ],
	Task::EXEC_UNTIL_FAIL,
	$inf);
sleep 1;
$node_primary->psql($regress_db,
	"begin; truncate t1; insert into t1 select 1; end;");
Task::wait_all_task;
is($node_replica1->safe_psql($regress_db, "select count(*) from t1"),
	1, "xlock t1 and insert success");

$node_primary->stop;
$node_replica1->stop;
$node_replica2->stop;

done_testing();
