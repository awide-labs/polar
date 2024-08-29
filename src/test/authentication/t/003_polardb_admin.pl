# 003_polardb_admin.pl
#	  Test polardb_admin database connection
#
# Copyright (c) 2022, Alibaba Group Holding Limited
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
#	  src/test/authentication/t/003_polardb_admin.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Delete pg_hba.conf from the given node, add a new entry to it
# and then execute a reload to refresh it.
sub reset_pg_hba
{
	my $node = shift;
	my $hba_method = shift;

	unlink($node->data_dir . '/pg_hba.conf');
	$node->append_conf('pg_hba.conf', "local all all $hba_method");
	$node->reload;
	return;
}

# Test access for a single role, useful to wrap all tests into one.
sub test_login
{
	my $node = shift;
	my $role = shift;
	my $datname = shift;
	my $expected_res = shift;
	my $status_string = 'failed';

	$status_string = 'success' if ($expected_res eq 0);

	my $res = $node->psql($datname, undef, extra_params => [ '-U', $role ]);
	is($res, $expected_res,
		"authentication $status_string for role $role to login $datname.");
	return;
}

# Initialize master node.
my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init();
$node->start;

# Create test roles.
$node->safe_psql(
	'postgres',
	"CREATE ROLE polardb_test_1 LOGIN;
     CREATE ROLE polardb_test_2 LOGIN;
     CREATE ROLE polardb_super_1 SUPERUSER LOGIN;
     CREATE USER polardb_test_3 LOGIN;
     CREATE USER polardb_test_4 LOGIN;
     CREATE USER polardb_super_2 SUPERUSER LOGIN;");

reset_pg_hba($node, 'trust');

# Test access.
test_login($node, 'polardb_test_1', "polardb_admin", 2);
test_login($node, 'polardb_test_2', "polardb_admin", 2);
test_login($node, 'polardb_test_3', "polardb_admin", 2);
test_login($node, 'polardb_test_4', "polardb_admin", 2);
test_login($node, 'polardb_super_1', "polardb_admin", 0);
test_login($node, 'polardb_super_2', "polardb_admin", 0);

done_testing();
