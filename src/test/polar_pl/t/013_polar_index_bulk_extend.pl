# 013_polar_index_bulk_extend.pl
#	  polar index insert bulk extend test
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
#	  src/test/polar_pl/t/013_polar_index_bulk_extend.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan tests => 2;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->polar_init_primary;
$node->start;

# Set the min bulk extend table size to 0, so the index bulk
# extend always hits.
$node->safe_psql('postgres',
	'alter system set polar_min_bulk_extend_table_size = 0;');

# Set the index bulk extend size to 256 (2MB), the index
# size will larger than 2MB.
$node->safe_psql('postgres',
	'alter system set polar_index_bulk_extend_size = 256;');
$node->reload;

$node->safe_psql(
	'postgres',
	q[create table test_index_bulk_extend(test1 int);
		create index test_index on test_index_bulk_extend(test1);]);

$node->safe_psql('postgres',
	q[insert into test_index_bulk_extend values(1);]);

# 2 * 1024 * 1024 = 2097152 = 2MB
is( $node->safe_psql(
		'postgres',
		"select pg_indexes_size('test_index_bulk_extend') > 2097152;"),
	't',
	'index bulk extend 2MB');

$node->safe_psql('postgres', q[truncate test_index_bulk_extend;]);

# Set the index bulk extend size to 512 (4MB), the index
# size will larger than 4MB.
$node->safe_psql('postgres',
	'alter system set polar_index_bulk_extend_size = 512;');
$node->reload;

$node->safe_psql('postgres',
	q[insert into test_index_bulk_extend values(1);]);

# 4 * 1024 * 1024 = 4194304 = 4MB
is( $node->safe_psql(
		'postgres',
		"select pg_indexes_size('test_index_bulk_extend') > 4194304;"),
	't',
	'index bulk extend 4MB');

$node->stop;
