# 000_test_logindex.pl
#	  Test for logindex test module
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
#	  src/test/modules/test_logindex/t/000_test_logindex.pl

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
    polar_enable_slru_hash_index=off
    polar_logindex_mem_size=64MB
    polar_xlog_queue_buffers=64MB
    shared_preload_libraries = 'test_logindex'
    max_worker_processes=32
    logging_collector=off
    restart_after_crash=off
]);

$node_primary->start;

my $regress_db = 'postgres';
$node_primary->safe_psql($regress_db, 'CREATE EXTENSION test_logindex;');

my $ret = $node_primary->safe_psql($regress_db, 'select test_logindex();');
is($ret, '0', 'succ to execute test_logindex()!');

$ret = $node_primary->safe_psql($regress_db, 'select test_bitpos();');
is($ret, '0', 'succ to execute test_bitpos()!');

$ret = $node_primary->safe_psql($regress_db, 'select test_ringbuf();');
is($ret, '0', 'succ to execute test_ringbuf()!');

$ret = $node_primary->safe_psql($regress_db, 'select test_mini_trans();');
is($ret, '0', 'succ to execute test_mini_trans()!');

$ret = $node_primary->safe_psql($regress_db, 'select test_fullpage();');
is($ret, '0', 'succ to execute test_fullpage()!');

$ret = $node_primary->safe_psql($regress_db,
	'select test_polar_rel_size_cache();');
is($ret, '0', 'succ to execute test_polar_rel_size_cache()!');

$ret =
  $node_primary->safe_psql($regress_db, 'select test_checkpoint_ringbuf();');
is($ret, '0', 'succ to execute test_checkpoint_ringbuf()!');

$node_primary->stop;
done_testing();
