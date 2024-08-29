#!/usr/bin/perl

# 017_logical_repl_bulk_read_wal.pl
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
#	  src/test/polar_pl/t/017_logical_repl_bulk_read_wal.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->append_conf('postgresql.conf',
	"polar_logical_repl_xlog_bulk_read_size = 128kB");
$node_publisher->start;

my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init(allows_streaming => 'logical');
$node_subscriber->append_conf('postgresql.conf',
	"wal_retrieve_retry_interval = 1ms");
$node_subscriber->start;

# Setup logical replication
$node_publisher->pgbench_init('postgres');
$node_subscriber->pgbench_init('postgres');
$node_subscriber->safe_psql('postgres',
	"truncate table pgbench_accounts, pgbench_branches, pgbench_history, pgbench_tellers"
);
my $pub_name = 'big_txn_pub';
my $sub_name = 'big_txn_sub';
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION $pub_name FOR ALL TABLES");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION $sub_name CONNECTION '$publisher_connstr application_name=$sub_name' PUBLICATION $pub_name"
);
$node_publisher->wait_for_catchup($sub_name);

# Stop logical replication temporarily to accumulate WALs.
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION $sub_name DISABLE");

my $wal_start_lsn = $node_publisher->lsn('insert');
while (1)
{
	$node_publisher->pgbench_test(
		dbname => 'postgres',
		client => 5,
		job => 5,
		time => 20,
		script => 'tpcb-like');

	my $cur_insert_lsn = $node_publisher->lsn('insert');

	# We have accumulated enough WALs.
	my $res = $node_publisher->safe_psql('postgres',
		"SELECT diff > 67108864 from pg_wal_lsn_diff('$cur_insert_lsn'::pg_lsn, '$wal_start_lsn'::pg_lsn) as diff;"
	);
	if ($res eq 't')
	{
		note "create wal records [$wal_start_lsn, $cur_insert_lsn)\n";
		last;
	}
	note
	  "current insert lsn: %cur_insert_lsn, start insert lsn: $wal_start_lsn\n";
}

# Start logical replication to consume WALs.
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION $sub_name ENABLE");
$node_publisher->wait_for_catchup($sub_name);

# Check consistency.
my $source = $node_publisher->safe_psql('postgres',
	"SELECT COUNT(*) from pgbench_history");
my $target = $node_subscriber->safe_psql('postgres',
	"SELECT COUNT(*) from pgbench_history");
ok($source == $target, "Publisher and subscriber have the same data.");

$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION $sub_name");
$node_subscriber->stop('fast');
$node_publisher->stop('fast');

done_testing();
