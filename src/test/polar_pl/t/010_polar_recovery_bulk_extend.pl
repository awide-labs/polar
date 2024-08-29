# 010_polar_recovery_bulk_extend.pl
#	  Test case: bulk extend in master crash recovery.
#     It will: (1) Insert; (2) Truncate table; (3) stop RW immediate; (4) start RW
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
#	  src/test/polar_pl/t/010_polar_recovery_bulk_extend.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Config;
plan tests => 3;
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;

my $node_replica = PostgreSQL::Test::Cluster->new('replica');
$node_replica->polar_init_replica($node_primary);
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->polar_init_standby($node_primary);
# Close the parallel_replay and checkpoint
$node_primary->append_conf('postgresql.conf', 'wal_sender_timeout=3600s');
$node_primary->append_conf('postgresql.conf', 'checkpoint_timeout=3600');
$node_primary->append_conf('postgresql.conf',
	'polar_enable_parallel_replay_standby_mode=false');
$node_replica->append_conf('postgresql.conf', 'wal_receiver_timeout=3600s');
$node_replica->append_conf('postgresql.conf', 'checkpoint_timeout=3600');
$node_replica->append_conf('postgresql.conf',
	'polar_enable_parallel_replay_standby_mode=false');
$node_standby->append_conf('postgresql.conf', 'wal_receiver_timeout=3600s');
$node_standby->append_conf('postgresql.conf', 'checkpoint_timeout=3600');
$node_standby->append_conf('postgresql.conf',
	'polar_enable_parallel_replay_standby_mode=false');

$node_primary->start;
$node_primary->polar_create_slot($node_replica->name);
$node_primary->polar_create_slot($node_standby->name);
$node_replica->start;
$node_standby->start;
$node_standby->polar_drop_all_slots;
# Load Data
$node_primary->safe_psql('postgres',
	q[drop table if exists recovery_bulk_extend_tbl;]);
$node_primary->safe_psql('postgres',
	q[Create table recovery_bulk_extend_tbl(c1 int,c2 int);]);
$node_primary->safe_psql('postgres',
	q[Insert into recovery_bulk_extend_tbl select generate_series(0,1000000), generate_series(0,1000000);]
);
my $filepath = $node_primary->safe_psql('postgres',
	q[Select pg_relation_filepath('recovery_bulk_extend_tbl');]);
my $file_size = $node_primary->safe_psql('postgres',
	q[Select pg_relation_size('recovery_bulk_extend_tbl');]);
print "total file path is $filepath \n";
print "old file size is $file_size \n";
# Drop Data. Truncating tables is not OK because truncating will create new relfilenode and relink
# instead of clear up the space of old relfilenode. The latter will not trigger recovery bulk extend.
# Delete from and vacuum will not clear the space of relfilenode in perl. So we use 'truncate -s 0 file'
my $pos = index($filepath, "/base");
my $end_pos = length($filepath);
print "The start pos is $pos, the end pos is $end_pos\n";
$filepath = substr($filepath, $pos, $end_pos - $pos);
my $datadir = $node_primary->polar_get_datadir;
$datadir .= $filepath;
$filepath = $datadir;
print "filepath is $filepath\n";
my @res = `truncate -s 0 $filepath`;
print "the truncate command is @res\n";
$file_size = $node_primary->safe_psql('postgres',
	q[Select pg_relation_size('recovery_bulk_extend_tbl');]);
print "new file size is $file_size \n";
# Crash and restart the postmaster
$node_primary->stop('immediate');
$node_primary->start;
is( $node_primary->safe_psql(
		'postgres',
		q[Select count(*) = 1000001 from recovery_bulk_extend_tbl;]),
	't',
	'heap recovery_bulk_extend_tbl int primary should be 1000001 after crash recovery'
);

# Check replica/standby
my $lsn = $node_primary->lsn('write');
$node_primary->wait_for_catchup($node_standby->name, 'replay', $lsn, 't',
	't', 300);
$node_primary->wait_for_catchup($node_replica->name, 'replay', $lsn, 't',
	't', 300);
is( $node_replica->safe_psql(
		'postgres',
		q[Select count(*) = 1000001 from recovery_bulk_extend_tbl;]),
	't',
	'heap recovery_bulk_extend_tbl in replica should be 1000001 after crash recovery'
);

is( $node_standby->safe_psql(
		'postgres',
		q[Select count(*) = 1000001 from recovery_bulk_extend_tbl;]),
	't',
	'heap recovery_bulk_extend_tbl in standby should be 1000001 after crash recovery'
);

$node_primary->stop();
$node_replica->stop();
$node_standby->stop();
