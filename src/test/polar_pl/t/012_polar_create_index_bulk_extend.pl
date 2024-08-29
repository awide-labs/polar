use strict;
# 012_polar_create_index_bulk_extend.pl
#	  create index bulk extend test
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
#	  src/test/polar_pl/t/012_polar_create_index_bulk_extend.pl

use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan tests => 2;

# Start Server
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;
$node_primary->start;

# Create extension polar_monitor
$node_primary->safe_psql('postgres',
	'CREATE EXTENSION IF NOT EXISTS polar_monitor;');


# Create table
$node_primary->safe_psql('postgres',
	q[create table bulk_extend_tbl(id int8, value int8);]);


# Close the feature
$node_primary->safe_psql('postgres',
	'alter system set polar_index_create_bulk_extend_size = 0;');
$node_primary->safe_psql('postgres',
	'alter system set polar_min_bulk_extend_table_size = 0;');
$node_primary->reload;

# Load Data
$node_primary->safe_psql('postgres',
	q[INSERT INTO bulk_extend_tbl select generate_series,generate_series from generate_series(0, 12800*185 + 184);]
);

# Create index
$node_primary->safe_psql('postgres',
	q[CREATE INDEX bulk_extend_idx on bulk_extend_tbl(id);]);

is( $node_primary->safe_psql(
		'postgres',
		'select idx_create_extend_times = 0 from polar_pg_stat_all_index_extend_stats where relname=\'bulk_extend_tbl\';'
	),
	't',
	'idx_create_extend_times should be 0');

# Reset table
$node_primary->safe_psql('postgres', q[drop table bulk_extend_tbl;]);

# Open the feature
$node_primary->safe_psql('postgres',
	'alter system set polar_index_create_bulk_extend_size = 512;');
$node_primary->reload;

$node_primary->safe_psql('postgres',
	q[create table bulk_extend_tbl(id int8, value int8);]);

# Load Data
$node_primary->safe_psql('postgres',
	q[INSERT INTO bulk_extend_tbl select generate_series,generate_series from generate_series(0, 12800*185 + 184);]
);

$node_primary->safe_psql('postgres',
	q[CREATE INDEX bulk_extend_idx on bulk_extend_tbl(id);]);

is( $node_primary->safe_psql(
		'postgres',
		'select idx_create_extend_times = 13 from polar_pg_stat_all_index_extend_stats where relname=\'bulk_extend_tbl\';'
	),
	't',
	'idx_create_extend_times should be 13');

$node_primary->stop;
