# 003_promote_replica_pending.pl
#	  Test promote replica when it's in snapshot pending state
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
#	  src/test/polar_consistency/t/003_promote_replica_pending.pl
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $sql_file = $ENV{PWD} . '/t/003_uncommit_trans.sql';
my $db = 'postgres';
my $timeout = 120;

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;

my $node_replica = PostgreSQL::Test::Cluster->new('replica1');
$node_replica->polar_init_replica($node_primary);

$node_primary->start;

$node_primary->polar_create_slot($node_replica->name);
$node_replica->start;

my $primary_psql = {};
$node_primary->psql_connect($db, $timeout, _psql => $primary_psql);

open my $fh, '<', $sql_file or die "Failed to open $sql_file: $!";
my $sql = '';

while (my $line = <$fh>)
{
	$sql .= $line;
}

my $primary_output =
  $node_primary->psql_execute($sql, _psql => $primary_psql);
my $checkpoint_psql = {};
$node_primary->psql_connect($db, $timeout, _psql => $checkpoint_psql);

$node_primary->psql_execute("checkpoint;", _psql => $checkpoint_psql);
$node_primary->psql_close(_psql => $checkpoint_psql);

my $failed = $node_replica->restart_no_check;
ok($failed != 0, "Replica is in snapshot pending");

$node_primary->kill9;
$node_replica->promote;

# wait for new primary to startup
$node_replica->polar_wait_for_startup(30);
$node_replica->polar_drop_all_slots;

my $result = $node_replica->safe_psql('postgres', "select 1");
chomp($result);
ok($result == 1, "Replica is promoted");

$node_replica->stop;
done_testing();
