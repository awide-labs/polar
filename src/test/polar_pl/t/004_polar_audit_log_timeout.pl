
#!/usr/bin/perl

# 004_polar_audit_log_timeout.pl
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
#	  src/test/polar_pl/t/004_polar_audit_log_timeout.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use threads;

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);

$node_primary->append_conf(
	'postgresql.conf', q[
		logging_collector = on
		log_statement = 'all'
        log_min_duration_statement = '1000'
        polar_enable_syslog_pipe_buffer = True
        polar_enable_syslog_file_buffer = false
        polar_audit_log_flush_timeout = 3600000
        polar_enable_multi_syslogger = true
        log_destination = 'polar_multi_dest'
	]
);

$node_primary->start;
my $data_dir = $node_primary->data_dir();
my $log_dir = "$data_dir\/log";

my $content_number = 10;

while (1)
{
	$node_primary->safe_psql('postgres', "select 5432");
	$content_number = $content_number - 1;

	if ($content_number == 0)
	{
		last;
	}
}

sleep(0.001);
my $result = qx{/bin/bash -c 'grep -wrn "select 5432" $log_dir | wc -l'};
print("select 5432 count: " . $result);

ok($result == 10, "flush exit success");


qx{/bin/bash -c "rm -rf $log_dir/*"};
my $log_count = qx{/bin/bash -c "ls $log_dir | wc -l"};
ok($log_count == 0, "log remove init ok");

$node_primary->restart;

$content_number = 10;
while (1)
{
	$node_primary->safe_psql('postgres', "select 5432");
	$content_number = $content_number - 1;

	if ($content_number == 0)
	{
		last;
	}
}

my $pid = $node_primary->safe_psql('postgres', "select pg_backend_pid()");
qx{/bin/bash -c "kill $pid"};

sleep(0.001);
$result = qx{/bin/bash -c 'grep -wrn "select 5432" $log_dir | wc -l'};
print("select 5432 count: " . $result);

ok($result == 10, "flush kill success");

qx{/bin/bash -c "rm -rf $log_dir/*"};
$log_count = qx{/bin/bash -c "ls $log_dir | wc -l"};
ok($log_count == 0, "log remove init ok");

$node_primary->restart;

$content_number = 10;
while (1)
{
	$node_primary->safe_psql('postgres', "select 5432");
	$content_number = $content_number - 1;

	if ($content_number == 0)
	{
		last;
	}
}
$node_primary->stop;

sleep(0.001);
$result = qx{/bin/bash -c 'grep -wrn "select 5432" $log_dir | wc -l'};
print("select 5432 count: " . $result);

ok($result == 10, "flush master stop success");

qx{/bin/bash -c "rm -rf $log_dir/*"};
$log_count = qx{/bin/bash -c "ls $log_dir | wc -l"};
ok($log_count == 0, "log remove init ok");

$node_primary->append_conf(
	'postgresql.conf', q[
        polar_audit_log_flush_timeout = 5000
	]
);

$node_primary->restart;

$node_primary->pgbench(
	'-n -t 10 -c10 -j10 -M simple',
	0,
	[
		qr{type: .*/001_pgbench_custom_script},
		qr{processed: 100/100},
		qr{mode: simple}
	],
	[qr{^$}],
	'pgbench custom script',
	{
		'001_pgbench_custom_script' => q{-- select 5432
        select 5432;
}
	});

sleep(0.001);
$result = qx{/bin/bash -c 'grep -wrn "select 5432" $log_dir | wc -l'};
print("select 5432 count: " . $result);

ok($result == 100, "flush parallel success");

# done with the node
$node_primary->stop;

done_testing();
