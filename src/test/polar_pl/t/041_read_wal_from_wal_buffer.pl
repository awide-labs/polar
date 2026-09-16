#!/usr/bin/perl

# 041_read_wal_from_wal_buffer.pl
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
#	  src/test/polar_pl/t/041_read_wal_from_wal_buffer.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use Test::More;

#
# Get a primary node ready for physical/logical replication.
#
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;
$node_primary->append_conf('postgresql.conf', "wal_writer_delay = 1ms");
$node_primary->append_conf('postgresql.conf', "wal_level = logical");
$node_primary->append_conf('postgresql.conf', "wal_buffers = 20MB");
$node_primary->append_conf('postgresql.conf',
	"polar_wal_buffer_reserved_old_pages = 2048");    # ~16MB
$node_primary->append_conf('postgresql.conf',
	"polar_wal_buffer_reserved_old_pages_ratio = 0.8");    # ~16MB
$node_primary->append_conf('postgresql.conf',
	"polar_enable_read_from_wal_buffers = off");

#
# Prepare a standby node.
#
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->polar_init_standby($node_primary);

$node_primary->start;
$node_primary->polar_create_slot($node_standby->name);
$node_standby->start;

#
# Prepare a logical subscriber node.
#
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->polar_init_primary;
$node_subscriber->append_conf('postgresql.conf',
	"wal_retrieve_retry_interval = 1ms");
$node_subscriber->start;

#
# Tool for getting I/O count.
#
$node_primary->safe_psql('postgres', 'CREATE EXTENSION polar_monitor;');

#
# Table structure and initial table sync for logical replication.
#
$node_primary->pgbench_init(dbname => 'postgres');
$node_subscriber->pgbench_init(dbname => 'postgres');
$node_subscriber->safe_psql('postgres',
	'truncate table pgbench_accounts, pgbench_branches, pgbench_history, pgbench_tellers'
);

my $publisher_connstr = $node_primary->connstr . ' dbname=postgres';
my $subname = 'sub';
$node_primary->safe_psql('postgres', "CREATE PUBLICATION pub FOR ALL TABLES");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION $subname CONNECTION '$publisher_connstr application_name=$subname' PUBLICATION pub"
);
$node_subscriber->wait_for_subscription_sync;
$node_primary->wait_for_catchup($subname);
$node_primary->wait_for_catchup($node_standby);

#
# Generate WAL which fits in WAL buffer (20 MB).
#
my $generated_wal_size = 20 * 1024 * 1024;

#
# Get WAL read count without using WAL buffer.
#
my $xlog_read_before = $node_primary->safe_psql('postgres',
	"SELECT SUM(read_count) FROM polar_stat_io_info() where filetype = 'WAL' AND read_count > 0;"
);
my $wal_start_lsn = $node_primary->lsn('insert');
$node_primary->generate_wal($wal_start_lsn, $generated_wal_size);
$node_primary->safe_psql('postgres', "CHECKPOINT");
$node_primary->wait_for_catchup($subname);
$node_primary->wait_for_catchup($node_standby);
my $xlog_read_after = $node_primary->safe_psql('postgres',
	"SELECT SUM(read_count) FROM polar_stat_io_info() where filetype = 'WAL' AND read_count > 0;"
);

my $unbuffered_read = $xlog_read_after - $xlog_read_before;

#
# Enable reading WAL from WAL buffer.
#
$node_primary->stop('immediate');
$node_primary->append_conf('postgresql.conf',
	"polar_enable_read_from_wal_buffers = on");
$node_primary->start;

#
# Get WAL read count using WAL buffer.
#
$xlog_read_before = $node_primary->safe_psql('postgres',
	"SELECT SUM(read_count) FROM polar_stat_io_info() where filetype = 'WAL' AND read_count > 0;"
);
$wal_start_lsn = $node_primary->lsn('insert');
$node_primary->generate_wal($wal_start_lsn, $generated_wal_size);
$node_primary->safe_psql('postgres', "CHECKPOINT");
$node_primary->wait_for_catchup($subname);
$node_primary->wait_for_catchup($node_standby);
$xlog_read_after = $node_primary->safe_psql('postgres',
	"SELECT SUM(read_count) FROM polar_stat_io_info() where filetype = 'WAL' AND read_count > 0;"
);

my $buffered_read = $xlog_read_after - $xlog_read_before;

#
# We should save a lot of I/Os (use half of unbuffered IOPS for more strict check).
#
ok( $buffered_read < $unbuffered_read / 2,
	"Read from WAL buffers uses $buffered_read I/Os while read WAL directly uses $unbuffered_read I/Os."
);

#
# Monitoring.
#
$node_primary->safe_psql('postgres',
	"SELECT * FROM polar_stat_walsnd_xlog_read();");
$node_primary->safe_psql('postgres', "SELECT polar_stat_walsnd_reset();");

#
# Clean up and we're done.
#
$node_primary->stop('fast');
$node_standby->stop('fast');
$node_subscriber->stop('fast');

done_testing();
