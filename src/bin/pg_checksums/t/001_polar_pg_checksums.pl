# 001_polar_pg_checksums.pl
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
#	  src/bin/pg_checksums/t/001_polar_pg_checksums.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan tests => 49;

sub destroy_file
{
	my ($path) = @_;
	my $size = (stat($path))[7];

	open my $fh, '>', $path or BAIL_OUT($!);
	binmode $fh;

	# fill file with zeros
	print $fh pack("S[$size]", (1 .. 256));
	close $fh;
	print "destroy file: $path\n";
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

# # create replica1
my $node_replica1 = PostgreSQL::Test::Cluster->new('replica1');
$node_replica1->polar_init_replica($node_primary);

# create standby1
my $node_standby1 = PostgreSQL::Test::Cluster->new('standby1');
$node_standby1->polar_init_standby($node_primary);


# primary config
$node_primary->append_conf(
	'postgresql.conf',
	"synchronous_standby_names='"
	  #   . $node_replica1->name . ","
	  . $node_standby1->name . "'");
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
command_fails_like(
	[ 'pg_checksums', $node_primary->data_dir ],
	qr/cluster must be shut down/,
	'pg_checksums failed!');
command_fails_like(
	[ 'pg_checksums', $node_replica1->data_dir ],
	qr/cluster must be shut down/,
	'pg_checksums failed!');
command_fails_like(
	[ 'pg_checksums', $node_standby1->data_dir ],
	qr/cluster must be shut down/,
	'pg_checksums failed!');

# stop nodes
$node_primary->stop;
# simple test
command_like(
	[ 'pg_checksums', $node_primary->data_dir ],
	qr/Bad checksums:\s+0/,
	'pg_checksums succ!');
command_like(
	[ 'pg_checksums', $node_replica1->data_dir ],
	qr/Bad checksums:\s+0/,
	'pg_checksums succ!');
command_fails_like(
	[ 'pg_checksums', $node_standby1->data_dir ],
	qr/cluster must be shut down/,
	'pg_checksums failed!');

# stop replica1
$node_replica1->stop;
# simple test
command_like(
	[ 'pg_checksums', $node_primary->data_dir ],
	qr/Bad checksums:\s+0/,
	'pg_checksums succ!');
command_like(
	[ 'pg_checksums', $node_replica1->data_dir ],
	qr/Bad checksums:\s+0/,
	'pg_checksums succ!');
command_fails_like(
	[ 'pg_checksums', $node_standby1->data_dir ],
	qr/cluster must be shut down/,
	'pg_checksums failed!');

# stop standby
$node_standby1->stop;
# simple test
command_like(
	[ 'pg_checksums', $node_primary->data_dir ],
	qr/Bad checksums:\s+0/,
	'pg_checksums succ!');
command_like(
	[ 'pg_checksums', $node_replica1->data_dir ],
	qr/Bad checksums:\s+0/,
	'pg_checksums succ!');
command_like(
	[ 'pg_checksums', $node_standby1->data_dir ],
	qr/Bad checksums:\s+0/,
	'pg_checksums succ!');

# result should keep same after we destroy the local copy one.
destroy_file($node_primary->data_dir . '/global/pg_control');
destroy_file($node_replica1->data_dir . '/global/pg_control');
destroy_file($node_standby1->data_dir . '/global/pg_control');
# simple test
command_like(
	[ 'pg_checksums', $node_primary->data_dir ],
	qr/Bad checksums:\s+0/,
	'pg_checksums succ!');
command_like(
	[ 'pg_checksums', $node_replica1->data_dir ],
	qr/Bad checksums:\s+0/,
	'pg_checksums succ!');
command_like(
	[ 'pg_checksums', $node_standby1->data_dir ],
	qr/Bad checksums:\s+0/,
	'pg_checksums succ!');

# primary's result of datadir should be same as replica1's result of polar_datadir.
my ($primary_stdout, $primary_stderr) =
  command_output([ 'pg_checksums', $node_primary->data_dir ]);
my ($replica1_stdout, $replica1_stderr) =
  command_output([ 'pg_checksums', $node_replica1->polar_get_datadir ]);
is($primary_stderr, '', $node_primary->name . ": no stderr");
is($replica1_stderr, '', $node_replica1->name . ": no stderr");
is( $primary_stdout =~ $replica1_stdout,
	1,
	$node_primary->name . "\'s result is the same as " . $node_replica1->name
);

my $primary_target = $node_primary->polar_get_datadir . '/base/1/1259';
my $primary_target_bak = $primary_target . '_bak';
my $sysret = `cp $primary_target $primary_target_bak`;
my $standby1_target = $node_standby1->polar_get_datadir . '/base/1/1259';
my $standby1_target_bak = $standby1_target . '_bak';
$sysret = `cp $standby1_target $standby1_target_bak`;
destroy_file($primary_target);
destroy_file($standby1_target);
# simple test
command_fails_like(
	[ 'pg_checksums', $node_primary->data_dir ],
	qr/checksum verification failed in file/,
	'pg_checksums failed!');
command_fails_like(
	[ 'pg_checksums', $node_replica1->data_dir ],
	qr/checksum verification failed in file/,
	'pg_checksums failed!');
command_fails_like(
	[ 'pg_checksums', $node_standby1->data_dir ],
	qr/checksum verification failed in file/,
	'pg_checksums failed!');
$sysret = `mv $primary_target_bak $primary_target`;
$sysret = `mv $standby1_target_bak $standby1_target`;
