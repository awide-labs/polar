# 011_polar_basebackup.pl
#	  PolarDB shared storage baseabckup ,restore, pitr test.
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
#	  src/bin/pg_basebackup/t/011_polar_basebackup.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More tests => 3;

sub wait_for_catchup_lsn
{
	my ($self, $target_lsn, $timeout) = @_;
	$timeout =
	  defined($timeout)
	  ? $timeout
	  : 180;    #default value, the same as poll_query_until

	my $lsn_expr;
	if (defined($target_lsn))
	{
		$lsn_expr = "'$target_lsn'";
	}
	else
	{
		$lsn_expr = 'pg_last_wal_replay_lsn()';
	}
	print "Waiting for node "
	  . $self->name . "'s "
	  . "replay_lsn to pass "
	  . $lsn_expr . "\n";
	my $query = qq[SELECT $lsn_expr <= pg_last_wal_replay_lsn();];
	if (!$self->poll_query_until('postgres', $query, 't', $timeout))
	{
		print "timed out waiting for catchup\n";
		return 0;
	}
	return 1;
}

$ENV{'PG_TEST_NOCLEAN'} = 1;
# Initialize primary node
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;

# make polar_datadir inside data_directory
my $primary_datadir = $node_primary->polar_get_datadir;
my $primary_basedir = $node_primary->data_dir;
my $new_primary_datadir = "$primary_basedir/polar_shared_data";
PostgreSQL::Test::Utils::system_or_bail('mv', $primary_datadir,
	$new_primary_datadir);
$node_primary->polar_set_datadir($new_primary_datadir);
$node_primary->append_conf('postgresql.conf',
	'polar_datadir=\'file-dio://./polar_shared_data/\'');
PostgreSQL::Test::Utils::system_or_bail('rm', '-f',
	"$primary_basedir/polar_node_static.conf");
$node_primary->start;

## Take backup
my $backup_name = "polar_backup";
my $polardata =
  $node_primary->backup_dir . '/' . $backup_name . '/' . '/polar_shared_data';
$node_primary->polar_backup($backup_name, $polardata);
# create standby
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);
$node_standby->polar_standby_set_recovery($node_primary);
$node_primary->polar_create_slot($node_standby->name);
$node_standby->start;
# Create some content on primary and check its presence in standby
$node_primary->safe_psql('postgres',
	"CREATE TABLE tab_int AS SELECT generate_series(1,1000) AS a");
# Wait for standbys to catch up
$node_primary->wait_for_catchup($node_standby, 'replay',
	$node_primary->lsn('insert'));
my $result =
  $node_standby->safe_psql('postgres', "SELECT count(*) FROM tab_int");
print "standby: $result\n";
is($result, qq(1000), 'check streamed content on standby');

#test promote
$node_standby->promote;
$node_standby->stop;
$node_primary->polar_drop_slot($node_standby->name);

## self-defined pg_basebackup tool usage test
$backup_name = "self_defined_backup";
my $backup_path = $node_primary->backup_dir . '/' . $backup_name;
print "# Taking polar_basebackup $backup_name from node "
  . $node_primary->name . "\n";
PostgreSQL::Test::Utils::system_or_bail(
	'polar_basebackup', '--pgdata=' . $backup_path,
	'--host=' . $node_primary->host, '--port=' . $node_primary->port,
	'--no-sync', '--write-recovery-conf',
	'--format=tar', '--gzip',
	'-X', 'fetch');
PostgreSQL::Test::Utils::system_or_bail('tar', '-zxvf',
	$backup_path . '/base.tar.gz',
	'-C', $backup_path);
PostgreSQL::Test::Utils::system_or_bail('rm', '-f',
	$backup_path . '/base.tar.gz');
# pg_basebackup tool write primary_conninfo into /postgresql.auto.conf
PostgreSQL::Test::Utils::system_or_bail('rm', '-f',
	$backup_path . '/postgresql.auto.conf');
PostgreSQL::Test::Utils::system_or_bail('tar', '-zxvf',
	$backup_path . '/data.tar.gz',
	'-C', $backup_path . '/polar_shared_data');
PostgreSQL::Test::Utils::system_or_bail('rm', '-f',
	$backup_path . '/data.tar.gz');
# create standby
$node_standby = PostgreSQL::Test::Cluster->new('standby1');
$node_standby->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);
$node_standby->polar_standby_set_recovery($node_primary);
$node_primary->polar_create_slot($node_standby->name);
$node_standby->start;
# Create some content on primary and check its presence in standby1
$node_primary->safe_psql('postgres',
	"CREATE TABLE tab_int_1 AS SELECT generate_series(1,1000) AS a");
# Wait for standbys to catch up
$node_primary->wait_for_catchup($node_standby, 'replay',
	$node_primary->lsn('insert'));
$result =
  $node_standby->safe_psql('postgres', "SELECT count(*) FROM tab_int_1");
print "standby1: $result\n";
is($result, qq(1000), 'check streamed content on standby1');

#test promote
$node_standby->promote;
$node_standby->stop;
$node_primary->polar_drop_slot($node_standby->name);

# TODO modify basebackup for polar_shared_data in data
# ## self-defined pg_basebackup tool usage test via stdout
# $backup_name = "self_defined_backup_stdout";
# $backup_path = $node_primary->backup_dir . '/' . $backup_name;
# mkdir $backup_path;
# my $backup_file = $backup_path . '/' . $backup_name . '.tar.gz';
# my $backup_file_fd;
# open($backup_file_fd, '>', $backup_file)
#   or BAIL_OUT("open failed: $!, file is $backup_file");
# print "# Taking polar_basebackup $backup_name from node "
#   . $node_primary->name . "\n";
# my @backup_cmd = (
# 	'polar_basebackup', '--pgdata=-',
# 	'--host=' . $node_primary->host, '--port=' . $node_primary->port,
# 	'--no-sync', '--write-recovery-conf',
# 	'--format=tar', '--gzip',
# 	'-X', 'fetch');
# my $stderr = '';
# my @run_cmd_opts = (\@backup_cmd, '>', $backup_file_fd, '2>', \$stderr);
# $result = IPC::Run::run @run_cmd_opts;
# ok($result,
# 		"pg_basebackup failed: exit code 0, cmd is "
# 	  . join(' ', @backup_cmd)
# 	  . ", stderr is $stderr");
# close($backup_file_fd)
#   or BAIL_OUT("close failed: $!, file is $backup_file");
# PostgreSQL::Test::Utils::system_or_bail('tar', '-zxvf', $backup_file, '-C', $backup_path);
# PostgreSQL::Test::Utils::system_or_bail('rm', '-f', $backup_file);
# # pg_basebackup tool write primary_conninfo into /postgresql.auto.conf
# PostgreSQL::Test::Utils::system_or_bail('rm', '-f', $backup_path . '/postgresql.auto.conf');
# # create standby
# $node_standby = PostgreSQL::Test::Cluster->new('standby2');
# $node_standby->init_from_backup($node_primary, $backup_name,
# 	has_streaming => 1);
# $node_standby->polar_standby_set_recovery($node_primary);
# $node_primary->polar_create_slot($node_standby->name);
# $node_standby->start;
# # Create some content on primary and check its presence in standby1
# $node_primary->safe_psql('postgres',
# 	"INSERT INTO tab_int_1 SELECT generate_series(1,1000) AS a");
# # Wait for standbys to catch up
# $node_primary->wait_for_catchup($node_standby, 'replay',
# 	$node_primary->lsn('insert'));
# $result =
#   $node_standby->safe_psql('postgres', "SELECT count(*) FROM tab_int_1");
# print "standby2: $result\n";
# is($result, qq(2000), 'check streamed content on standby2');

# #test promote
# $node_standby->promote;
# $node_standby->stop;
# $node_primary->polar_drop_slot($node_standby->name);

$node_primary->stop;

## self-defined PITR usage test
my $pitr_name = "self_defined_pitr";
my $archive_path = $node_primary->archive_dir . '/' . $pitr_name;
mkdir $node_primary->archive_dir;
mkdir $archive_path;
$backup_path = $node_primary->backup_dir . '/' . $pitr_name;
# prepare for pitr
$node_primary->append_conf('postgresql.conf', 'archive_mode=on');
$node_primary->append_conf('postgresql.conf', 'archive_timeout=300');
$node_primary->append_conf('postgresql.conf',
	'archive_command = \'cp -i %p ' . $archive_path . '/%f\'');
$node_primary->start;
print "# Taking polar_basebackup $pitr_name from node "
  . $node_primary->name . "\n";
PostgreSQL::Test::Utils::system_or_bail(
	'polar_basebackup', '--pgdata=' . $backup_path,
	'--host=' . $node_primary->host, '--port=' . $node_primary->port,
	'--no-sync', '--write-recovery-conf',
	'--format=tar', '--gzip',
	'-X', 'fetch');
PostgreSQL::Test::Utils::system_or_bail('tar', '-zxvf',
	$backup_path . '/base.tar.gz',
	'-C', $backup_path);
PostgreSQL::Test::Utils::system_or_bail('rm', '-f',
	$backup_path . '/base.tar.gz');
# pg_basebackup tool write primary_conninfo into /postgresql.auto.conf
PostgreSQL::Test::Utils::system_or_bail('rm', '-f',
	$backup_path . '/postgresql.auto.conf');
PostgreSQL::Test::Utils::system_or_bail('tar', '-zxvf',
	$backup_path . '/data.tar.gz',
	'-C', $backup_path . '/polar_shared_data');
PostgreSQL::Test::Utils::system_or_bail('rm', '-f',
	$backup_path . '/data.tar.gz');
# Create some content and more WALs on primary
$node_primary->safe_psql('postgres', "select pg_switch_wal();");
$node_primary->safe_psql('postgres',
	"CREATE TABLE tab_int_2 AS SELECT generate_series(1,1000) AS a");
$node_primary->safe_psql('postgres', "select pg_switch_wal();");
$node_primary->safe_psql('postgres',
	"CREATE TABLE tab_int_3 AS SELECT generate_series(1,1000) AS a");
$node_primary->safe_psql('postgres', "select pg_switch_wal();");
$node_primary->safe_psql('postgres', "checkpoint;");
my $insert_lsn = $node_primary->lsn('insert');
$node_primary->stop;
# create standby
$node_standby = PostgreSQL::Test::Cluster->new('standby3');
$node_standby->init_from_backup($node_primary, $pitr_name,
	enable_restoring => 1);
$node_standby->polar_standby_set_recovery($node_primary);
$node_standby->append_conf('postgresql.conf',
	'restore_command = \'cp ' . $archive_path . '/%f %p\'');
$node_standby->start;

# test result
wait_for_catchup_lsn($node_standby, $insert_lsn, 600);
$result =
  $node_standby->safe_psql('postgres', "SELECT count(*) FROM tab_int_3;");
print "standby3: $result\n";
is($result, qq(1000), 'check pitr content on standby3');
#test promote
$node_standby->promote;
$node_standby->stop;

# TODO modify pg_basebackup
# ## merge double stream pg_basebackup tool usage test
# $node_primary->start;
# $backup_name = "double_stream_backup";
# $backup_path = $node_primary->backup_dir . '/' . $backup_name;
# mkdir $backup_path;
# print
#   "# Taking polar_basebackup $backup_name from node \"$node_primary->name\"\n";
# PostgreSQL::Test::Utils::system_or_bail('polar_basebackup --pgdata=- --host='
# 	  . $node_primary->host
# 	  . ' --port='
# 	  . $node_primary->port
# 	  . ' --no-sync --write-recovery-conf --format=tar -X stream >'
# 	  . $backup_path
# 	  . '/data.tar');
# PostgreSQL::Test::Utils::system_or_bail('tar', '-xvf', $backup_path . '/data.tar',
# 	'-C', $backup_path);
# PostgreSQL::Test::Utils::system_or_bail('rm', '-f', $backup_path . '/data.tar');
# command_ok(
# 	[
# 		'pg_verifybackup', '-i',
# 		'polar_shared_data/pg_wal', '--no-parse-wal',
# 		$backup_path
# 	],
# 	'backup successfully verified');
# PostgreSQL::Test::Utils::system_or_bail('rm', '-f', $backup_path . '/postgresql.auto.conf');
# # create standby
# $node_standby = PostgreSQL::Test::Cluster->new('standby4');
# $node_standby->init_from_backup($node_primary, $backup_name,
# 	has_streaming => 1);
# $node_standby->polar_standby_set_recovery($node_primary);
# $node_primary->polar_create_slot($node_standby->name);
# $node_standby->start;
# # Create some content on master and check its presence in standby1
# $node_primary->safe_psql('postgres',
# 	"CREATE TABLE tab_int_4 AS SELECT generate_series(1,1000) AS a");
# # Wait for standbys to catch up
# $node_primary->wait_for_catchup($node_standby, 'replay',
# 	$node_primary->lsn('insert'));
# $result =
#   $node_standby->safe_psql('postgres', "SELECT count(*) FROM tab_int_4");
# print "standby1: $result\n";
# is($result, qq(1000), 'check streamed content on standby1');

# $node_standby->stop;
# $node_primary->stop;
