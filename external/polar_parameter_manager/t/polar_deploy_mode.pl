#!/usr/bin/perl

# polar_deploy_mode.pl
#	  Tests deploy in different mode
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
#	  external/polar_parameter_manager/t/polar_deploy_mode.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Config;
use PolarDB::DCRegression;

if ($Config{osname} eq 'MSWin32')
{

	# some Windows Perls at least don't like IPC::Run's start/kill_kill regime.
	plan skip_all => "Test fails on Windows perl";
}

############### define check function ##############
sub check_consistency
{
	my ($compatibility_mode, $deploy_mode) = @_;

	my $special_conf = 'huge_pages = try';

	############### init master/replica/standby ##############

	my $node_master = PostgreSQL::Test::Cluster->new('primary');
	$node_master->polar_init_primary(
		extra => [
			# "--compatibility_mode=$compatibility_mode", POLAR_TODO
			"--deploy-mode=$deploy_mode"
		]);
	$node_master->append_conf('postgresql.conf', $special_conf);

	my $node_replica = PostgreSQL::Test::Cluster->new('replica');
	$node_replica->polar_init_replica($node_master);

	my $node_standby = PostgreSQL::Test::Cluster->new('standby');
	$node_standby->polar_init_standby($node_master);

	$node_master->start;
	$node_master->polar_create_slot($node_replica->name);
	$node_master->polar_create_slot($node_standby->name);

	$node_replica->start;
	$node_standby->start;

	$node_standby->polar_drop_all_slots;

	############### Check Settings Format Correct ##############
	is( $node_master->safe_psql(
			'postgres',
			qq[select count(*) from (select name from pg_file_settings where sourcefile like '%polar_settings.conf' group by name having count(*) != 1) as s;]
		),
		0,
		"Duplicate settings in polar_setting.conf.$compatibility_mode.$deploy_mode.sample before command"
	);

	## check cluster set command run ok ##
	$node_master->safe_psql('postgres',
		q[ALTER SYSTEM FOR CLUSTER SET archive_command TO '';]);
	$node_master->safe_psql('postgres',
		q[ALTER SYSTEM FOR CLUSTER SET archive_command TO '';]);

	is( $node_master->safe_psql(
			'postgres',
			qq[select count(*) from (select name from pg_file_settings where sourcefile like '%polar_settings.conf' group by name having count(*) != 1) as s;]
		),
		0,
		"Duplicate settings in polar_setting.conf.$compatibility_mode.$deploy_mode.sample after command"
	);

	############### Check Data Correct ##############
	$node_master->safe_psql('postgres', q[DROP TABLE IF EXISTS public.t;]);
	$node_master->safe_psql('postgres',
		q[CREATE TABLE public.t(a INT,b VARCHAR,c NUMERIC);]);
	$node_master->safe_psql('postgres',
		q[INSERT INTO public.t select i,i::varchar,i::numeric from generate_series(1,1000)i;]
	);

	# Wait until node 3 has connected and caught up
	my $lsn = $node_master->lsn('insert');
	$node_master->wait_for_catchup('replica', 'replay', $lsn);
	$node_master->wait_for_catchup('standby', 'replay', $lsn);

	is( $node_master->safe_psql(
			'postgres', qq[SELECT count(*) from public.t]),
		1000,
		"data not correct in master");

	is( $node_replica->safe_psql(
			'postgres', qq[SELECT count(*) from public.t]),
		1000,
		"data not correct in replica");

	is( $node_standby->safe_psql(
			'postgres', qq[SELECT count(*) from public.t]),
		1000,
		"data not correct in standby");

	############### stop ##############
	$node_master->stop;
	$node_replica->stop;
	$node_standby->stop;

	$node_master->clean_node;
	$node_replica->clean_node;
	$node_standby->clean_node;
}
############### test consistency in differnt deploy mode ##############
check_consistency("pg", "cloud");
# check_consistency("ora", "cloud"); POLAR_TODO
check_consistency("pg", "apclocal");
# check_consistency("ora", "apclocal"); POLAR_TODO
check_consistency("pg", "apcshared");
# check_consistency("ora", "apcshared"); POLAR_TODO
done_testing();
