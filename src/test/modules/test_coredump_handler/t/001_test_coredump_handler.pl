# 001_test_coredump_handler.pl
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
#	  src/test/modules/test_coredump_handler/t/001_test_coredump_handler.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use Test::More;

if ($ENV{with_libunwind} ne 'yes')
{
	plan skip_all => 'coredump handler not supported by this build';
}

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init;
$node_primary->start;

$node_primary->safe_psql('postgres',
	'create extension test_coredump_handler');

sub test_coredump_handler
{
	my ($sql, $expect_ret) = @_;
	my ($ret, $stdout, $stderr) =
	  $node_primary->psql('postgres', $sql, on_error_stop => 0);

	is($stdout, '');

	if ($expect_ret eq 3)
	{
		is($ret, 2);
		like($stderr, qr/server closed the connection unexpectedly/);
		$node_primary->restart;
	}
	elsif ($expect_ret eq 2)
	{
		is($ret, 2);
		like($stderr, qr/server closed the connection unexpectedly/);
		like($stderr, qr/FATAL.*Ignore coredump signal/);
	}
	else
	{
		is($ret, $expect_ret);
		like($stderr, qr/ERROR.*Ignore coredump signal/);
	}
}

# without coredump handler
test_coredump_handler('select test_sigsegv()', 3);
test_coredump_handler('select test_panic()', 3);

# with coredump handler, but without fuzzy match
$node_primary->append_conf('postgresql.conf',
	"polar_ignore_coredump_functions = 'test_panic'");
$node_primary->reload;
test_coredump_handler('select test_sigsegv()', 3);
test_coredump_handler('select test_panic()', 2);

$node_primary->append_conf('postgresql.conf',
	"polar_ignore_coredump_functions = 'test_sigsegv'");
$node_primary->reload;
test_coredump_handler('select test_sigsegv()', 2);
test_coredump_handler('select test_panic()', 3);

# with coredump handler and fuzzy match
$node_primary->append_conf('postgresql.conf',
	"polar_ignore_coredump_functions = 'exec_simple_query'");
$node_primary->append_conf('postgresql.conf',
	"polar_ignore_coredump_fuzzy_match = on");
$node_primary->reload;
test_coredump_handler('select test_sigsegv()', 2);
test_coredump_handler('select test_panic()', 2);

# with coredump handler and fuzzy match, and make it ERROR
$node_primary->append_conf('postgresql.conf',
	"polar_ignore_coredump_level = 'ERROR'");
$node_primary->reload;
test_coredump_handler('select test_sigsegv()', 0);
test_coredump_handler('select test_panic()', 0);

done_testing();
