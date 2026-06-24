# Test for datamax streaming in async (max_performance) and synchronous
# (max_protection) modes.
#
# The original PolarDB suite exercised three Data-Guard-style protection modes:
# max_performance (async), max_protection (synchronous, block forever) and
# max_availability (synchronous with auto-degrade on timeout). In this fork's
# port the PolarDB "transaction sync mode" subsystem
# (polar_enable_transaction_sync_mode / polar_sync_replication_timeout and the
# semi-synchronous jitter-tolerance layer) was dropped upstream, so:
#   - max_performance maps to plain asynchronous streaming;
#   - max_protection maps to standard PostgreSQL synchronous replication
#     (synchronous_standby_names + synchronous_commit), which now applies
#     directly without an enabling GUC;
#   - max_availability has no equivalent and is intentionally not ported.
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use Test::More tests=>4;

# master
my $node_master = PostgreSQL::Test::Cluster->new('master');
$node_master->polar_init_primary();
my $primary_system_identifier = $node_master->polar_get_system_identifier;

# datamax
my $node_datamax = PostgreSQL::Test::Cluster->new('datamax');
$node_datamax->polar_init_datamax($primary_system_identifier);

# standby
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->polar_init_standby_no_recovery();
$node_standby->polar_set_root_node($node_master);
$node_standby->polar_standby_build_data;
$node_standby->polar_standby_set_recovery($node_datamax);

$node_master->start;
$node_master->polar_create_slot($node_datamax->name);
$node_master->stop;

$node_datamax->start;
$node_datamax->polar_create_slot($node_standby->name);
$node_datamax->safe_psql('postgres','CREATE EXTENSION polar_monitor;');
$node_datamax->stop;

# set datamax recovery.conf after create extension
$node_datamax->polar_datamax_set_recovery($node_master);

#### 1. datamax streaming in max_performance mode
# start cluster
$node_master->start;
$node_datamax->start;
$node_standby->start;

my $wait_timeout = 40;
$node_master->safe_psql('postgres', 'CREATE TABLE test_table(val integer);');
$node_datamax->wait_walstreaming_establish_timeout($wait_timeout);
# stop walreceiver process of datamax when streaming in max_performance mode
$node_datamax->stop_child('walreceiver');
my $newval = $node_master->safe_psql('postgres',
		'INSERT INTO test_table(val) SELECT coalesce(max(val),0) + 1 AS newval FROM test_table RETURNING val');
my $result = $node_master->safe_psql('postgres',
		qq[SELECT 1 FROM test_table WHERE val = $newval]);
print "stop walreceiver, insert_result: $result\n";
ok($result == 1, 'insert success when stop datamax and streaming in max_performance mode');

# resume walreceiver process of datamax when streaming in max_performance mode
$node_datamax->resume_child('walreceiver');
$newval = $node_master->safe_psql('postgres',
		'INSERT INTO test_table(val) SELECT coalesce(max(val),0) + 1 AS newval FROM test_table RETURNING val');
$result = $node_master->safe_psql('postgres',
		qq[SELECT 1 FROM test_table WHERE val = $newval]);
print "resume walrecever, insert result: $result\n";
ok($result == 1, 'insert success when resume datamax');

#### 2. datamax streaming in synchronous (max_protection) mode
# Standard PostgreSQL synchronous replication: with synchronous_standby_names
# pointing at the datamax and synchronous_commit on, a commit must wait for the
# datamax to flush the WAL (RPO=0). This needs no
# polar_enable_transaction_sync_mode; standard syncrep applies directly.
$node_master->safe_psql('postgres', 'ALTER SYSTEM SET synchronous_commit = on;');
$node_master->safe_psql('postgres',
		"ALTER SYSTEM SET synchronous_standby_names = '".$node_datamax->name."';");
$node_master->reload;

# stop walreceiver process of datamax when streaming in max_protection mode
$node_datamax->wait_walstreaming_establish_timeout($wait_timeout);
$node_datamax->stop_child('walreceiver');
my $timed_out = 0;
$node_master->safe_psql('postgres',
		'INSERT INTO test_table(val) SELECT coalesce(max(val),0) + 1 AS newval FROM test_table RETURNING val',
		timeout => 20, timed_out => \$timed_out);
print "stop walreceiver, timed_out: $timed_out\n";
ok($timed_out == 1, 'insert hangs when stop datamax and streaming in max_protection mode');

# resume walreceiver process of datamax when streaming in max_protection mode
$node_datamax->resume_child('walreceiver');
$newval = $node_master->safe_psql('postgres',
		'INSERT INTO test_table(val) SELECT coalesce(max(val),0) + 1 AS newval FROM test_table RETURNING val');
$result = $node_master->safe_psql('postgres',
		qq[SELECT 1 FROM test_table WHERE val = $newval]);
print "resume walrecever, insert result: $result\n";
ok($result == 1, 'insert success when resume datamax in max_protection mode');

$node_standby->stop;
$node_datamax->stop;
$node_master->stop;


