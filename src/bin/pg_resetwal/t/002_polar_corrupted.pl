# 002_polar_corrupted.pl
#	  Tests for handling a corrupted pg_control
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
#	  src/bin/pg_resetwal/t/002_polar_corrupted.pl

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan tests => 18;

# create primary
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;
# create replica1
my $node_replica1 = PostgreSQL::Test::Cluster->new('replica1');
$node_replica1->polar_init_replica($node_primary);
# create standby1
my $node_standby1 = PostgreSQL::Test::Cluster->new('standby1');
$node_standby1->polar_init_standby($node_primary);
# primary config
$node_primary->append_conf('postgresql.conf',
		"synchronous_standby_names='"
	  . $node_replica1->name . ","
	  . $node_standby1->name
	  . "'");
$node_primary->append_conf('postgresql.conf', 'wal_sender_timeout=3600s');
# replica1 config
$node_replica1->append_conf('postgresql.conf', 'wal_receiver_timeout=3600s');
# standby1 config
$node_standby1->append_conf('postgresql.conf', 'wal_receiver_timeout=3600s');
# start primary
$node_primary->start;
# create slots needed by replica1 and standby1
$node_primary->polar_create_slot($node_replica1->name);
$node_primary->polar_create_slot($node_standby1->name);
# start replica1
$node_replica1->start;
# start standby1
$node_standby1->start;
$node_standby1->polar_drop_all_slots;

$node_primary->stop;
$node_replica1->stop;
$node_standby1->stop;

my ($pg_control, $size, $data, $fh, $pg_control_bak, $sysret);
my @nodes = ($node_primary, $node_replica1, $node_standby1);
foreach my $node (@nodes)
{
	$pg_control = $node->polar_get_datadir . '/global/pg_control';
	$size = (stat($pg_control))[7];
	# backup
	$pg_control_bak = $pg_control . '_bak';
	$sysret = `cp -rp $pg_control $pg_control_bak`;
	# Read out the head of the file to get PG_CONTROL_VERSION in
	# particular.
	open $fh, '<', $pg_control or BAIL_OUT($!);
	binmode $fh;
	read $fh, $data, 16;
	close $fh;
	# Fill pg_control with zeros
	open $fh, '>', $pg_control or BAIL_OUT($!);
	binmode $fh;
	print $fh pack("x[$size]");
	close $fh;
	command_checks_all(
		[ 'pg_resetwal', '-n', $node->data_dir ],
		0,
		[qr/pg_control version number/],
		[
			qr/pg_resetwal: warning: pg_control exists but is broken or wrong version; ignoring it/
		],
		'processes corrupted pg_control all zeroes');

	# Put in the previously saved header data.  This uses a different code
	# path internally, allowing us to process a zero WAL segment size.
	open $fh, '>', $pg_control or BAIL_OUT($!);
	binmode $fh;
	print $fh $data, pack("x[" . ($size - 16) . "]");
	close $fh;

	command_checks_all(
		[ 'pg_resetwal', '-n', $node->data_dir ],
		0,
		[qr/pg_control version number/],
		[
			qr/\Qpg_resetwal: warning: pg_control specifies invalid WAL segment size (0 bytes); proceed with caution\E/
		],
		'processes zero WAL segment size');
	# restore
	$sysret = `mv $pg_control_bak $pg_control`;
}
