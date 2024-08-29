# 002_improper_prepared_stmt.pl
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
#	  src/test/polar_pl/t/002_improper_prepared_stmt.pl

# Test monitoring improper use of prepared statement. The prepared and extended queries
# cannot be tested by pg_regress. So we have to test them by pgbench in perl.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use Test::More;

my $node_primary;
my $logstart;
my $old_simple_cnt;
my $new_simple_cnt;
my $old_unnamed_cnt;
my $new_unnamed_cnt;
my $old_unparam_cnt;
my $new_unparam_cnt;

# Initialize primary node
$node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init();
$node_primary->start;

# Init tables
$node_primary->pgbench_init(dbname => 'postgres');

# 1. SimpleProtocolExecCount
$old_simple_cnt = $node_primary->safe_psql('postgres',
	"SELECT value FROM polar_feature_utils.polar_unique_feature_usage WHERE name = 'SimpleProtocolExecCount';"
);
$old_unnamed_cnt = $node_primary->safe_psql('postgres',
	"SELECT value FROM polar_feature_utils.polar_unique_feature_usage WHERE name = 'UnnamedStmtExecCount';"
);
$old_unparam_cnt = $node_primary->safe_psql('postgres',
	"SELECT value FROM polar_feature_utils.polar_unique_feature_usage WHERE name = 'UnparameterizedStmtExecCount';"
);
# Execute a simple query
$node_primary->safe_psql('postgres', "SELECT 1;");

$new_simple_cnt = $node_primary->safe_psql('postgres',
	"SELECT value FROM polar_feature_utils.polar_unique_feature_usage WHERE name = 'SimpleProtocolExecCount';"
);
$new_unnamed_cnt = $node_primary->safe_psql('postgres',
	"SELECT value FROM polar_feature_utils.polar_unique_feature_usage WHERE name = 'UnnamedStmtExecCount';"
);
$new_unparam_cnt = $node_primary->safe_psql('postgres',
	"SELECT value FROM polar_feature_utils.polar_unique_feature_usage WHERE name = 'UnparameterizedStmtExecCount';"
);
diag("SimpleProtocolExecCount old: $old_simple_cnt, new: $new_simple_cnt");
ok( $old_simple_cnt < $new_simple_cnt,
	"SimpleProtocolExecCount shoud increase");
diag("UnnamedStmtExecCount old: $old_unnamed_cnt, new: $new_unnamed_cnt");
ok($old_unnamed_cnt == $new_unnamed_cnt,
	"UnnamedStmtExecCount shoud not increase with simple query");
diag(
	"UnparameterizedStmtExecCount old: $old_unparam_cnt, new: $new_unparam_cnt"
);
ok($old_unparam_cnt == $new_unparam_cnt,
	"UnparameterizedStmtExecCount shoud not increase with simple query");


# 2. UnnamedStmtExecCount
$old_simple_cnt = $node_primary->safe_psql('postgres',
	"SELECT value FROM polar_feature_utils.polar_unique_feature_usage WHERE name = 'SimpleProtocolExecCount';"
);
$old_unnamed_cnt = $node_primary->safe_psql('postgres',
	"SELECT value FROM polar_feature_utils.polar_unique_feature_usage WHERE name = 'UnnamedStmtExecCount';"
);
$old_unparam_cnt = $node_primary->safe_psql('postgres',
	"SELECT value FROM polar_feature_utils.polar_unique_feature_usage WHERE name = 'UnparameterizedStmtExecCount';"
);
# Execute extended select-only queries with pgbench, 1s is enough
$node_primary->pgbench_test(
	dbname => 'postgres',
	client => 1,
	job => 1,
	time => 1,
	script => 'select-only',
	protocol => 'extended',
	extra => '-n');
$new_simple_cnt = $node_primary->safe_psql('postgres',
	"SELECT value FROM polar_feature_utils.polar_unique_feature_usage WHERE name = 'SimpleProtocolExecCount';"
);
$new_unnamed_cnt = $node_primary->safe_psql('postgres',
	"SELECT value FROM polar_feature_utils.polar_unique_feature_usage WHERE name = 'UnnamedStmtExecCount';"
);
$new_unparam_cnt = $node_primary->safe_psql('postgres',
	"SELECT value FROM polar_feature_utils.polar_unique_feature_usage WHERE name = 'UnparameterizedStmtExecCount';"
);
# The query to select polar_unique_feature_usage is a simple query
diag("SimpleProtocolExecCount old: $old_simple_cnt, new: $new_simple_cnt");
ok( $old_simple_cnt < $new_simple_cnt,
	"SimpleProtocolExecCount shoud increase");
diag("UnnamedStmtExecCount old: $old_unnamed_cnt, new: $new_unnamed_cnt");
ok($old_unnamed_cnt < $new_unnamed_cnt,
	"UnnamedStmtExecCount shoud increase with extended select-only queries");
diag(
	"UnparameterizedStmtExecCount old: $old_unparam_cnt, new: $new_unparam_cnt"
);
ok( $old_unparam_cnt == $new_unparam_cnt,
	"UnparameterizedStmtExecCount shoud not increase with extended select-only queries"
);


# 3. UnparameterizedStmtExecCount
$old_simple_cnt = $node_primary->safe_psql('postgres',
	"SELECT value FROM polar_feature_utils.polar_unique_feature_usage WHERE name = 'SimpleProtocolExecCount';"
);
$old_unnamed_cnt = $node_primary->safe_psql('postgres',
	"SELECT value FROM polar_feature_utils.polar_unique_feature_usage WHERE name = 'UnnamedStmtExecCount';"
);
$old_unparam_cnt = $node_primary->safe_psql('postgres',
	"SELECT value FROM polar_feature_utils.polar_unique_feature_usage WHERE name = 'UnparameterizedStmtExecCount';"
);
# Execute prepared tpcb-like queries with pgbench, 1s is enough
$node_primary->pgbench_test(
	dbname => 'postgres',
	client => 1,
	job => 1,
	time => 1,
	script => 'tpcb-like',
	protocol => 'prepared',
	extra => '-n');
$new_simple_cnt = $node_primary->safe_psql('postgres',
	"SELECT value FROM polar_feature_utils.polar_unique_feature_usage WHERE name = 'SimpleProtocolExecCount';"
);
$new_unnamed_cnt = $node_primary->safe_psql('postgres',
	"SELECT value FROM polar_feature_utils.polar_unique_feature_usage WHERE name = 'UnnamedStmtExecCount';"
);
$new_unparam_cnt = $node_primary->safe_psql('postgres',
	"SELECT value FROM polar_feature_utils.polar_unique_feature_usage WHERE name = 'UnparameterizedStmtExecCount';"
);
# The query to select polar_unique_feature_usage is a simple query
diag("SimpleProtocolExecCount old: $old_simple_cnt, new: $new_simple_cnt");
ok( $old_simple_cnt < $new_simple_cnt,
	"SimpleProtocolExecCount shoud increase");
diag("UnnamedStmtExecCount old: $old_unnamed_cnt, new: $new_unnamed_cnt");
ok( $old_unnamed_cnt == $new_unnamed_cnt,
	"UnnamedStmtExecCount shoud not increase with prepared tpcb-like queries"
);
diag(
	"UnparameterizedStmtExecCount old: $old_unparam_cnt, new: $new_unparam_cnt"
);
ok( $old_unparam_cnt < $new_unparam_cnt,
	"UnparameterizedStmtExecCount shoud increase with prepared tpcb-like queries"
);


# 4. DeallocateStmtExecCount
# We are unable to test it here because it can only be tested by JDBC.
# It's ok because the logic is simple.


done_testing();
