
#!/usr/bin/perl

# 006_test_polar_param_bind_audit_log.pl
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
#	  src/test/polar_pl/t/006_test_polar_param_bind_audit_log.pl

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
        polar_enable_syslog_pipe_buffer = on
        polar_audit_log_flush_timeout = 1000
		logging_collector = on
		log_statement = all
        polar_enable_multi_syslogger = true
        log_destination = 'polar_multi_dest'
	]
);

$node_primary->start;
my $data_dir = $node_primary->data_dir();
my $log_dir = "$data_dir\/log";

$node_primary->pgbench(
	'-i', 0,
	[qr{^$}],
	[
		qr{creating tables},
		qr{vacuuming},
		qr{creating primary keys},
		qr{done in \d+\.\d\d s }
	],
	'pgbench scale 1 initialization',);

$node_primary->pgbench(
	'--transactions=20 --client=1 -M extended --builtin=si -C --no-vacuum -s 1',
	0,
	[
		qr{builtin: simple update},
		qr{clients: 1\b},
		qr{threads: 1\b},
		qr{processed: 20/20},
		qr{mode: extended}
	],
	[qr{scale option ignored}],
	'pgbench simple update');

my $result =
  qx{/bin/bash -c "if [ `ls $log_dir | wc -l` != \'0\' ] ; then cat `ls -t $log_dir/*audit* | head -1` | tail -3 | head -1; fi"};
print "bind test result: " . $result;

ok(substr($result, 0, 7) eq 'params:', "bind test pass");

# done with the node
$node_primary->stop;

done_testing();
