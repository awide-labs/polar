#!/usr/bin/perl

# 051_polar_multi_syslogger.pl
#
# Copyright (c) 2025, Alibaba Group Holding Limited
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
#	  src/test/polar_pl/t/051_polar_multi_syslogger.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);
$node_primary->append_conf(
	'postgresql.conf', q[
        polar_audit_log_flush_timeout = 1000
		logging_collector = on
		log_statement = all
        polar_enable_multi_syslogger = true
        polar_syslogger_num = 16
        log_destination = 'polar_multi_dest'
	]
);

$node_primary->start;
my $data_dir = $node_primary->data_dir();
my $log_dir = "$data_dir\/log";

my @sql_statements = (
	"CREATE TABLE test_table (id SERIAL PRIMARY KEY, name TEXT, value INTEGER);",
	"INSERT INTO test_table (name, value) VALUES ('test1', 100)",
	"INSERT INTO test_table (name, value) VALUES ('test2', 200)",
	"INSERT INTO test_table (name, value) VALUES ('test3', 300)",
	"SELECT * FROM test_table WHERE 1 = 1",
	"SELECT * FROM test_table WHERE value > 150",
	"UPDATE test_table SET value = 150 WHERE name = 'test1'",
	"DELETE FROM test_table WHERE value = 300");

foreach my $statement (@sql_statements)
{
	$node_primary->safe_psql('postgres', $statement);
}

# Check if all 16 loggers created log files
my @audit_files = glob("$log_dir/*audit.log");

# Should have 16 audit log files
is(scalar(@audit_files), 16, "Should have 16 auditlog files");

# Check specific log entries for each SQL statement
# Since audit logs are distributed across multiple loggers based on PID,
# we need to check all log files for each statement

sub statements_logged_once
{
	foreach my $statement (@sql_statements)
	{
		my $found = 0;
		foreach my $file (@audit_files)
		{
			open(my $fh, '<', $file) or die "Could not open file '$file': $!";
			while (my $line = <$fh>)
			{
				if (index($line, $statement) != -1)
				{
					$found += 1;
					# log level of this line should be LOG
					return -1 if index($line, 'LOG:  statement:') == -1;
				}
			}
			close($fh);
		}
		return 0 if $found == 0;
		return -1 if $found > 1;
	}

	return 1;
}

my $logged_once;
foreach (1 .. $PostgreSQL::Test::Utils::timeout_default)
{
	$logged_once = statements_logged_once();
	last if $logged_once != 0;
	sleep 1;
}
ok($logged_once == 1,
	"All statements should be found exactly once in audit logs");

done_testing();
