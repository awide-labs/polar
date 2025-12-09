
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
        polar_syslogger_num = 1
        log_destination = 'polar_multi_dest'
	]
);

$node_primary->start;
my $data_dir = $node_primary->data_dir();
my $log_dir = "$data_dir\/log";
sub audit_log_count
{
	my ($pattern) = @_;
	my @files = glob("$log_dir/*audit*");
	my $count = 0;

	for my $file (@files)
	{
		next unless -f $file;
		open my $fh, '<', $file or next;
		while (my $line = <$fh>)
		{
			$count++ if index($line, $pattern) != -1;
		}
		close $fh;
	}

	return $count;
}

sub wait_for_audit_log
{
	my ($pattern, $predicate, $timeout) = @_;
	$timeout //= 30;
	my $count = 0;

	for (1 .. $timeout)
	{
		$count = audit_log_count($pattern);
		last if $predicate->($count);
		sleep 1;
	}

	return $count;
}

sub clear_logs
{
	system('/bin/bash', '-c', "rm -rf $log_dir/*");
}

sub log_file_count
{
	my @files = glob("$log_dir/*");
	return scalar(@files);
}


$node_primary->psql(
	'postgres',
	"SELECT * FROM non_exist_table;" . ("select pg_sleep(1);" x 2),
	on_error_stop => 0);

my $count = wait_for_audit_log("non_exist_table", sub { $_[0] > 0 });
ok($count > 0, "audit log contains errors: $count");

clear_logs();
my $log_count = log_file_count();
ok($log_count == 0, "log remove init ok");

$node_primary->append_conf(
	'postgresql.conf', q[
        polar_enable_error_to_audit_log = off
	]
);

$node_primary->restart;
$node_primary->psql(
	'postgres',
	"SELECT * FROM non_exist_table;" . ("select pg_sleep(1);" x 2),
	on_error_stop => 0);

$count = wait_for_audit_log("non_exist_table", sub { $_[0] > 0 }, 1);
ok($count == 0, "audit log suppressed when disabled: $count");

clear_logs();
$log_count = log_file_count();
ok($log_count == 0, "log remove init ok");

$node_primary->append_conf(
	'postgresql.conf', q[
        polar_enable_error_to_audit_log = on
		log_statement = none
	]
);

$node_primary->restart;
$node_primary->psql(
	'postgres',
	"SELECT * FROM non_exist_table;" . ("select pg_sleep(1);" x 2),
	on_error_stop => 0);

$count = wait_for_audit_log("non_exist_table", sub { $_[0] > 0 }, 1);
ok($count == 0, "log_statement none hides SQL: $count");

clear_logs();
$log_count = log_file_count();
ok($log_count == 0, "log remove init ok");

$node_primary->append_conf(
	'postgresql.conf', q[
        polar_enable_error_to_audit_log = on
		log_statement = ddl
		log_line_prefix = '\\1\\n\\t%p\\t%r\\t%u\\t%d\\t%t\\t%e\\t%T\\t%S\\t%U\\t%E\\t\\t'
	]
);

$node_primary->restart;
$node_primary->psql(
	'postgres',
	"SELECT * FROM non_exist_table;" . ("select pg_sleep(1);" x 2),
	on_error_stop => 0);
$count = wait_for_audit_log("42P01", sub { $_[0] > 0 });
ok($count == 1, "error code logged once: $count");
# done with the node
$node_primary->stop;

done_testing();
