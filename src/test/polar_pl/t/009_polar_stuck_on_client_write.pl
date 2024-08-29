# 009_polar_stuck_on_client_write.pl
#	  Test for backend process gets stuck on client_write
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
#	  src/test/polar_pl/t/009_polar_stuck_on_client_write.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# primary node
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;

$node_primary->start;
$node_primary->safe_psql('postgres', 'CREATE TABLE test_table(val integer);');
# insert more data so that the output buffer can be filled up when select from the table
$node_primary->safe_psql('postgres',
	"INSERT INTO test_table(val) SELECT generate_series(1,5000000) as newwal"
);

my $timeout = 3;
my $psql = {};
$node_primary->psql_connect("postgres", $timeout, _psql => $psql);

# find backend process and stop it
my $backend = $node_primary->find_child("idle");
print "ready to stop backend pid: $backend\n";
my @res = `kill -stop $backend`;
# execute select in psql
$node_primary->psql_execute("select * from test_table;", _psql => $psql);
my $host = $node_primary->host;
my $port = $node_primary->port;
my $client = readpipe(
	"ps -ef | grep \"psql\" | grep \"$host\" | grep \"$port\" | grep -v grep | awk '{print \$2}'"
);
print "ready to stop client pid: $client\n";

# stop client and resume backend process, it will get stuck on secure_write()
@res = `kill -stop $client`;
@res = `kill -cont $backend`;

# cancel the query
sleep 3;
$node_primary->safe_psql('postgres', "select pg_cancel_backend($backend)");
# check the query
my $pid_new = $node_primary->find_child("SELECT");
print "backend pid:$pid_new\n";
ok($pid_new == $backend, "backend can't be canceled");
@res = `kill -cont $client`;
$node_primary->psql_close(_psql => $psql);

# standby node
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->polar_init_standby($node_primary);
$node_standby->append_conf('postgresql.conf',
	"max_standby_streaming_delay = 3000");
$node_standby->append_conf('postgresql.conf',
	"max_standby_archive_delay = 3000");
$node_standby->append_conf('postgresql.conf', "hot_standby_feedback = off");
#disable logging collector, need grep logfile later
$node_standby->append_conf('postgresql.conf', "Logging_collector = false");

$node_primary->polar_create_slot($node_standby->name);
$node_standby->start;
my $wait_timeout = 40;
my $walpid =
  $node_standby->wait_walstreaming_establish_timeout($wait_timeout);
ok($walpid != 0, "walstream between primary and standby is normal");

# create recovery conflict
my $standby_psql = {};
$node_standby->psql_connect("postgres", $timeout, _psql => $standby_psql);
$backend = $node_standby->find_child("idle");
print "ready to stop backend pid: $backend\n";
@res = `kill -stop $backend`;
$node_standby->psql_execute("select * from test_table;",
	_psql => $standby_psql);
$host = $node_standby->host;
$port = $node_standby->port;
$client = readpipe(
	"ps -ef | grep \"psql\" | grep \"$host\" | grep \"$port\" | grep -v grep | awk '{print \$2}'"
);
print "ready to stop client pid: $client\n";
@res = `kill -stop $client`;
@res = `kill -cont $backend`;

# delete data and do vacuum in primary node
$node_primary->safe_psql("postgres", "delete from test_table");
my $lsn = $node_primary->lsn('write');
$node_primary->wait_for_catchup($node_standby->name, 'replay', $lsn, 't',
	't', 300);
$node_primary->safe_psql("postgres", "vacuum test_table");
$node_primary->wait_for_catchup($node_standby->name, 'flush');

# wait until recovery conflict generate
sleep 6;
$pid_new = $node_standby->find_child("SELECT");
print "backend pid:$pid_new\n";
ok($pid_new == 0, "backend is canceled");
@res = `kill -cont $client`;
$node_standby->psql_close(_psql => $standby_psql);

my $logdir = $node_standby->logfile;
print "standby logfile is: $logdir\n";
my @exits =
  `grep -rn "terminating connection due to conflict with recovery" $logdir`;
my $found = @exits;
print "found:$found\n";
ok($found == 1, "backend is canceled due to recovery conflict");

$node_standby->stop;
$node_primary->stop;
done_testing();
