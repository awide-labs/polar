#!/usr/bin/perl

# 002_polar_max_connections.pl
#	  Testing for show hook of max_connections.
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
#	  src/test/polar_pl/t/002_polar_max_connections.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $role_super = 'regress_super';
my $role_non_super = 'regress_non_super';

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);
$node_primary->start;
$node_primary->safe_psql('postgres',
	'CREATE ROLE ' . $role_super . ' LOGIN SUPERUSER');
$node_primary->safe_psql('postgres',
	'CREATE ROLE ' . $role_non_super . ' LOGIN');
$node_primary->stop('fast');

$node_primary->append_conf('postgresql.conf', 'max_connections = 20');
$node_primary->append_conf('postgresql.conf',
	'polar_max_non_super_conns = -1');
$node_primary->start;

my $result = $node_primary->safe_psql(
	'postgres',
	'SHOW max_connections',
	extra_params => [ '-U', $role_super ]);
is($result, '20', 'should be max_connections');
$result = $node_primary->safe_psql(
	'postgres',
	'SHOW max_connections',
	extra_params => [ '-U', $role_non_super ]);
is($result, '20', 'should be max_connections');

$node_primary->stop('fast');
$node_primary->append_conf('postgresql.conf',
	'polar_max_non_super_conns = 10');
$node_primary->start;

$result = $node_primary->safe_psql(
	'postgres',
	'SHOW max_connections',
	extra_params => [ '-U', $role_super ]);
is($result, '20', 'should be max_connections');
$result = $node_primary->safe_psql(
	'postgres',
	'SHOW max_connections',
	extra_params => [ '-U', $role_non_super ]);
is($result, '10', 'should be polar_max_non_super_conns');

$node_primary->stop('fast');
$node_primary->append_conf('postgresql.conf',
	'polar_max_non_super_conns = 40');
$node_primary->start;

$result = $node_primary->safe_psql(
	'postgres',
	'SHOW max_connections',
	extra_params => [ '-U', $role_super ]);
is($result, '20', 'should be max_connections');
$result = $node_primary->safe_psql(
	'postgres',
	'SHOW max_connections',
	extra_params => [ '-U', $role_non_super ]);
is($result, '20', 'should be max_connections');

# done with the node
$node_primary->stop;

done_testing();
