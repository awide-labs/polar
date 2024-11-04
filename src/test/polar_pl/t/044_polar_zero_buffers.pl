#!/usr/bin/perl
# 044_polar_zero_buffers.pl
#	  Test polar_pwrite_zeros with different size of GUC polar_zero_buffers.
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
#	  src/test/polar_pl/t/044_polar_zero_buffers.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize primary node
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);
$node_primary->start;
my $backup_name = 'my_backup';

# Take backup
$node_primary->backup($backup_name);

# Create streaming standby linking to primary
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);
$node_standby->start;

sub test_polar_pwrite_zeros
{
	$node_primary->safe_psql('postgres', 'CREATE TABLE t(id int)');
	$node_primary->safe_psql('postgres',
		'INSERT INTO t SELECT generate_series(1,1000000)');
	$node_primary->safe_psql('postgres', 'CREATE INDEX ON t(id)');
	is( $node_primary->safe_psql(
			'postgres', 'SET enable_indexscan = off; SELECT count(*) FROM t'),
		1000000,
		"heap is ok on primary");
	is( $node_primary->safe_psql(
			'postgres', 'SET enable_seqscan = off; SELECT count(*) FROM t'),
		1000000,
		"btree index is ok on primary");

	$node_primary->wait_for_catchup($node_standby);

	is( $node_standby->safe_psql(
			'postgres', 'SET enable_indexscan = off; SELECT count(*) FROM t'),
		1000000,
		"heap is ok on standby");
	is( $node_standby->safe_psql(
			'postgres', 'SET enable_seqscan = off; SELECT count(*) FROM t'),
		1000000,
		"btree index is ok on standby");

	$node_primary->safe_psql('postgres', 'DROP TABLE t');
}

foreach my $zero_buffers (-1, 0, 1, 3, 16, 512)
{
	print("test zero_buffers=$zero_buffers\n");
	$node_primary->append_conf('postgresql.conf',
		"polar_zero_buffers = $zero_buffers");
	$node_primary->restart;
	$node_standby->append_conf('postgresql.conf',
		"polar_zero_buffers = $zero_buffers");
	$node_standby->restart;
	test_polar_pwrite_zeros;
}

done_testing();
