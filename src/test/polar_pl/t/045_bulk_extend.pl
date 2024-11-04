#!/usr/bin/perl
# 045_bulk_extend.pl
#	  Test bulk extend for heap_tbl table and btree index.
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
#	  src/test/polar_pl/t/045_bulk_extend.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize primary node
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);
$node_primary->append_conf('postgresql.conf',
	"polar_heap_bulk_extend_size = 0");
$node_primary->append_conf('postgresql.conf',
	"polar_index_bulk_extend_size = 0");
$node_primary->append_conf('postgresql.conf',
	"polar_recovery_bulk_extend_size = 0");
$node_primary->append_conf('postgresql.conf', "max_connections = 10");
$node_primary->append_conf('postgresql.conf', "shared_buffers = 16MB");
$node_primary->append_conf('postgresql.conf', "enable_seqscan = off");
$node_primary->start;
my $backup_name = 'my_backup';

# Take backup
$node_primary->backup($backup_name);

# Create streaming standby linking to primary
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);
$node_standby->start;

$node_primary->safe_psql('postgres', 'CREATE EXTENSION amcheck');
$node_primary->safe_psql('postgres', 'CREATE EXTENSION bloom');

$node_primary->safe_psql('postgres', 'CREATE TABLE heap_tbl(id int)');
$node_primary->safe_psql('postgres',
	'CREATE INDEX btree_idx ON heap_tbl(id)');
$node_primary->safe_psql('postgres',
	'CREATE TABLE misc_tbl(id int4, arr int4[], gp point, sp point, m int4)');
$node_primary->safe_psql('postgres',
	'CREATE INDEX gin_idx ON misc_tbl USING gin(arr)');
$node_primary->safe_psql('postgres',
	'CREATE INDEX gist_idx ON misc_tbl USING gist(gp)');
$node_primary->safe_psql('postgres',
	'CREATE INDEX spgist_idx ON misc_tbl USING spgist(sp)');
$node_primary->safe_psql('postgres',
	'CREATE INDEX bloom_idx ON misc_tbl USING bloom(m, id)');

my ($base_heap_size, $base_btree_size, $base_gin_size,
	$base_gist_size, $base_spgist_size, $base_bloom_size);

sub bulk_extend_sanity_check
{
	my $node = shift;
	my $extend_size = shift;

	my $heap_size = $node->safe_psql('postgres',
		"SELECT pg_relation_size('heap_tbl')/8192");
	my $btree_size = $node->safe_psql('postgres',
		"SELECT pg_relation_size('btree_idx')/8192");
	my $gin_size =
	  $node->safe_psql('postgres', "SELECT pg_relation_size('gin_idx')/8192");
	my $gist_size = $node->safe_psql('postgres',
		"SELECT pg_relation_size('gist_idx')/8192");
	my $spgist_size = $node->safe_psql('postgres',
		"SELECT pg_relation_size('spgist_idx')/8192");
	my $bloom_size = $node->safe_psql('postgres',
		"SELECT pg_relation_size('bloom_idx')/8192");

	if ($extend_size == 0)
	{
		if ($node eq $node_primary)
		{
			$base_heap_size = $node->safe_psql('postgres',
				"SELECT pg_relation_size('heap_tbl')/8192");
			$base_btree_size = $node->safe_psql('postgres',
				"SELECT pg_relation_size('btree_idx')/8192");
			$base_gin_size = $node->safe_psql('postgres',
				"SELECT pg_relation_size('gin_idx')/8192");
			$base_gist_size = $node->safe_psql('postgres',
				"SELECT pg_relation_size('gist_idx')/8192");
			$base_spgist_size = $node->safe_psql('postgres',
				"SELECT pg_relation_size('spgist_idx')/8192");
			$base_bloom_size = $node->safe_psql('postgres',
				"SELECT pg_relation_size('bloom_idx')/8192");
		}
	}
	else
	{
		ok(($heap_size - $base_heap_size) < $extend_size,
			'no waste in heap bulk extend');
		print("heap_size: $heap_size, base_heap_size: $base_heap_size\n");
		ok(($btree_size - $base_btree_size) < $extend_size,
			'no waste in btree bulk extend');
		print("btree_size: $btree_size, base_btree_size: $base_btree_size\n");
		ok(($gin_size - $base_gin_size) < $extend_size,
			'no waste in gin bulk extend');
		print("gin_size: $gin_size, base_gin_size: $base_gin_size\n");
		ok(($gist_size - $base_gist_size) < $extend_size,
			'no waste in gist bulk extend');
		print("gist_size: $gist_size, base_gist_size: $base_gist_size\n");
		ok(($spgist_size - $base_spgist_size) < $extend_size,
			'no waste in spgist bulk extend');
		print(
			"spgist_size: $spgist_size, base_spgist_size: $base_spgist_size\n"
		);
		ok(($bloom_size - $base_bloom_size) < $extend_size,
			'no waste in bloom bulk extend');
		print("bloom_size: $bloom_size, base_bloom_size: $base_bloom_size\n");
	}

	# heap and btree got amcheck, use it
	$node->safe_psql('postgres', "SELECT verify_heapam('heap_tbl')");
	if ($node eq $node_primary)
	{
		$node->safe_psql('postgres',
			"SELECT bt_index_parent_check('btree_idx', 't', 't')");
	}
	else
	{
		$node->safe_psql('postgres',
			"SELECT bt_index_check('btree_idx', 't')");
	}

	is( $node->safe_psql(
			'postgres',
			'WITH rand AS (SELECT (floor(random() * 99998) + 3)::int v)
			 SELECT count(*) FROM misc_tbl, rand
			 WHERE arr @> array[1, rand.v::int]'
		),
		1,
		'gin index check');
	is( $node->safe_psql(
			'postgres',
			'WITH rand AS (SELECT ceil(random() * 100000)::int v)
			 SELECT count(*) FROM misc_tbl, rand
			 WHERE gp <@ box(point(rand.v*10,rand.v*10), point((rand.v+1)*10, (rand.v+1)*10))'
		),
		1,
		'gist index check');
	is( $node->safe_psql(
			'postgres',
			'WITH rand AS (SELECT ceil(random() * 100000)::int v)
			 SELECT count(*) FROM misc_tbl, rand
			 WHERE sp <@ box(point(rand.v*10,rand.v*10), point((rand.v+1)*10, (rand.v+1)*10))'
		),
		1,
		'spgist index check');
	is( $node->safe_psql(
			'postgres',
			'WITH rand AS (SELECT (ceil(random() * 99999) + 1)::int v)
			 SELECT count(*) FROM misc_tbl, rand
			 WHERE m = rand.v%10 and id = rand.v'
		),
		1,
		'bloom index check');
}

sub test_bulk_extend
{
	my $extend_size = shift;

	print("Test bulk extend with size of $extend_size blocks\n");

	$node_primary->safe_psql('postgres', 'TRUNCATE TABLE heap_tbl');
	$node_primary->safe_psql('postgres', 'TRUNCATE TABLE misc_tbl');

	$node_primary->safe_psql('postgres',
		'INSERT INTO heap_tbl SELECT generate_series(1,1000000)');
	$node_primary->safe_psql(
		'postgres',
		"INSERT INTO misc_tbl
		 SELECT g, array[1, 2, g], point(g*10+1, g*10+1), point(g*10+1, g*10+1), g%10
		 FROM generate_series(1, 100000) g"
	);

	bulk_extend_sanity_check($node_primary, $extend_size);

	$node_primary->stop('immediate');
	$node_primary->start;
	bulk_extend_sanity_check($node_primary, $extend_size);

	$node_primary->wait_for_catchup($node_standby);
	bulk_extend_sanity_check($node_standby, $extend_size);
}

foreach my $extend_size (0, 1, 3, 16, 512)
{
	print("test extend_size=$extend_size\n");
	$node_primary->append_conf('postgresql.conf',
		"polar_heap_bulk_extend_size = $extend_size");
	$node_primary->append_conf('postgresql.conf',
		"polar_index_bulk_extend_size = $extend_size");
	$node_primary->append_conf('postgresql.conf',
		"polar_recovery_bulk_extend_size = $extend_size");
	$node_primary->reload;
	test_bulk_extend $extend_size;
}

done_testing();
