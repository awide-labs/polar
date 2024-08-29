# 002_polar_pg_controldata.pl
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
#	  src/bin/pg_controldata/t/002_polar_pg_controldata.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan tests => 31;

sub destroy_control_file
{
	my ($path) = @_;
	my $size = (stat($path))[7];

	open my $fh, '>', $path or BAIL_OUT($!);
	binmode $fh;

	# fill file with zeros
	print $fh pack("x[$size]");
	close $fh;
}

sub command_output
{
	my ($cmd) = @_;
	my ($stdout, $stderr);
	print("# Running: " . join(" ", @{$cmd}) . "\n");
	my $result = IPC::Run::run $cmd, '>', \$stdout, '2>', \$stderr;
	return ($stdout, $stderr);
}

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

# simple test
command_like([ 'pg_controldata', $node_primary->data_dir ],
	qr/checkpoint/, 'pg_controldata produces output');
command_like([ 'pg_controldata', $node_replica1->data_dir ],
	qr/checkpoint/, 'pg_controldata produces output');
command_like([ 'pg_controldata', $node_standby1->data_dir ],
	qr/checkpoint/, 'pg_controldata produces output');

# primary's result should not be same as replica1's result.
my ($primary_stdout, $primary_stderr) =
  command_output([ 'pg_controldata', $node_primary->data_dir ]);
my ($replica1_stdout, $replica1_stderr) =
  command_output([ 'pg_controldata', $node_replica1->data_dir ]);
is($primary_stderr, '', $node_primary->name . ": no stderr");
is($replica1_stderr, '', $node_replica1->name . ": no stderr");
ok( $primary_stdout ne $replica1_stdout,
	$node_primary->name
	  . "\'s result is not the same as "
	  . $node_replica1->name);

# stop nodes
$node_primary->stop;
$node_standby1->stop;
$node_replica1->stop;

# result should keep same after we destroy the local copy one.
destroy_control_file($node_primary->data_dir . '/global/pg_control');
destroy_control_file($node_replica1->data_dir . '/global/pg_control');
destroy_control_file($node_standby1->data_dir . '/global/pg_control');

# simple test
command_like([ 'pg_controldata', $node_primary->data_dir ],
	qr/checkpoint/, 'pg_controldata produces output');
command_like([ 'pg_controldata', $node_replica1->data_dir ],
	qr/checkpoint/, 'pg_controldata produces output');
command_like([ 'pg_controldata', $node_standby1->data_dir ],
	qr/checkpoint/, 'pg_controldata produces output');

# primary's result should be same as replica1's result.
($primary_stdout, $primary_stderr) =
  command_output([ 'pg_controldata', $node_primary->data_dir ]);
($replica1_stdout, $replica1_stderr) =
  command_output([ 'pg_controldata', $node_replica1->data_dir ]);
is($primary_stderr, '', $node_primary->name . ": no stderr");
is($replica1_stderr, '', $node_replica1->name . ": no stderr");
ok( $primary_stdout ne $replica1_stdout,
	$node_primary->name
	  . "\'s result is not the same as "
	  . $node_replica1->name);

# check with a corrupted pg_control

my $pg_control = $node_primary->polar_get_datadir . '/global/pg_control';
destroy_control_file($pg_control);
command_checks_all(
	[ 'pg_controldata', $node_primary->data_dir ],
	0,
	[
		qr/WARNING: Calculated CRC checksum does not match value stored in file/,
		qr/WARNING: invalid WAL segment size/
	],
	[qr/^$/],
	'pg_controldata with corrupted pg_control');

command_like([ 'pg_controldata', $node_replica1->data_dir ],
	qr/checkpoint/, 'pg_controldata produces output');


$node_primary->clean_node;
$node_standby1->clean_node;
$node_replica1->clean_node;
