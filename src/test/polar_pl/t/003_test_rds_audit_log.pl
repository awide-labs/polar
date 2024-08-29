
#!/usr/bin/perl

# 003_test_rds_audit_log.pl
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
#	  src/test/polar_pl/t/003_test_rds_audit_log.pl

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
		log_line_prefix = '\\1\\n\\t%p\\t%r\\t%u\\t%d\\t%t\\t%e\\t%T\\t%S\\t%U\\t%E\\t\\t'
		logging_collector = on
		log_statement = 'all'
        log_min_duration_statement = '1000'
        polar_enable_syslog_pipe_buffer = false
        polar_enable_syslog_file_buffer = false
        polar_enable_multi_syslogger = true
        log_destination = 'polar_multi_dest'
	]
);

$node_primary->start;
my $data_dir = $node_primary->data_dir();
my $log_dir = "$data_dir\/log";

$node_primary->safe_psql('postgres', "BEGIN;");
$node_primary->safe_psql('postgres', "SELECT 1;");
$node_primary->safe_psql('postgres', "COMMIT;");

my $result =
  qx{/bin/bash -c "if [ `ls $log_dir | wc -l` != \'0\' ] ; then cat `ls -t $log_dir/*audit* | head -1` | tail -1 | head -1; fi"};
print($result);

my @array = split(/\t/, $result);

ok($array[7] =~ /^\d+$/, "audit log check digit pass");
ok($array[8] =~ /^\d+$/, "audit log check digit pass");
ok($array[9] =~ /^\d+$/, "audit log check digit pass");
ok($array[10] =~ /^\d+$/, "audit log check digit pass");

$node_primary->append_conf(
	'postgresql.conf', q[
		logging_collector = off
	]
);
$node_primary->restart;
$node_primary->safe_psql('postgres', "SELECT 1;");
$result =
  qx{/bin/bash -c "if [ `ls $log_dir | wc -l` != \'0\' ] ; then cat `ls -t $log_dir/*audit* | head -1` | tail -1 | head -1; fi"};
print("flag off result" . $result);
ok(index($result, "SELECT 1") == -1, "audit log flag off pass");


$node_primary->append_conf(
	'postgresql.conf', q[
		logging_collector = on
	]
);

$node_primary->restart;
$node_primary->safe_psql('postgres', "SELECT 1;");
$node_primary->safe_psql('postgres',
	"create user test_password_user password \'12345678\'");
$result =
  qx{/bin/bash -c "if [ `ls $log_dir | wc -l` != \'0\' ] ; then cat `ls -t $log_dir/*audit* | head -1` | tail -1 | head -1; fi"};
print($result);
ok(index($result, "password") == -1, "audit log hide password");
ok(index($result, "12345678") == -1, "audit log hide password");

my $substring = "123";
my $long_text_sql = "SELECT " . (($substring . ",") x 1024) . $substring;
$node_primary->safe_psql('postgres', $long_text_sql);
$result =
  qx{/bin/bash -c "if [ `ls $log_dir | wc -l` != \'0\' ] ; then cat `ls -t $log_dir/*audit* | head -1` | tail -1 | head -1; fi"};
print($result);

my $count = 0;
$count++ while $result =~ /\Q$substring\E/g;
print("123 count:" . $count);
ok($count < 1024, "audit log shrink success");

$node_primary->append_conf(
	'postgresql.conf', q[
		log_min_duration_statement = 0
	]
);
$node_primary->restart;
$substring = "1";
$long_text_sql = "SELECT " . (($substring . ",") x 1024) . $substring;
$node_primary->safe_psql('postgres', $long_text_sql);
$result =
  qx{/bin/bash -c "if [ `ls $log_dir | wc -l` != \'0\' ] ; then cat `ls -t $log_dir/*slow* | head -1` | tail -1 | head -1; fi"};
print($result);
ok(index($result, "duration") > 0, "slow log find sql pass");

$node_primary->append_conf(
	'postgresql.conf', q[
		log_min_duration_statement = 1000
	]
);

$node_primary->restart;
$node_primary->psql(
	'postgres',
	"SELECT * FROM non_exist_table",
	on_error_stop => 0);
$result =
  qx{/bin/bash -c "if [ `ls $log_dir | wc -l` != \'0\' ] ; then cat `ls -t $log_dir/*audit* | head -1` | tail -1 | head -1; fi"};
print("error log find in audit result: " . $result);
ok(index($result, "non_exist_table") > 0, "error log find in audit");

$node_primary->append_conf(
	'postgresql.conf', q[
		polar_auditlog_max_query_length = 2048
	]
);

$node_primary->restart;
$substring = "1";
$long_text_sql = "SELECT " . ($substring x 2048);
$node_primary->safe_psql('postgres', $long_text_sql);
$result =
  qx{/bin/bash -c "if [ `ls $log_dir | wc -l` != \'0\' ] ; then cat `ls -t $log_dir/*audit* | head -1` | tail -1 | head -1; fi"};

$count = 0;
$count++ while $result =~ /\Q$substring\E/g;
print("1 count:" . $count);
ok($count > 1800 && $count < 2048, "audit log default shrink success");

$node_primary->append_conf(
	'postgresql.conf', q[
		polar_auditlog_max_query_length = 32000
	]
);

$node_primary->restart;
$long_text_sql = "SELECT " . ($substring x 32000);
$node_primary->safe_psql('postgres', $long_text_sql);
$result =
  qx{/bin/bash -c "if [ `ls $log_dir | wc -l` != \'0\' ] ; then cat `ls -t $log_dir/*audit* | head -1` | tail -1 | head -1; fi"};

$count = 0;
$count++ while $result =~ /\Q$substring\E/g;
print("1 count:" . $count);
ok($count > 31000 && $count <= 32000, "audit log shrink success");

$node_primary->safe_psql('postgres',
	"ALTER SYSTEM SET polar_auditlog_max_query_length to DEFAULT; SELECT pg_reload_conf(); SELECT pg_sleep(1);"
);

$node_primary->append_conf(
	'postgresql.conf', q[
		log_rotation_size = 1
	]
);

qx{/bin/bash -c "rm -rf $log_dir/*"};
my $log_count = qx{/bin/bash -c "ls $log_dir | wc -l"};
ok($log_count == 0, "log remove init ok");

$node_primary->restart;

$substring = "2";
$long_text_sql = "SELECT " . (($substring . ",") x 1024) . "1";

$node_primary->safe_psql('postgres', $long_text_sql);
$result = qx{/bin/bash -c "ls $log_dir | wc -l"};
print("log dir log number1: " . $result);
sleep 1;

$node_primary->safe_psql('postgres', $long_text_sql);
sleep 1;
$node_primary->safe_psql('postgres', "select 1;");

my $retry = 10;
my $pass = 0;
while (1)
{
	$log_count = qx{/bin/bash -c "ls $log_dir | wc -l"};
	print("log dir log number2: " . $log_count);

	if (($log_count - $result) >= 1)
	{
		$pass = 1;
	}

	if ($retry == 0 || $pass == 1)
	{
		last;
	}

	$retry = $retry - 1;
	sleep 1;
}
ok($pass == 1, "log rotate pass");

qx{/bin/bash -c "rm -rf $log_dir/*"};
$log_count = qx{/bin/bash -c "ls $log_dir | wc -l"};
ok($log_count == 0, "log remove init ok");

$node_primary->append_conf(
	'postgresql.conf', q[
		polar_max_auditlog_files = 10
		polar_max_slowlog_files = 10
		polar_max_log_files = 10
	]
);
$node_primary->restart;

my $total_times = 400;
my $audit_log_num = 0;
my $total_log_num = 0;
my $is_remove = 1;

while (1)
{
	$node_primary->safe_psql('postgres', $long_text_sql);
	$audit_log_num = qx{/bin/bash -c "ls $log_dir/*audit* | wc -l"};
	$total_log_num = qx{/bin/bash -c "ls $log_dir | wc -l"};

	$node_primary->safe_psql('postgres', "select pg_sleep(0.1)");

	if ($audit_log_num > 13 || $total_log_num > 33)
	{
		print(  "audit log numner: "
			  . $audit_log_num
			  . " total log number: "
			  . $total_log_num);
		$is_remove = 0;
	}

	$total_times = $total_times - 1;

	if ($total_times == 0 || $is_remove == 0)
	{
		last;
	}
}

ok($is_remove == 1, "log remove pass");

# done with the node
$node_primary->stop;

done_testing();
