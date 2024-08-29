# 016_logindex_recycle_only_for_replica_slots.pl
#	  polar logindex recycle test case
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
#	  src/test/polar_pl/t/016_logindex_recycle_only_for_replica_slots.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

our $regress_db = 'postgres';
our %prev_log_index_meta;
our %cur_log_index_meta;

sub wait_replication_catchup
{
	my ($root_node, $node) = @_;
	my $timeout = 60;

	for (my $i = 0; $i < $timeout; $i += 1)
	{
		my $slot_name = $node->name;
		my $sql =
		  "select active from pg_replication_slots where slot_name = '$slot_name';";
		my $active = $root_node->safe_psql($regress_db, $sql);
		print "sql is '$sql', active is $active\n";
		if ($active eq 't')
		{
			last;
		}
		sleep(1);
	}

	$root_node->safe_psql($regress_db, "insert into test values (1);");
	my $cur_insert_lsn = $root_node->lsn('insert');
	$root_node->safe_psql($regress_db, "insert into test values (2);");
	$root_node->wait_for_catchup($node->name, 'replay', $cur_insert_lsn);
}

sub get_log_index_meta
{
	my ($node) = @_;
	my $polar_datadir = $node->polar_get_datadir;
	my $logindex_meta = readpipe(
		"polar_tools logindex-meta -f $polar_datadir/pg_logindex/log_index_meta"
	);
	print "logindex meta: $logindex_meta\n";

	my $pattern = qr/(\S+)\s*=\s*(\S+)/;
	my @matches = ($logindex_meta =~ /$pattern/g);
	my %log_index_meta;

	for (my $i = 0; $i < @matches; $i += 2)
	{
		@log_index_meta{ $matches[$i] } = $matches[ $i + 1 ];
	}

	return %log_index_meta;
}

sub wait_for_logindex_meta_catchup
{
	my ($root_node, $replica, @wait_slots) = @_;
	my $max_loop = 60;
	my $insert_sql = "insert into test select generate_series(1, 100000);";
	my $truncate_sql = "truncate table test;";
	my $catchup = 0;

	%prev_log_index_meta = get_log_index_meta($root_node);

	for (my $i = 0; $i < $max_loop; $i += 1)
	{
		$root_node->safe_psql($regress_db, $insert_sql);
		$root_node->safe_psql($regress_db, $truncate_sql);
		$root_node->safe_psql($regress_db, 'checkpoint;');
		$root_node->polar_get_redo_location($root_node->data_dir);
		$replica->safe_psql($regress_db, 'checkpoint;');
		$replica->polar_get_redo_location($root_node->data_dir);

		%cur_log_index_meta = get_log_index_meta($root_node);

		if (int($cur_log_index_meta{'seg.no'}) >
			int($prev_log_index_meta{'seg.no'}))
		{
			my $lsn = $prev_log_index_meta{'seg.max_lsn'};
			my $all_slot_catchup = 1;
			foreach my $wait_slot (@wait_slots)
			{
				my $ret = $root_node->safe_psql($regress_db,
					"select '$lsn'::pg_lsn > restart_lsn from pg_replication_slots where slot_name = '$wait_slot';"
				);
				if ($ret eq 'f')
				{
					$all_slot_catchup = 0;
					last;
				}
			}
			if ($all_slot_catchup)
			{
				$catchup = 1;
				last;
			}
			%prev_log_index_meta = %cur_log_index_meta;
		}
		print "prev logindex meta: seg.no="
		  . $prev_log_index_meta{'seg.no'}
		  . ", seg.max_lsn="
		  . $prev_log_index_meta{'seg.max_lsn'} . "\n";
		print "cur logindex meta: seg.no="
		  . $cur_log_index_meta{'seg.no'}
		  . ", seg.max_lsn="
		  . $cur_log_index_meta{'seg.max_lsn'} . "\n";
		sleep 1;
	}

	return $catchup;
}

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;
$node_primary->append_conf('postgresql.conf', "wal_level='logical'");

my $node_replica = PostgreSQL::Test::Cluster->new('replica');
$node_replica->polar_init_replica($node_primary);

# add a standby and set it's root to replica
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->polar_init_standby($node_primary);

$node_primary->start;

$node_primary->polar_create_slot($node_replica->name);
$node_primary->polar_create_slot($node_standby->name);

$node_primary->safe_psql($regress_db, 'create extension polar_monitor;');
$node_primary->safe_psql($regress_db,
	"create table test(id int, name char(512) default 'A');");

# current physical slot's node type should be UNKNOWN
my $slot_name = $node_replica->name;
my $replica_slot_type = $node_primary->safe_psql($regress_db,
	"select polar_get_slot_node_type('$slot_name');");
is($replica_slot_type, 'UNKNOWN',
	'replica slot node type is UNKNOWN before starting replication');

$slot_name = $node_standby->name;
my $standby_slot_type = $node_primary->safe_psql($regress_db,
	"select polar_get_slot_node_type('$slot_name');");
is($standby_slot_type, 'UNKNOWN',
	'standby slot node type is UNKNOWN before starting replication');

# start replica node and check slot node type
$node_replica->start;
wait_replication_catchup($node_primary, $node_replica);

$slot_name = $node_replica->name;
$replica_slot_type = $node_primary->safe_psql($regress_db,
	"select polar_get_slot_node_type('$slot_name');");
is($replica_slot_type, 'REPLICA',
	'replica slot node type is REPLICA after starting replication');

# start standby node and check slot node type
$node_standby->start;
wait_replication_catchup($node_primary, $node_standby);

$slot_name = $node_standby->name;
$standby_slot_type = $node_primary->safe_psql($regress_db,
	"select polar_get_slot_node_type('$slot_name');");
is($standby_slot_type, 'STANDBY',
	'standby slot node type is STANDBY after starting replication');

# shutdown standby node and check slot node type
$node_standby->stop;

$slot_name = $node_standby->name;
$standby_slot_type = $node_primary->safe_psql($regress_db,
	"select polar_get_slot_node_type('$slot_name');");
is($standby_slot_type, 'STANDBY',
	'standby slot node type is STANDBY after standby shutdown');

# shutdown replica node and check slot node type
$node_replica->stop;

$slot_name = $node_replica->name;
$replica_slot_type = $node_primary->safe_psql($regress_db,
	"select polar_get_slot_node_type('$slot_name');");
is($replica_slot_type, 'REPLICA',
	'replica slot node type is REPLICA after replica shutdown');

# restart primary node and check slot node type
$node_primary->stop('i');
$node_primary->start;

$slot_name = $node_replica->name;
$replica_slot_type = $node_primary->safe_psql($regress_db,
	"select polar_get_slot_node_type('$slot_name');");
is($replica_slot_type, 'UNKNOWN',
	'replica slot node type is UNKNOWN after primary restart and before starting replication'
);

$slot_name = $node_standby->name;
$standby_slot_type = $node_primary->safe_psql($regress_db,
	"select polar_get_slot_node_type('$slot_name');");
is($standby_slot_type, 'UNKNOWN',
	'standby slot node type is UNKNOWN after primary restart and before starting replication'
);

# start replica and standby
$node_replica->start;
wait_replication_catchup($node_primary, $node_replica);
$node_standby->start;
wait_replication_catchup($node_primary, $node_standby);

# create logical slot
my $logical_slot = 'logical1';
$node_primary->safe_psql($regress_db,
	"SELECT * FROM pg_create_logical_replication_slot('$logical_slot', 'test_decoding');"
);
my $restart_lsn_1 = $node_primary->safe_psql($regress_db,
	"select restart_lsn from pg_replication_slots where slot_name = '$logical_slot';"
);
my $ret = $node_primary->safe_psql($regress_db,
	"select '$restart_lsn_1'::pg_lsn != '0/0'::pg_lsn;");
is($ret, 't', "succ to create logical slot");

# stop standby's startup and test for logindex recycle
$node_primary->safe_psql($regress_db, 'checkpoint;');
$node_standby->stop_child('walreceiver');

$ret = wait_for_logindex_meta_catchup($node_primary, $node_replica,
	($node_standby->name, $logical_slot));
is($ret, 1, "logindex recycle ignored standby's slot and logical slot");

$node_standby->resume_child('walreceiver');
wait_replication_catchup($node_primary, $node_standby);

# create logical slot, stop logical slot and then test for logindex recycle
my $restart_lsn_2 = $node_primary->safe_psql($regress_db,
	"select restart_lsn from pg_replication_slots where slot_name = '$logical_slot';"
);
$ret = $node_primary->safe_psql($regress_db,
	"select '$restart_lsn_1'::pg_lsn != '$restart_lsn_2'::pg_lsn;");
is($ret, 'f', "logical slot's restart lsn hasn't ever been changed");

$node_primary->stop;
$node_replica->stop;
$node_standby->stop;

done_testing();
