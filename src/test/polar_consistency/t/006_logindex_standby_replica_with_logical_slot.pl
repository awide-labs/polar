# 006_logindex_standby_replica_with_logical_slot.pl
#	  Test case: one primary node, one replica node and one standby node.
#
# Copyright (c) 2022, Alibaba Group Holding Limited
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
#	  src/test/polar_consistency/t/006_logindex_standby_replica_with_logical_slot.pl
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use PolarDB::DCRegression;

my $sql_dir = $ENV{PWD} . '/sql';
my $regress_db = 'postgres';
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;

my $node_replica1 = PostgreSQL::Test::Cluster->new('replica1');
$node_replica1->polar_init_replica($node_primary);

my $node_standby1 = PostgreSQL::Test::Cluster->new('standby1');
$node_standby1->polar_init_standby($node_primary);

# special mode 1: wal_level = logical
$node_primary->append_conf('postgresql.conf', 'wal_level = logical');
$node_replica1->append_conf('postgresql.conf', 'wal_level = logical');
$node_standby1->append_conf('postgresql.conf', 'wal_level = logical');

$node_primary->append_conf('postgresql.conf', 'huge_pages=off');
$node_replica1->append_conf('postgresql.conf', 'huge_pages=off');
$node_standby1->append_conf('postgresql.conf', 'huge_pages=off');

$node_primary->start;
$node_primary->polar_create_slot($node_replica1->name);
$node_primary->polar_create_slot($node_standby1->name);
$node_replica1->start;
$node_standby1->start;
$node_standby1->polar_drop_all_slots;

# create logical slot test_logical
my $test_logical_slot = 'test_logical';
$node_primary->polar_create_slot($test_logical_slot, 'logical');

my @replicas = ($node_replica1, $node_standby1);
my $regress = DCRegression->create_new_test($node_primary);
$node_primary->safe_psql(
	$regress->regress_db, "
create or replace function create_test_logical_random(range integer)
returns boolean as \$\$
declare
	idx integer := 0;
	slotname text;
	slot_rec record;
begin
	while idx <> range * 2 loop
		slotname := 'test_logical_slot_' || round(random() * range);
		select into slot_rec * from pg_replication_slots where slot_name = slotname;
		if slot_rec.slot_name is null then
			select into slot_rec pg_create_logical_replication_slot(slotname, 'test_decoding');
			return true;
		end if;
		idx := idx + 1;
	end loop;
	return false;
end;
\$\$ language plpgsql;
");
$regress->register_random_pg_command('restart: standby1 replica1');
$regress->register_random_pg_command('promote: replica1');
$regress->register_random_pg_command('promote: standby1');
$regress->register_random_pg_command('polar_promote: standby1');
$regress->register_random_pg_command('polar_promote: replica1');
$regress->register_random_pg_command(
	"sql:select pg_drop_replication_slot(slot_name) from pg_replication_slots where slot_type = 'logical' and active = 'f' and slot_name like 'test_logical_%' limit 1;:primary"
);
$regress->register_random_pg_command(
	'sql:select create_test_logical_random(2);:primary');
$regress->register_random_pg_command(
	'sql:alter system set fsync = off; select pg_reload_conf();:primary');
$regress->register_random_pg_command(
	'sql:alter system set fsync = on; select pg_reload_conf();:primary');
print "random pg commands are: " . $regress->get_random_pg_command . "\n";

my $start_time = time();
for (my $i = 0; $i < 50; $i++)
{
	my $pg_control_command = $regress->get_one_random_pg_command;
	print "Get one random pg command : $pg_control_command\n";
	$regress->pg_command($pg_control_command, \@replicas);
	# Sometimes, node will cost more time to accept connections after some pg_ctl commands.
	$regress->wait_for_all_node(\@replicas);
	my $primary = $regress->node_primary;
	my ($stdout, $stderr);
	$primary->psql(
		'postgres',
		"select pg_logical_slot_get_changes('$test_logical_slot', NULL,NULL)",
		stdout => \$stdout,
		stderr => \$stderr,
		on_error_die => 1,
		on_error_stop => 1);
	ok( $stderr eq '',
		"$i: logical slot check succeed after $pg_control_command\nstdout: $stdout\nstderr: $stderr\n"
	);
}
my $cost_time = time() - $start_time;
note "Test cost time $cost_time";

my @nodes = ($node_primary, $node_replica1, $node_standby1);
$regress->shutdown_all_nodes(\@nodes);
done_testing();
