# 002_promote_kill.pl
#	  Promote replica and then remount fs with incorrect mode after oom
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
#	  src/test/polar_consistency/t/002_promote_kill.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $db = 'postgres';
my $timeout = 1000;

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;

my $node_replica = PostgreSQL::Test::Cluster->new('replica1');
$node_replica->polar_init_replica($node_primary);

$node_replica->append_conf('postgresql.conf', 'restart_after_crash=on');

$node_primary->start;

$node_primary->polar_create_slot($node_replica->name);
$node_replica->start;

my $result = $node_primary->safe_psql('postgres', "select 1");
chomp($result);
ok($result == 1, "Primary is running");

$result = $node_replica->safe_psql('postgres', "select 1");
chomp($result);
ok($result == 1, "Replica is running");

$node_primary->stop;
$node_replica->promote;

# wait for new primary to startup
$node_replica->polar_wait_for_startup(30);
$node_replica->polar_drop_all_slots;
# kill background writer after replica is running after promote
$node_replica->kill_child('background writer');

# wait for new primary to startup
$result = $node_replica->polar_wait_for_startup(30);
ok($result == 1, "Fix remount fs after replica is promoted and crash");

$node_replica->stop;
done_testing();
