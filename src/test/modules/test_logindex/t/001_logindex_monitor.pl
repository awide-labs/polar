# 001_logindex_monitor.pl
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
#	  src/test/modules/test_logindex/t/001_logindex_monitor.pl
use strict;
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

$node_primary->start;
$node_primary->polar_create_slot($node_replica->name);
$node_primary->polar_create_slot($node_standby->name);

$node_replica->start;
$node_standby->start;

$node_standby->polar_drop_all_slots;

my $result =
  $node_primary->safe_psql('postgres', "create extension polar_monitor;");

$node_primary->safe_psql('postgres',
	"create table test_logindex(i int); insert into test_logindex select generate_series(1,200000);"
);

$node_replica->restart;

$node_replica->safe_psql('postgres', "select * from test_logindex ;");

$node_standby->safe_psql('postgres', "select * from test_logindex ;");

$node_primary->safe_psql('postgres', "update test_logindex set i = 1");
$node_primary->wait_for_catchup($node_replica);

$node_replica->safe_psql('postgres', "select * from test_logindex ;");

$node_standby->safe_psql('postgres', "select * from test_logindex ;");

$node_primary->safe_psql('postgres',
	"insert into test_logindex select generate_series(1,1000000);");

$node_primary->wait_for_catchup($node_replica);

$result = $node_primary->safe_psql('postgres',
	"select push_cnt > pop_cnt, free_up_cnt >= 0, total_written > total_read, send_phys_io_cnt >= 0, evict_ref_cnt >= 0 from polar_xlog_queue_stat_detail();"
);
is($result, qq(t|t|t|t|t), 'check 1');


$result = $node_replica->safe_psql('postgres',
	"select push_cnt > pop_cnt, free_up_cnt >= 0, total_written > total_read, send_phys_io_cnt >= 0, evict_ref_cnt >= 0 from polar_xlog_queue_stat_detail();"
);
is($result, qq(t|t|t|t|t), 'check 1');

$result = $node_standby->safe_psql('postgres',
	"select push_cnt > pop_cnt, free_up_cnt >= 0, total_written > total_read, send_phys_io_cnt >= 0, evict_ref_cnt >= 0 from polar_xlog_queue_stat_detail();"
);
is($result, qq(t|t|t|t|t), 'check 1');

$node_primary->teardown_node();

$node_replica->psql(
	'postgres',
	'select pg_promote();',
	on_error_die => 0,
	on_error_stop => 0);
$node_replica->polar_wait_for_startup(60);

$node_replica->safe_psql('postgres',
	"insert into test_logindex select generate_series(1,1000000);");

$result = $node_replica->safe_psql('postgres',
	"select push_cnt > pop_cnt, free_up_cnt >= 0, total_written > total_read, send_phys_io_cnt >= 0, evict_ref_cnt >= 0 from polar_xlog_queue_stat_detail();"
);
is($result, qq(t|t|t|t|t), 'check 1');

$node_replica->polar_drop_all_slots;
$node_replica->stop;

# promote standby
$node_standby->safe_psql('postgres', "select pg_promote();");
$node_standby->polar_wait_for_startup(60);
$node_standby->safe_psql('postgres',
	"insert into test_logindex select generate_series(1,1000000);");

$result = $node_standby->safe_psql('postgres',
	"select push_cnt > pop_cnt, free_up_cnt >= 0, total_written > total_read, send_phys_io_cnt >= 0, evict_ref_cnt >= 0 from polar_xlog_queue_stat_detail();"
);
is($result, qq(t|t|t|t|t), 'check 1');

$node_standby->stop;
done_testing();
