#!/usr/bin/perl
# 009_polar_flush_nonlatest_buffers.pl
#	  Tests data consistency after flush nonlatest buffers
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
#	  src/test/polar_consistency/t/009_polar_flush_nonlatest_buffers.pl

use strict;
use warnings;
use Config;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use PolarDB::DCRegression;

if ($ENV{enable_fault_injector} eq 'no')
{
	plan skip_all => 'Fault injector not supported by this build';
}

$ENV{PG_TEST_TIMEOUT_DEFAULT} = 20;

############### init primary/standby ##############
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;

my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->polar_init_standby($node_primary);

$node_primary->start;
$node_primary->polar_create_slot($node_standby->name);

$node_standby->start;

my $res =
  $node_primary->safe_psql('postgres', "create extension faultinjector;");
print "create faultinjector res: $res\n";

############### test standby ##############
$node_primary->safe_psql('postgres', q[CREATE DATABASE test_db_1;]);
$node_primary->safe_psql('test_db_1', q[CREATE TABLE test_rel(id int);]);

my $regress = DCRegression->create_new_test($node_primary);
ok( $regress->replay_check($node_standby),
	"replay check succ for " . $node_standby->name);

# tell standby's logindex bg writer to skip replay
$res = $node_standby->safe_psql('postgres',
	"select inject_fault('polar_logindex_bg_skip_replay', 'enable', '', '', 1, -1, -1)"
);
print "enable logindex bg skip replay fault inject:$res\n";

###### file copy ######
# primary insert new data and get insert lsn
$node_primary->safe_psql('test_db_1', q[INSERT INTO test_rel VALUES(1);]);
$node_primary->safe_psql('postgres',
	q[CREATE DATABASE test_db_2 TEMPLATE test_db_1 STRATEGY = file_copy;]);
my $insert_lsn = $node_primary->lsn('insert');
$node_primary->safe_psql('test_db_1', q[INSERT INTO test_rel VALUES(2);]);

# wait standby to catch up, die after timeout
$node_primary->wait_for_catchup($node_standby, 'replay', $insert_lsn);

# check data consistency
my $primary_count =
  $node_primary->safe_psql('test_db_2', q[SELECT count(*) from test_rel;]);
my $standby_count =
  $node_standby->safe_psql('test_db_2', q[SELECT count(*) from test_rel;]);

ok( $primary_count eq $standby_count,
	"data consistency check succeed after CREATEDB with file_copy: $primary_count vs $standby_count"
);

###### wal log ######
# primary insert new data and get insert lsn
$node_primary->safe_psql('test_db_1', q[INSERT INTO test_rel VALUES(1);]);
$node_primary->safe_psql('postgres',
	q[CREATE DATABASE test_db_3 TEMPLATE test_db_1 STRATEGY = wal_log;]);
$insert_lsn = $node_primary->lsn('insert');
$node_primary->safe_psql('test_db_1', q[INSERT INTO test_rel VALUES(3);]);

# wait standby to catch up, die after timeout
$node_primary->wait_for_catchup($node_standby, 'replay', $insert_lsn);

# check data consistency
$primary_count =
  $node_primary->safe_psql('test_db_3', q[SELECT count(*) from test_rel;]);
$standby_count =
  $node_standby->safe_psql('test_db_3', q[SELECT count(*) from test_rel;]);

ok( $primary_count eq $standby_count,
	"data consistency check succeed after CREATEDB with wal_copy: $primary_count vs $standby_count"
);

# tell standby's logindex bg writer to continue replaying
$res = $node_standby->safe_psql('postgres',
	"select inject_fault('polar_logindex_bg_skip_replay', 'reset')");
print "reset logindex bg skip replay fault inject:$res\n";

############### stop ##############
$node_primary->stop;
$node_standby->stop;
done_testing();
