#!/usr/bin/perl
# 001_smgrperf.pl
#	  Test smgrperf tool, for coverage.
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
#	  external/polar_smgrperf/t/001_smgrperf.pl

use strict;
use warnings;
use Config;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize primary node
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init();
$node_primary->append_conf('postgresql.conf', 'statement_timeout = 3s');
$node_primary->start;

$node_primary->safe_psql('postgres', 'CREATE EXTENSION polar_smgrperf');

my $stderr;

# Run smgrperf tests
$node_primary->psql(
	'postgres',
	qq[
		set polar_zero_extend_method to none;
		select polar_smgrperf_extend();
	],
	stderr => \$stderr);
like(
	$stderr,
	qr/ERROR:  canceling statement due to statement timeout/,
	'polar_smgrperf_extend canceled by statement timeout');
like($stderr, qr/INFO:  Summary:/, 'polar_smgrperf_extend (none) ok');

$node_primary->psql(
	'postgres',
	qq[
		set polar_zero_extend_method to bulkwrite;
		select polar_smgrperf_extend();
	],
	stderr => \$stderr);
like(
	$stderr,
	qr/ERROR:  canceling statement due to statement timeout/,
	'polar_smgrperf_extend canceled by statement timeout');
like($stderr, qr/INFO:  Summary:/, 'polar_smgrperf_extend (bulkwrite) ok');

$node_primary->psql(
	'postgres',
	qq[
		set polar_zero_extend_method to fallocate;
		select polar_smgrperf_extend();
	],
	stderr => \$stderr);
like(
	$stderr,
	qr/ERROR:  canceling statement due to statement timeout/,
	'polar_smgrperf_extend canceled by statement timeout');
like($stderr, qr/INFO:  Summary:/, 'polar_smgrperf_extend (fallocate) ok');

$node_primary->safe_psql('postgres',
	'set statement_timeout=0; select polar_smgrperf_prepare()');

$node_primary->psql(
	'postgres',
	'select polar_smgrperf_read()',
	stderr => \$stderr);
like(
	$stderr,
	qr/ERROR:  canceling statement due to statement timeout/,
	'polar_smgrperf_read canceled by statement timeout');
like($stderr, qr/INFO:  Summary:/, 'polar_smgrperf_read ok');

$node_primary->psql(
	'postgres',
	'select polar_smgrperf_write()',
	stderr => \$stderr);
like(
	$stderr,
	qr/ERROR:  canceling statement due to statement timeout/,
	'polar_smgrperf_write canceled by statement timeout');
like($stderr, qr/INFO:  Summary:/, 'polar_smgrperf_write ok');

$node_primary->psql(
	'postgres',
	'select polar_smgrperf_nblocks()',
	stderr => \$stderr);
like(
	$stderr,
	qr/ERROR:  canceling statement due to statement timeout/,
	'polar_smgrperf_nblocks canceled by statement timeout');
like($stderr, qr/INFO:  Summary:/, 'polar_smgrperf_nblocks ok');

$node_primary->safe_psql('postgres', 'select polar_smgrperf_cleanup()');

done_testing();
