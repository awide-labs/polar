#!/usr/bin/perl

# 038_bulk_write.pl
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
#	  src/test/polar_pl/t/038_bulk_write.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node_primary;
my $node_replica;
my $node_standby;

sub test_index_create()
{
	$node_primary->safe_psql('postgres',
		'CREATE TABLE t(id int, ir int4range)');
	$node_primary->safe_psql('postgres',
		'INSERT INTO t SELECT i, int4range(i, i+100) FROM generate_series(1,10000) AS i'
	);
	$node_primary->safe_psql('postgres', 'CREATE INDEX ON t(id)');
	$node_primary->safe_psql('postgres',
		'CREATE INDEX ON t USING SPGIST (ir)');
	$node_primary->safe_psql('postgres', 'VACUUM FULL t');
	is( $node_primary->safe_psql(
			'postgres', 'SET enable_seqscan = off; SELECT count(*) FROM t'),
		10000,
		"btree index is ok");
	$node_primary->safe_psql('postgres', 'TRUNCATE t');
	$node_primary->safe_psql('postgres',
		'INSERT INTO t SELECT i, int4range(i, i+100) FROM generate_series(1,10000) AS i'
	);
	is( $node_primary->safe_psql(
			'postgres', 'SET enable_seqscan = off; SELECT count(*) FROM t'),
		10000,
		"btree index is ok");

	$node_primary->wait_for_catchup($node_replica);
	is( $node_replica->safe_psql(
			'postgres', 'SET enable_seqscan = off; SELECT count(*) FROM t'),
		10000,
		"btree index is ok in replica");

	$node_primary->wait_for_catchup($node_standby);
	is( $node_standby->safe_psql(
			'postgres', 'SET enable_seqscan = off; SELECT count(*) FROM t'),
		10000,
		"btree index is ok in standby");

	$node_primary->safe_psql('postgres', 'DROP TABLE t');
	$node_primary->safe_psql('postgres',
		'CREATE TABLE t(id int, ir int4range)');
	$node_primary->safe_psql('postgres',
		'INSERT INTO t SELECT i, int4range(i, i+100) FROM generate_series(1,10000) AS i'
	);
	$node_primary->safe_psql('postgres', 'CREATE INDEX ON t(id)');
	$node_primary->stop('immediate');
	$node_primary->start;
	is( $node_primary->safe_psql(
			'postgres', 'SET enable_seqscan = off; SELECT count(*) FROM t'),
		10000,
		"btree index is ok after crash");

	$node_primary->safe_psql('postgres', 'DROP TABLE t');
}

$node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;

$node_replica = PostgreSQL::Test::Cluster->new('replica');
$node_replica->polar_init_replica($node_primary);

$node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->polar_init_standby($node_primary);

$node_primary->start;

$node_primary->polar_create_slot($node_replica->name);
$node_primary->polar_create_slot($node_standby->name);

$node_replica->start;
$node_standby->start;

# test bulk write with polar_zero_extend_method = none
$node_primary->append_conf('postgresql.conf',
	"polar_zero_extend_method = none");
$node_primary->reload;

foreach my $maxpages (1, 2, 16, 128, 512)
{
	print("test maxpages=$maxpages\n");
	$node_primary->append_conf('postgresql.conf',
		"polar_bulk_write_maxpages = $maxpages");
	$node_primary->reload;
	test_index_create;
}

# test bulk write with polar_zero_extend_method = bulkwrite
$node_primary->append_conf('postgresql.conf',
	"polar_zero_extend_method = bulkwrite");
$node_primary->reload;

foreach my $maxpages (1, 2, 16, 128, 512)
{
	print("test maxpages=$maxpages\n");
	$node_primary->append_conf('postgresql.conf',
		"polar_bulk_write_maxpages = $maxpages");
	$node_primary->reload;
	test_index_create;
}

# test bulk write with polar_zero_extend_method = fallocate
$node_primary->append_conf('postgresql.conf',
	"polar_zero_extend_method = fallocate");
$node_primary->reload;

foreach my $maxpages (1, 2, 16, 128, 512)
{
	print("test maxpages=$maxpages\n");
	$node_primary->append_conf('postgresql.conf',
		"polar_bulk_write_maxpages = $maxpages");
	$node_primary->reload;
	test_index_create;
}

# done with the node
$node_primary->stop;
$node_replica->stop;
$node_standby->stop;

done_testing();
