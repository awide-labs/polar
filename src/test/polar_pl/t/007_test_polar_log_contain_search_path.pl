
#!/usr/bin/perl

# 007_test_polar_log_contain_search_path.pl
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
#	  src/test/polar_pl/t/007_test_polar_log_contain_search_path.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use DBI;

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);

$node_primary->append_conf(
	'postgresql.conf', q[
		log_line_prefix = '\\1\\n\\tpolarpg_v1\\t%p\\t%r\\t%u\\t%d\\t%t\\t%e\\t%T\\t%S\\t%U\\t%E\\t\\t'
		logging_collector = on
		log_statement = 'ddl'
        log_min_duration_statement = '1000'
        polar_enable_syslog_pipe_buffer = false
        polar_enable_syslog_file_buffer = false
        polar_enable_output_search_path_to_log = true
        polar_enable_error_to_audit_log = true
        polar_enable_multi_syslogger = true
        polar_syslogger_num = 1
        log_destination = 'polar_multi_dest'
	]
);

$node_primary->start;
my $data_dir = $node_primary->data_dir();
my $log_dir = "$data_dir\/log";

sub wait_for_log_entry
{
	my ($pattern, $log_type, $tail_lines) = @_;
	$tail_lines = 1 unless defined $tail_lines;
	my $max_retries = 30;
	my $retry = 0;
	my $result = '';

	while ($retry < $max_retries)
	{
		$result = qx{/bin/bash -c "if [ `ls $log_dir | wc -l` != \'0\' ] ; then cat `ls -t $log_dir/*${log_type}* | head -1` | tail -${tail_lines} | head -1; fi"};
		if (index($result, $pattern) >= 0)
		{
			return $result;
		}
		sleep(1);
		$retry++;
	}
	return $result;
}

# simple query to audit log
$node_primary->safe_psql('postgres', "CREATE TABLE log_contain1(i int);");
my $result = wait_for_log_entry("CREATE TABLE log_contain1(i int);", "audit");
print("simple query to audit log result: " . $result);
ok( index($result, "CREATE TABLE log_contain1(i int);") > 0
	  && index($result, "\/*\"\$user\", public*\/") > 0,
	"simple query to audit log pass");

# simple query to slow log
$node_primary->safe_psql('postgres', "SELECT pg_sleep(2);");
$result = wait_for_log_entry("SELECT pg_sleep(2);", "slow");
print("simple query to slow log result: " . $result);
ok( index($result, "SELECT pg_sleep(2);") > 0
	  && index($result, "\/*\"\$user\", public*\/") > 0,
	"simple query to slow log pass");

# error msg to audit log
$node_primary->psql('postgres', "SELECT 1/0;", on_error_stop => 0);
$result = wait_for_log_entry("SELECT 1/0;", "audit");
print("error msg to audit log result: " . $result);
ok( index($result, "SELECT 1/0;") > 0
	  && index($result, "\/*\"\$user\", public*\/") > 0,
	"error msg to audit log pass");

$node_primary->pgbench(
	'-n -t1 -c1 -j1 -M extended',
	0,
	[
		qr{type: .*/001_pgbench_custom_script},
		qr{processed: 1/1},
		qr{mode: extend}
	],
	[qr{^$}],
	'pgbench custom script',
	{
		'001_pgbench_custom_script' => q{-- select pg_sleep(1.5)
        select pg_sleep(1.5);
}
	});

# execute query to slow log
$result = wait_for_log_entry("select pg_sleep(1.5);", "slow", 2);
print("execute query to slow log result: " . $result);
ok( index($result, "select pg_sleep(1.5);") > 0
	  && index($result, "\/*\"\$user\", public*\/") > 0,
	"execute query to slow log pass");

$node_primary->pgbench(
	'-n -t1 -c1 -j1 -M extended',
	0,
	[
		qr{type: .*/002_pgbench_custom_script},
		qr{processed: 1/1},
		qr{mode: extend}
	],
	[qr{^$}],
	'pgbench custom script',
	{
		'002_pgbench_custom_script' => q{-- CREATE TABLE log_contain2(i int);
        CREATE TABLE log_contain2(i int);
}
	});

# execute query to audit log
$result = wait_for_log_entry("CREATE TABLE log_contain2(i int);", "audit", 2);
print("execute query to audit log result: " . $result);
ok( index($result, "CREATE TABLE log_contain2(i int);") > 0
	  && index($result, "\/*\"\$user\", public*\/") > 0,
	"execute query to audit log pass");

# done with the node
$node_primary->stop;

done_testing();
