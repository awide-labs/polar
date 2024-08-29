# 010_polar_replica_copydata_when_start.pl
#	  Test for init local dir from shared storage when replica starts
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
#	  src/test/polar_consistency/t/010_polar_replica_copydata_when_start.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use PolarDB::DCRegression;

# primary
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;

$node_primary->append_conf('postgresql.conf', "wal_keep_size = 10GB");
$node_primary->start;
$node_primary->safe_psql('postgres', 'CREATE TABLE test_table(val integer);');
$node_primary->safe_psql('postgres',
	'INSERT INTO test_table(val) SELECT generate_series(1, 100)');

my $result =
  $node_primary->safe_psql('postgres', 'SELECT count(*) FROM test_table');
ok($result == 100, "primary insert data success");

### 1 test create new replica node
my $node_replica = PostgreSQL::Test::Cluster->new('replica');
$node_replica->polar_init_replica($node_primary);
$node_primary->polar_create_slot($node_replica->name);
$node_primary->append_conf('postgresql.conf',
	"synchronous_standby_names='" . $node_replica->name . "'");
$node_replica->append_conf('postgresql.conf', "wal_keep_size = 10GB");
$node_primary->restart;
$node_replica->start;

# check whether walstreaming between primary and replica is normal
my $walpid = $node_replica->wait_walstreaming_establish_timeout(20);
print "walreceiver pid of replica: $walpid\n";
ok($walpid != 0, "walstream between primary and replica is normal");

$node_primary->wait_for_catchup($node_replica);
$result =
  $node_replica->safe_psql('postgres', 'SELECT count(*) FROM test_table');
ok($result == 100, "replica copydata and replay success");

### 2 test promote replica and demote primary
# drop unnecessary replication slot
$node_replica->stop;
$node_primary->polar_drop_slot($node_replica->name);
$node_primary->stop;
$node_replica->start;
$node_replica->promote;
$node_replica->polar_wait_for_startup(60, 0);

# create new data to check data consistency after promote
$node_replica->safe_psql('postgres',
	'INSERT INTO test_table(val) SELECT generate_series(101, 200)');
$result =
  $node_replica->safe_psql('postgres', 'SELECT count(*) FROM test_table');
ok($result == 200, "promoted replica and insert data success");
$node_replica->polar_create_slot($node_primary->name);
$node_primary->polar_replica_set_recovery($node_replica);
$node_primary->start;
# check whether walstreaming between primary and replica1 is normal after promote and demote
$walpid = $node_primary->wait_walstreaming_establish_timeout(20);
print "walreceiver pid of primary: $walpid\n";
ok($walpid != 0, "walstream between primary and replica is normal");
# check whether primary replay the wal successfully
$node_replica->wait_for_catchup($node_primary);
$result =
  $node_primary->safe_psql('postgres', 'SELECT count(*) FROM test_table');
ok($result == 200, "demoted primary and replay data success");

### 3 test replica data is more than primary
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->polar_init_standby($node_replica);

$node_replica->polar_create_slot($node_standby->name);
$node_standby->polar_standby_set_recovery($node_replica);
$node_standby->start;

$node_replica->wait_for_catchup($node_standby);
$result =
  $node_standby->safe_psql('postgres', 'SELECT count(*) FROM test_table');
ok($result == 200, "standby replay data success");

$node_primary->stop;
$node_standby->stop;
$node_replica->polar_drop_slot($node_primary->name);
$node_replica->polar_drop_slot($node_standby->name);
$node_replica->safe_psql('postgres',
	'INSERT INTO test_table(val) SELECT generate_series(201, 400)');
$result =
  $node_replica->safe_psql('postgres', 'SELECT count(*) FROM test_table');
ok($result == 400, "new primary insert data success");
$node_replica->safe_psql('postgres', 'checkpoint');
$node_replica->stop;

# prmote standby
$node_standby->append_conf('postgresql.conf', "wal_keep_size = 10GB");
$node_standby->start;
$node_standby->promote;
$node_standby->polar_wait_for_startup(60, 0);
$result =
  $node_standby->safe_psql('postgres', 'SELECT count(*) FROM test_table');
print "new primary result: $result\n";
ok($result == 200, "promote standby success");

$node_standby->polar_create_slot($node_replica->name);
# remove polar_node_static.conf so that polar_datadir of replica can be changed
my $replica_basedir = $node_replica->data_dir;
PostgreSQL::Test::Utils::system_or_bail('rm', '-f',
	"$replica_basedir/polar_node_static.conf");
# copy original shared storage pg_control to local
my $replica_datadir = $node_replica->polar_get_datadir;
PostgreSQL::Test::Utils::system_or_bail(
	'cp',
	"$replica_datadir/global/pg_control",
	"$replica_basedir/global/pg_control");
my $replica_redoptr =
  $node_replica->polar_get_redo_location($replica_basedir);
my $primary_redoptr =
  $node_standby->polar_get_redo_location($node_standby->polar_get_datadir);
print
  "new primary_redoptr: $primary_redoptr, replica_redoptr:$replica_redoptr\n";

$node_replica->polar_replica_set_recovery($node_standby);
$node_replica->start;
$walpid = $node_replica->wait_walstreaming_establish_timeout(20);
print "walreceiver pid of new replica: $walpid\n";
ok($walpid != 0, "walstream between new primary and replica is normal");
# check whether new replica replay the wal successfully
$node_standby->wait_for_catchup($node_replica);
$result =
  $node_replica->safe_psql('postgres', 'SELECT count(*) FROM test_table');
print "new replica result: $result\n";
ok($result == 200, "new replica start success");

$node_standby->safe_psql('postgres',
	'INSERT INTO test_table(val) SELECT generate_series(201, 400)');
$result =
  $node_standby->safe_psql('postgres', 'SELECT count(*) FROM test_table');
print "new primary insert result: $result\n";
ok($result == 400, "new primary insert data success");
$node_standby->wait_for_catchup($node_replica);
$result =
  $node_replica->safe_psql('postgres', 'SELECT count(*) FROM test_table');
print "new replica result: $result\n";
ok($result == 400, "new replica replay data success");

$node_replica->stop;
$node_standby->stop;
done_testing();
