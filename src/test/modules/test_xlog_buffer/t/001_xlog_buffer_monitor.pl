use strict;
# 001_xlog_buffer_monitor.pl
#	  perl test case for xlog buffer monitor
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
#	  src/test/modules/test_xlog_buffer/t/001_xlog_buffer_monitor.pl

use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use PolarDB::DCRegression;
my $regress_db = 'postgres';

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;

my $node_replica = PostgreSQL::Test::Cluster->new('replica');
$node_replica->polar_init_replica($node_primary);

my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->polar_init_standby($node_primary);

$node_primary->append_conf('postgresql.conf',
	'polar_xlog_page_buffers = 8MB');
$node_replica->append_conf('postgresql.conf',
	'polar_xlog_page_buffers = 8MB');
$node_standby->append_conf('postgresql.conf',
	'polar_xlog_page_buffers = 8MB');

$node_primary->start;
$node_primary->polar_create_slot($node_replica->name);
$node_primary->polar_create_slot($node_standby->name);

$node_replica->start;
$node_standby->start;

$node_standby->polar_drop_all_slots;

my $result =
  $node_primary->safe_psql('postgres', "create extension polar_monitor;");

$node_primary->safe_psql('postgres',
	"create table test_xlog_buffer(i int); insert into test_xlog_buffer select generate_series(1,200000);"
);

$node_replica->safe_psql('postgres', "select * from test_xlog_buffer ;");

$node_primary->safe_psql('postgres', "update test_xlog_buffer set i = 1");
$node_primary->wait_for_catchup($node_replica);

$node_replica->safe_psql('postgres', "select * from test_xlog_buffer ;");

$result = $node_replica->safe_psql('postgres',
	"select hit_count > 0, io_count > 0, others_append_count > 0, startup_append_count >= 0 from polar_xlog_buffer_stat_info();"
);
is($result, qq(t|t|t|t), 'check replica');


$node_primary->safe_psql('postgres', "update test_xlog_buffer set i = 2");
$node_primary->wait_for_catchup($node_standby);

$node_standby->safe_psql('postgres', "select * from test_xlog_buffer ;");

$result = $node_standby->safe_psql('postgres',
	"select hit_count > 0, io_count >= 0, others_append_count >= 0, startup_append_count > 0 from polar_xlog_buffer_stat_info();"
);
is($result, qq(t|t|t|t), 'check standby');

$node_primary->stop();
$node_replica->stop();
$node_standby->stop;
done_testing();
