# 001_test_local_cache.pl
#	  Test for local cache module
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
#	  src/test/modules/test_local_cache/t/001_test_local_cache.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# primary node
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;

$node_primary->append_conf(
	'postgresql.conf', q[
    max_worker_processes=32
    shared_preload_libraries='test_local_cache'
    restart_after_crash=off
    logging_collector=off
]);
$node_primary->start;

my $regress_db = 'postgres';
$node_primary->safe_psql($regress_db, 'CREATE EXTENSION test_local_cache;');
my $ret = $node_primary->safe_psql($regress_db, 'select test_local_cache();');

is($ret, '0', 'succ to execute test_local_cache()!');

$node_primary->stop;
done_testing();
