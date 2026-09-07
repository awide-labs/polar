#!/usr/bin/perl

# 039_logical_repl_bulk_read_wal.pl
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
#	  src/test/polar_pl/t/039_logical_repl_bulk_read_wal.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->polar_init_primary(allows_streaming => 'logical');
$node_publisher->append_conf('postgresql.conf',
	"polar_logical_repl_xlog_bulk_read_size = 128kB");
$node_publisher->start;

my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->polar_init_primary(allows_streaming => 'logical');
$node_subscriber->append_conf('postgresql.conf',
	"wal_retrieve_retry_interval = 1ms");
$node_subscriber->start;

$node_publisher->safe_psql('postgres', 'CREATE EXTENSION polar_monitor;');
$node_publisher->safe_psql('postgres', 'CHECKPOINT;');

# Setup logical replication
$node_publisher->pgbench_init(dbname => 'postgres', scale => 10);
$node_subscriber->pgbench_init(dbname => 'postgres', scale => 10);
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

# Generate enough WALs to make replication latency large enough.
my $xlog_read_count_before = $node_publisher->safe_psql('postgres',
	"SELECT SUM(read_count) FROM polar_stat_io_info() where filetype = 'WAL' AND read_count > 0;"
);
my $wal_start_lsn = $node_publisher->lsn('insert');
my $latency = $node_publisher->generate_wal($wal_start_lsn, 32 * 1024 * 1024)
  ;    # 32MB at least

# Start logical replication to consume WALs.
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION $sub_name ENABLE");
$node_publisher->wait_for_catchup_with_progress($sub_name);

# Check consistency.
my $source = $node_publisher->safe_psql('postgres',
	"SELECT COUNT(*) from pgbench_history");
my $target = $node_subscriber->safe_psql('postgres',
	"SELECT COUNT(*) from pgbench_history");
ok($source == $target, "Publisher and subscriber have the same data.");

# IOPS count check
my $xlog_read_count = $node_publisher->safe_psql('postgres',
	"SELECT SUM(read_count) FROM polar_stat_io_info() where filetype = 'WAL' AND read_count > 0;"
);
$xlog_read_count = $xlog_read_count - $xlog_read_count_before;
ok($xlog_read_count < $latency / 8192,
	"WAL sender: read $xlog_read_count times for $latency bytes.");

$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION $sub_name");
$node_subscriber->stop('fast');
$node_publisher->stop('fast');

done_testing();
