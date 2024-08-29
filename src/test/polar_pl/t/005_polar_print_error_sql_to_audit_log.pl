
#!/usr/bin/perl

# 005_polar_print_error_sql_to_audit_log.pl
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
#	  src/test/polar_pl/t/005_polar_print_error_sql_to_audit_log.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use threads;
use threads::shared;

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);

$node_primary->append_conf(
	'postgresql.conf', q[
        polar_enable_error_to_audit_log = on
        polar_enable_syslog_pipe_buffer = off
        polar_audit_log_flush_timeout = 1000
		logging_collector = on
		log_statement = ddl
        polar_enable_multi_syslogger = true
        log_destination = 'polar_multi_dest'
	]
);

$node_primary->start;
my $data_dir = $node_primary->data_dir();
my $log_dir = "$data_dir\/log";
my $ret : shared;
$ret = 0;

sub check_log
{
	my $sleep_time = shift;
	my $pattern = shift;

	sleep $sleep_time;

	$ret =
	  qx{/bin/bash -c 'grep -wrn "non_exist_table" $log_dir/*audit* | wc -l'};

	print("thread result: " . $ret);
}


my $thr = threads->create('check_log', 1, "non_exist_table");
$node_primary->psql(
	'postgres',
	"SELECT * FROM non_exist_table;" . ("select pg_sleep(1);" x 2),
	on_error_stop => 0);
$thr->join();

ok($ret > 0, "thread check log success: $ret");
$ret = -1;

qx{/bin/bash -c "rm -rf $log_dir/*"};
my $log_count = qx{/bin/bash -c "ls $log_dir | wc -l"};
ok($log_count == 0, "log remove init ok");

$node_primary->append_conf(
	'postgresql.conf', q[
        polar_enable_error_to_audit_log = off
	]
);

$node_primary->restart;
$thr = threads->create('check_log', 1, "non_exist_table");
$node_primary->psql(
	'postgres',
	"SELECT * FROM non_exist_table;" . ("select pg_sleep(1);" x 2),
	on_error_stop => 0);
$thr->join();

ok($ret == 0, "thread check log success: $ret");
$ret = -1;

qx{/bin/bash -c "rm -rf $log_dir/*"};
$log_count = qx{/bin/bash -c "ls $log_dir | wc -l"};
ok($log_count == 0, "log remove init ok");

$node_primary->append_conf(
	'postgresql.conf', q[
        polar_enable_error_to_audit_log = on
		log_statement = none
	]
);

$node_primary->restart;
$thr = threads->create('check_log', 1, "non_exist_table");
$node_primary->psql(
	'postgres',
	"SELECT * FROM non_exist_table;" . ("select pg_sleep(1);" x 2),
	on_error_stop => 0);
$thr->join();

ok($ret == 0, "thread check log success: $ret");
$ret = -1;

qx{/bin/bash -c "rm -rf $log_dir/*"};
$log_count = qx{/bin/bash -c "ls $log_dir | wc -l"};
ok($log_count == 0, "log remove init ok");

$node_primary->append_conf(
	'postgresql.conf', q[
        polar_enable_error_to_audit_log = on
		log_statement = ddl
		log_line_prefix = '\\1\\n\\t%p\\t%r\\t%u\\t%d\\t%t\\t%e\\t%T\\t%S\\t%U\\t%E\\t\\t'
	]
);

$node_primary->restart;
$thr = threads->create('check_log', 1, "42P01");
$node_primary->psql(
	'postgres',
	"SELECT * FROM non_exist_table;" . ("select pg_sleep(1);" x 2),
	on_error_stop => 0);
$thr->join();

ok($ret == 1, "thread check log success: $ret");
# done with the node
$node_primary->stop;

done_testing();
