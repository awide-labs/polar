#!/usr/bin/perl

# 004_rename_wal_ready_file.pl
#	  Test PolarDB rename wal.ready file
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
#	  src/test/polar_pl/t/004_rename_wal_ready_file.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More tests => 9;

my $regress_db = 'postgres';

# Test rename wal.ready file to wal.done
sub test_rename_wal_ready
{
	my $node = shift;
	my $wal_name = shift;
	my $expected_res = shift;

	my $res = $node->psql($regress_db,
		"set polar_rename_wal_ready_file = '$wal_name'");
	is($res, $expected_res,
		"rename wal ready file $wal_name for " . $node->name);
}

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;

my $node_replica = PostgreSQL::Test::Cluster->new('replica');
$node_replica->polar_init_replica($node_primary);

$node_primary->append_conf('postgresql.conf', 'archive_mode=on');
$node_replica->append_conf('postgresql.conf', 'archive_mode=on');

$node_primary->start;
$node_primary->polar_create_slot($node_replica->name);
$node_replica->start;

my $wal_name_exists = $node_primary->safe_psql($regress_db,
	'SELECT pg_walfile_name(pg_switch_wal())');
$node_primary->safe_psql($regress_db,
	"CREATE USER polar_su PASSWORD 'polar_pwd' superuser;");

# Start tests.
test_rename_wal_ready($node_replica, $wal_name_exists, 3);
test_rename_wal_ready($node_primary, $wal_name_exists, 0);
test_rename_wal_ready($node_primary, $wal_name_exists, 3);    # do it again
test_rename_wal_ready($node_primary, '.', 3);
test_rename_wal_ready($node_primary, '/', 3);
test_rename_wal_ready($node_primary, ' ', 3);
test_rename_wal_ready($node_primary, '', 0);
test_rename_wal_ready($node_primary, '000000011111111111111111', 3);
$ENV{"PGUSER"} = "polar_su";
$ENV{"PGPASSWORD"} = "polar_pwd";
test_rename_wal_ready($node_primary, "$wal_name_exists", '3');
# Finish tests.

$node_primary->stop;
$node_replica->stop;
