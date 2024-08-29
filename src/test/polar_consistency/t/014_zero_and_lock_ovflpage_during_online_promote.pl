# 014_zero_and_lock_ovflpage_during_online_promote.pl
#	  Test for RBM_ZERO_AND_LOCK during online promote
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
#	  src/test/polar_consistency/t/014_zero_and_lock_ovflpage_during_online_promote.pl

use strict;
use warnings;
use Config;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($Config{osname} eq 'MSWin32')
{
	# some Windows Perls at least don't like IPC::Run's start/kill_kill regime.
	plan skip_all => 'Test fails on Windows perl';
}
elsif ($ENV{enable_fault_injector} eq 'no')
{
	plan skip_all => 'Fault injector not supported by this build';
}

# primary node
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;
$node_primary->append_conf('postgresql.conf', "hot_standby_feedback=off");

# replica
my $node_replica = PostgreSQL::Test::Cluster->new('replica1');
$node_replica->polar_init_replica($node_primary);

$node_primary->append_conf('postgresql.conf',
	"synchronous_standby_names='" . $node_replica->name . "'");
$node_primary->start;
$node_primary->polar_create_slot($node_replica->name);

$node_replica->append_conf('postgresql.conf', "checkpoint_timeout=60000");
$node_replica->append_conf('postgresql.conf', "restart_after_crash=off");
$node_replica->append_conf('postgresql.conf', "hot_standby_feedback=off");
$node_replica->start;

my $res =
  $node_primary->safe_psql('postgres', "create extension faultinjector;");
print "create faultinjector res: $res\n";
$res =
  $node_primary->safe_psql('postgres', "create extension pg_walinspect;");
print "create pg_walinspect res: $res\n";
$res = $node_primary->safe_psql('postgres', "create extension pageinspect;");
print "create pageinspect res: $res\n";
$res =
  $node_primary->safe_psql('postgres', "create extension polar_monitor;");
print "create polar_monitor res: $res\n";

# enable fault inject in replica
$res = $node_replica->safe_psql('postgres',
	"select inject_fault('polar_delay_wal_replay', 'enable', '', '', 1, -1, -1)"
);
print "enable polar_delay_wal_replay fault inject:$res\n";

sub init_table
{
	my ($node) = @_;
	my $init_sql = "
		create table t1(id int, g_vchar varchar(256)) with (autovacuum_enabled=off);
		create index t1_hash on t1 using hash(g_vchar);
	";

	my $result = $node->safe_psql('postgres', $init_sql);
	print "init result: $result\n";
	my $insert = $node->lsn('insert');
	print "after init, insert lsn is $insert\n";

	return $insert;
}

sub vacuum_table
{
	my ($node) = @_;

	my $vacuum_sql = "vacuum t1; analyse t1;";
	my $result = $node->safe_psql('postgres', $vacuum_sql);
	print "vacuum table results: $result\n";
	my $insert = $node->lsn('insert');
	print "after vacuum table, insert lsn is $insert\n";
	return $insert;
}

sub get_wal_records
{
	my ($node, $start_lsn, $end_lsn, $cols, $condition) = @_;
	my $get_sql =
	  "select $cols from pg_get_wal_records_info('$start_lsn', '$end_lsn') where $condition;";
	my $result = $node->safe_psql('postgres', $get_sql);
	print "get wal records from sql [$get_sql]:\n$result\n";
	return $result;
}

sub inspect_hash_index
{
	my ($node, $index, $pageno) = @_;

	my $meta_page_info_sql = "
		select ntuples, ffactor, bsize, bmsize, bmshift, maxbucket, highmask, lowmask, ovflpoint
		from hash_metapage_info(get_raw_page('$index', 0));
	";
	my $meta_page_info = $node->safe_psql('postgres', $meta_page_info_sql);

	my $page_stats_sql = "
		select live_items, dead_items, page_size, free_size, hasho_prevblkno, hasho_nextblkno,
			   hasho_bucket, hasho_flag, hasho_page_id
		from hash_page_stats(get_raw_page('$index', $pageno));
	";
	my $page_stats = $node->safe_psql('postgres', $page_stats_sql);

	print
	  "meta page info: $meta_page_info\npage($pageno)'s stats: $page_stats\n";
	return ($meta_page_info, $page_stats);
}

my ($cur_insert, $prev_insert, $meta_page_info, $page_stats);

init_table($node_primary);
vacuum_table($node_primary);

my $hash_index = 't1_hash';
my $relfienode = $node_primary->safe_psql('postgres',
	"select relfilenode from pg_class where relname = '$hash_index';");
my $database = $node_primary->safe_psql('postgres',
	"select oid from pg_database where datname = 'postgres';");
my $tablespace = '1663';
my $target_buftag = "rel $tablespace/$database/$relfienode fork main blk 4";
print "target buftag: $target_buftag\n";

my $insert_id_zero = "insert into t1 values (0, repeat('A', 256));";

# fill bucket 1 whose pageno is 2
$node_primary->safe_psql('postgres', $insert_id_zero);
($meta_page_info, $page_stats) =
  inspect_hash_index($node_primary, $hash_index, 2);
my $prev_free_size = (split('\|', $page_stats))[3];
$node_primary->safe_psql('postgres', $insert_id_zero);
($meta_page_info, $page_stats) =
  inspect_hash_index($node_primary, $hash_index, 2);
my $cur_free_size = (split('\|', $page_stats))[3];
my $one_tuple_size = $prev_free_size - $cur_free_size;
my $need_tuples = $cur_free_size / $one_tuple_size;
my $fill_bucket_sql =
  "insert into t1 select 0, repeat('A', 256) from generate_series(1, $need_tuples);";
$prev_insert = $node_primary->lsn('insert');
$node_primary->safe_psql('postgres', $fill_bucket_sql);
$cur_insert = vacuum_table($node_primary);
my $result = get_wal_records($node_primary, $prev_insert, $cur_insert, "*",
	"block_ref like '\%$target_buftag\%'");
ok($result eq '', "ovfl page 4 hasn't been created for now.");

# create ovfl page
my $insert_id_one = "insert into t1 values (1, repeat('A', 256));";
$node_primary->safe_psql('postgres', $insert_id_one);
($meta_page_info, $page_stats) =
  inspect_hash_index($node_primary, $hash_index, 4);
my $page_flag = (split('\|', $page_stats))[7];
ok($page_flag eq 1, "ovfl page 4 has been created for now.");

# do checkpoint
$node_primary->wait_for_catchup($node_replica);
my $prev_redo_lsn =
  $node_primary->polar_get_redo_location($node_primary->data_dir());
$node_primary->safe_psql('postgres', 'checkpoint;');
my $cur_redo_lsn =
  $node_primary->polar_get_redo_location($node_primary->data_dir());
print "redo lsn change from $prev_redo_lsn to $cur_redo_lsn\n";

# insert one more tuple to make the first WAL record after redo is not page init related
$prev_insert = $node_primary->lsn('insert');
$node_primary->safe_psql('postgres', $insert_id_one);
$cur_insert = $node_primary->lsn('insert');
get_wal_records($node_primary, $prev_insert, $cur_insert, "*",
	"block_ref like '\%$target_buftag\%'");

# free ovfl page 4
$prev_insert = $node_primary->lsn('insert');
$node_primary->safe_psql('postgres', "delete from t1 where id = 1;");
vacuum_table($node_primary);
$cur_insert = $node_primary->lsn('insert');
get_wal_records($node_primary, $prev_insert, $cur_insert, "*",
	"block_ref like '\%$target_buftag\%'");

# stop primary without shutdown checkpoint
$node_primary->stop('i');

# promote replica
$node_replica->promote;
$node_replica->polar_drop_all_slots();
$node_replica->polar_wait_for_startup(300, 0);

# use ovfl page 4 again
$prev_insert = $node_replica->lsn('insert');
$node_replica->safe_psql('postgres', $insert_id_one);
$cur_insert = $node_replica->lsn('insert');
get_wal_records($node_replica, $prev_insert, $cur_insert, '*',
	"block_ref like '\%$target_buftag\%'");

($meta_page_info, $page_stats) =
  inspect_hash_index($node_replica, $hash_index, 4);
$page_flag = (split('\|', $page_stats))[7];
ok($page_flag eq 1, "ovfl page 4 has been created again for now.");

$res = $node_replica->safe_psql('postgres',
	"select inject_fault('polar_delay_wal_replay', 'reset')");
print "reset delay wal replay:$res\n";

$node_replica->safe_psql('postgres', 'checkpoint;');
$node_replica->stop;
done_testing();
