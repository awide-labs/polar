#!/usr/bin/perl

# polar_login_history.pl
#
# IDENTIFICATION
#	  external/polar_login_history/t/polar_login_history.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

############### init primary/replica/standby ##############
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;
$node_primary->append_conf('postgresql.conf',
	"shared_preload_libraries = 'polar_login_history'");

my $node_replica = PostgreSQL::Test::Cluster->new('replica');
$node_replica->polar_init_replica($node_primary);

my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->polar_init_standby($node_primary);

$node_primary->start;

$node_primary->polar_create_slot($node_replica->name);
$node_primary->polar_create_slot($node_standby->name);

$node_replica->start;
$node_standby->start;

$node_standby->polar_drop_all_slots;


############### enable login history ##############
$node_primary->append_conf('postgresql.conf',
	"polar_login_history.enable = on");
$node_primary->restart;
is( $node_primary->safe_psql('postgres', 'show polar_login_history.enable;'),
	'on',
	'login history function is enabled');

$node_replica->append_conf('postgresql.conf',
	"polar_login_history.enable = on");
$node_replica->restart;
is( $node_replica->safe_psql('postgres', 'show polar_login_history.enable;'),
	'on',
	'login history function is enabled');

$node_standby->append_conf('postgresql.conf',
	"polar_login_history.enable = on");
$node_standby->restart;
is( $node_standby->safe_psql('postgres', 'show polar_login_history.enable;'),
	'on',
	'login history function is enabled');


############### log in to the database with the new user ##############
$node_primary->safe_psql('postgres', 'create user zhangsan;');
$node_replica->restart;

is( $node_primary->psql(
		'postgres', undef, extra_params => [ '-U', 'zhangsan' ]),
	0,
	'login success');
isnt(
	$node_primary->psql(
		'postgres1', undef, extra_params => [ '-U', 'zhangsan' ]),
	0,
	'login success');

is( $node_replica->psql(
		'postgres', undef, extra_params => [ '-U', 'zhangsan' ]),
	0,
	'login success');
isnt(
	$node_replica->psql(
		'postgres1', undef, extra_params => [ '-U', 'zhangsan' ]),
	0,
	'login success');

is( $node_standby->psql(
		'postgres', undef, extra_params => [ '-U', 'zhangsan' ]),
	0,
	'login success');
isnt(
	$node_standby->psql(
		'postgres1', undef, extra_params => [ '-U', 'zhangsan' ]),
	0,
	'login success');

$node_primary->safe_psql('postgres', 'drop user zhangsan;');


############### stop ##############
$node_primary->stop;
$node_replica->stop;
$node_standby->stop;
done_testing();
