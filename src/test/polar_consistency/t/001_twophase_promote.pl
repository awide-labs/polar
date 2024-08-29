# 001_twophase_promote.pl
#	  Test promote replica when there's twophase
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
#	  src/test/polar_consistency/t/001_twophase_promote.pl
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $sql_file = $ENV{PWD} . '/t/001_twophase_promote.sql';
my $db = 'postgres';
my $timeout = 1000;

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;

my $node_replica = PostgreSQL::Test::Cluster->new('replica1');
$node_replica->polar_init_replica($node_primary);

$node_primary->start;

$node_primary->polar_create_slot($node_replica->name);
$node_replica->start;

open my $fh, '<', $sql_file or die "Failed to open $sql_file: $!";
my $sql = '';

while (my $line = <$fh>)
{
	$sql .= $line;
}

$node_primary->safe_psql($db, $sql);
$node_primary->stop;

$node_replica->promote;

# wait for new primary to startup
$node_replica->polar_wait_for_startup(30);
$node_replica->polar_drop_all_slots;

$node_replica->safe_psql($db, "COMMIT PREPARED 'twophase_test';");

my $result = $node_replica->safe_psql($db, "SELECT MAX(v) FROM twophase;");
chomp($result);
ok($result == 10000, "Replica is promoted");

$result = $node_replica->safe_psql($db, "SELECT COUNT(*) FROM twophase;");
chomp($result);
ok($result == 91, "All data are visible");

$node_replica->stop;
done_testing();
