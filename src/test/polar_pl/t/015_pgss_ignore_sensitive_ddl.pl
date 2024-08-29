#!/usr/bin/perl

# 015_pgss_ignore_sensitive_ddl.pl
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
#	  src/test/polar_pl/t/015_pgss_ignore_sensitive_ddl.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $role_super = 'regress_super';
my $role_non_super = 'regress_non_super';

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);
$node_primary->append_conf('postgresql.conf',
	qq(shared_preload_libraries = 'pg_stat_statements'));
# track all user's statements no matter who this is.
$node_primary->append_conf('postgresql.conf',
	'pg_stat_statements.enable_superuser_track = on');

# do not ignore any sensitive statements like community does
$node_primary->append_conf('postgresql.conf',
	'pg_stat_statements.ignore_sensitive_ddl = off');
$node_primary->start;

$node_primary->safe_psql('postgres', 'CREATE EXTENSION pg_stat_statements');

$node_primary->safe_psql('postgres',
	qq(CREATE ROLE $role_super LOGIN SUPERUSER PASSWORD '123456'));
$node_primary->safe_psql('postgres',
	qq(CREATE ROLE $role_non_super LOGIN PASSWORD '123456'));
$node_primary->safe_psql('postgres',
	qq(ALTER ROLE $role_super PASSWORD '654321'));
$node_primary->safe_psql('postgres',
	qq(ALTER ROLE $role_non_super PASSWORD '654321'));
$node_primary->safe_psql('postgres', qq(DROP ROLE $role_super));
$node_primary->safe_psql('postgres', qq(DROP ROLE $role_non_super));
$node_primary->safe_psql('postgres', 'CHECKPOINT');

my $cnt = $node_primary->safe_psql('postgres',
	q(SELECT COUNT(*) FROM pg_stat_statements WHERE query LIKE '% PASSWORD %')
);
is($cnt, 4, "sensitive DDL is tracked as expected");
$cnt = $node_primary->safe_psql('postgres',
	q(SELECT COUNT(*) FROM pg_stat_statements WHERE query LIKE '%CHECKPOINT%')
);
is($cnt, 1, "not sensitive DDL is tracked as expected");
$node_primary->safe_psql('postgres', 'SELECT pg_stat_statements_reset()');

$node_primary->stop('fast');
$node_primary->append_conf('postgresql.conf',
	'pg_stat_statements.ignore_sensitive_ddl = on');
$node_primary->start;

$node_primary->safe_psql('postgres',
	qq(CREATE ROLE $role_super LOGIN SUPERUSER PASSWORD '123456'));
$node_primary->safe_psql('postgres',
	qq(CREATE ROLE $role_non_super LOGIN PASSWORD '123456'));
$node_primary->safe_psql('postgres',
	qq(ALTER ROLE $role_super PASSWORD '654321'));
$node_primary->safe_psql('postgres',
	qq(ALTER ROLE $role_non_super PASSWORD '654321'));
$node_primary->safe_psql('postgres', qq(DROP ROLE $role_super));
$node_primary->safe_psql('postgres', qq(DROP ROLE $role_non_super));
$node_primary->safe_psql('postgres', 'CHECKPOINT');

$cnt = $node_primary->safe_psql('postgres',
	q(SELECT COUNT(*) FROM pg_stat_statements WHERE query LIKE '% PASSWORD %')
);
is($cnt, 0, "sensitive DDL is ignored as expected");
$cnt = $node_primary->safe_psql('postgres',
	q(SELECT COUNT(*) FROM pg_stat_statements WHERE query LIKE '%CHECKPOINT%')
);
is($cnt, 1, "not sensitive DDL is tracked as expected");
$node_primary->safe_psql('postgres', 'SELECT pg_stat_statements_reset()');

# done with the node
$node_primary->stop;

done_testing();
