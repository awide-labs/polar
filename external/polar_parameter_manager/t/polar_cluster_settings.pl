#!/usr/bin/perl
# polar_cluster_settings.pl
#	  Tests ALTER SYSTEM [FOR CLUSTER] [RELOAD]
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
#	  external/polar_parameter_manager/t/polar_cluster_settings.pl

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
elsif ($ENV{enable_fault_injector} eq 'no')
{
	plan skip_all => 'Fault injector not supported by this build';
}

############### init master/replica/standby ##############
my $node_master = PostgreSQL::Test::Cluster->new('primary');
$node_master->polar_init_primary;
$node_master->append_conf('postgresql.conf', "restart_after_crash = true");

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

$node_master->safe_psql(
	'postgres',
	q[
create view polar_settings as 
	select name,setting,applied from pg_file_settings 
    where sourcefile like '%polar_settings.conf%';
	create extension faultinjector;]);


############### define check function ##############
sub check_consistency
{
	my ($log_prefix, $test_guc, $expected_value, $check_guc_in_file,
		$is_reload)
	  = @_;
	$is_reload = 1 unless defined($is_reload);    # default value


	if ($check_guc_in_file)
	{
		$node_master->poll_query_until(
			'postgres',
			qq[SELECT setting = '$expected_value' from polar_settings where name = '$test_guc';
		])
		  or die
		  "$log_prefix: Timed out waiting for $test_guc = $expected_value in polar_settings in master";

		$node_replica->poll_query_until(
			'postgres',
			qq[SELECT setting = '$expected_value' from polar_settings where name = '$test_guc';
		])
		  or die
		  "$log_prefix: Timed out waiting for $test_guc = $expected_value in polar_settings in replica";

		$node_standby->poll_query_until(
			'postgres',
			qq[SELECT setting = '$expected_value' from polar_settings where name = '$test_guc';
		])
		  or die
		  "$log_prefix: Timed out waiting for $test_guc = $expected_value in polar_settings in standby";
	}
	else
	{
		$node_master->poll_query_until(
			'postgres',
			qq[SELECT count(setting) = 0 from polar_settings where name = '$test_guc';
		])
		  or die
		  "$log_prefix: Timed out waiting for $test_guc not in polar_settings in master";
		$node_replica->poll_query_until(
			'postgres',
			qq[SELECT count(setting) = 0 from polar_settings where name = '$test_guc';
		])
		  or die
		  "$log_prefix: Timed out waiting for $test_guc not in polar_settings in replica";
		$node_standby->poll_query_until(
			'postgres',
			qq[SELECT count(setting) = 0 from polar_settings where name = '$test_guc';
		])
		  or die
		  "$log_prefix: Timed out waiting for $test_guc not in polar_settings in standby";
	}

	if ($is_reload)
	{
		$node_master->safe_psql('postgres', q[SELECT pg_reload_conf();]);
		$node_replica->safe_psql('postgres', q[SELECT pg_reload_conf();]);
		$node_standby->safe_psql('postgres', q[SELECT pg_reload_conf();]);
	}

	# wait
	sleep(1);

	is($node_master->safe_psql('postgres', qq[show $test_guc;]),
		$expected_value,
		"'$log_prefix': show $test_guc = $expected_value in master");

	is( $node_replica->safe_psql('postgres', qq[show $test_guc;]),
		$expected_value,
		"'$log_prefix': show $test_guc = $expected_value in replica");

	is( $node_standby->safe_psql('postgres', qq[show $test_guc;]),
		$expected_value,
		"'$log_prefix': show $test_guc = $expected_value in standby");
}
############### test consistency ##############
check_consistency("test debug_print_rewritten = off in the beginning",
	"debug_print_rewritten", "off", 0);

$node_master->safe_psql('postgres',
	q[ALTER SYSTEM FOR CLUSTER SET debug_print_rewritten = on;]);

check_consistency(
	"test debug_print_rewritten = on after ALTER SYSTEM FOR CLUSTER SET",
	"debug_print_rewritten", "on", 1);

$node_master->safe_psql('postgres',
	q[ALTER SYSTEM FOR CLUSTER RESET debug_print_rewritten;]);

check_consistency(
	"test debug_print_rewritten = off after ALTER SYSTEM FOR CLUSTER RESET",
	"debug_print_rewritten", "off", 0);
############### test consistency in any fault injector ##############
$node_master->safe_psql('postgres',
	q[ALTER SYSTEM FOR CLUSTER SET debug_print_rewritten = on;]);
$node_master->safe_psql(
	'postgres',
	q[	select inject_fault('all', 'reset');
		select inject_fault('POLAR ALTER SYSTEM FOR CLUSTER BEFORE WAL WRITE', 'panic');]
);
$node_master->psql(
	'postgres',
	'ALTER SYSTEM FOR CLUSTER SET debug_print_rewritten = off;',
	on_error_stop => 0);
check_consistency("test debug_print_rewritten = on if panic before WAL",
	"debug_print_rewritten", "on", 1);

# extra test alter force.
$node_master->safe_psql('postgres',
	q[ALTER SYSTEM FOR CLUSTER FORCE SET xxxx.xxxx = '30MB';]);
$node_master->safe_psql('postgres',
	q[ALTER SYSTEM FOR CLUSTER FORCE SET polar_xxx = '70MB';]);
$node_master->safe_psql('postgres',
	q[ALTER SYSTEM FOR CLUSTER RELOAD FORCE SET client_min_messages to 'error';]
);
#

$node_master->safe_psql(
	'postgres',
	q[	select inject_fault('all', 'reset');
		select inject_fault('POLAR ALTER SYSTEM FOR CLUSTER AFTER WAL WRITE', 'panic');]
);
$node_master->psql(
	'postgres',
	'ALTER SYSTEM FOR CLUSTER SET debug_print_rewritten = off;',
	on_error_stop => 0);

# extra check alter force.
check_consistency("test xxxx.xxxx = 30MB if panic before WAL",
	"xxxx.xxxx", "30MB", 1);
#
check_consistency("test debug_print_rewritten = on if panic after WAL",
	"debug_print_rewritten", "off", 1);

############### test alter system for cluster reload command ##############
$node_master->safe_psql('postgres',
	q[ALTER SYSTEM FOR CLUSTER RELOAD SET polar_enable_strategy_reject_buffer = off;]
);
check_consistency(
	"test polar_enable_strategy_reject_buffer = off if panic after WAL",
	"polar_enable_strategy_reject_buffer",
	"off", 1, 0);
############### test alter system reload command ##############
$node_master->safe_psql('postgres',
	q[ALTER SYSTEM RELOAD SET polar_enable_strategy_reject_buffer = on;]);
# wait handle sighup signal
sleep(1);
my $result = $node_master->safe_psql('postgres',
	q[show polar_enable_strategy_reject_buffer;]);
is($result, 'on', 'check master alter system reload');
$node_replica->safe_psql('postgres',
	q[ALTER SYSTEM RELOAD SET polar_enable_strategy_reject_buffer = on;]);
# wait handle sighup signal
sleep(1);
$result = $node_replica->safe_psql('postgres',
	q[show polar_enable_strategy_reject_buffer;]);
is($result, 'on', 'check replica alter system reload');
$node_standby->safe_psql('postgres',
	q[ALTER SYSTEM RELOAD SET polar_enable_strategy_reject_buffer = on;]);
# wait handle sighup signal
sleep(1);
$result = $node_standby->safe_psql('postgres',
	q[show polar_enable_strategy_reject_buffer;]);
is($result, 'on', 'check standby alter system reload');

############### stop ##############
$node_master->stop;
$node_standby->stop;
$node_replica->stop;
done_testing();
