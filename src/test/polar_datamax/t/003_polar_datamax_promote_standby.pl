# Test for datamax promote standby
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils ();
use Test::More tests=>29;

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

# set polar_enable_promote_wait_for_walreceive_done = on
$node_master->append_conf('postgresql.conf', "polar_enable_promote_wait_for_walreceive_done = on");
$node_master->append_conf('postgresql.conf', "synchronous_commit = on");
$node_master->append_conf('postgresql.conf', "synchronous_standby_names='".$node_datamax->name."'");
$node_datamax->append_conf('postgresql.conf', "polar_enable_promote_wait_for_walreceive_done = on");
$node_standby->append_conf('postgresql.conf', "polar_enable_promote_wait_for_walreceive_done = on");

# start cluster
$node_master->start;
$node_datamax->start;
$node_standby->start;

my $wait_timeout = 40;
$node_master->safe_psql('postgres', 'CREATE TABLE test_table(val integer);');
$node_datamax->wait_walstreaming_establish_timeout($wait_timeout);
my $walpid = $node_standby->wait_walstreaming_establish_timeout($wait_timeout);
ok($walpid != 0, "walstream between datamax and standby is normal");

#### 1. promote when master and datamax runs normarlly
my $promote_ret = $node_standby->promote_constraint(0);
ok($promote_ret != 0, "promote is not allowed when master runs normally");
my ($cmdret, $stdout, $stderr);
$cmdret = $node_standby->psql('postgres',
		'INSERT INTO test_table(val) SELECT coalesce(max(val),0) + 1 AS newval FROM test_table RETURNING val',
		stdout => \$stdout, stderr => \$stderr);
print "cmdret: $cmdret, stdout: $stdout, stderr: $stderr\n";
my $error = "ERROR:  cannot execute INSERT in a read-only transaction";
ok($stderr =~ $error, "promote fail when master runs normally, standby read only");

#### 2. promote when master runs normally and datamax stops
$node_datamax->stop;
$promote_ret = $node_standby->promote_constraint(0);
ok($promote_ret != 0, "promote is not allowed when datamax stops, no walstreaming between datamax and standby");
$cmdret = $node_standby->psql('postgres',
		'INSERT INTO test_table(val) SELECT coalesce(max(val),0) + 1 AS newval FROM test_table RETURNING val',
		stdout => \$stdout, stderr => \$stderr);
print "cmdret: $cmdret, stdout: $stdout, stderr: $stderr\n";
ok($cmdret !=0 && $stderr =~ $error, "promote fail when master runs normally and datamax stops, standby read only");

#### 3. promote when master and datamax stops
$node_master->stop;
$promote_ret = $node_standby->promote_constraint(0);
ok($promote_ret != 0, "promote is not allowed when master and datamax stops, no walstreaming between datamax and standby");
$cmdret = $node_standby->psql('postgres',
		'INSERT INTO test_table(val) SELECT coalesce(max(val),0) + 1 AS newval FROM test_table RETURNING val',
		stdout => \$stdout, stderr => \$stderr);
print "cmdret: $cmdret, stdout: $stdout, stderr: $stderr\n";
ok($cmdret !=0 && $stderr =~ $error, "promote fail when master and datamax stops, standby read only");

#### 4. promote when master stops running and datamax runs normally, check whether standby has received all data
$node_master->start;
$node_datamax->start;
$node_datamax->wait_walstreaming_establish_timeout($wait_timeout);
$node_standby->wait_walstreaming_establish_timeout($wait_timeout);

# stop walreciver process of standby
$node_standby->stop_child('walreceiver');
# insert new data in master
$node_master->safe_psql('postgres', "select pg_switch_wal();");	
my $newval = 0;
my $i = 0;
for($i = 0; $i < 10; $i = $i + 1)
{
	$newval = $node_master->safe_psql('postgres',
		'INSERT INTO test_table(val) SELECT coalesce(max(val),0) + 1 AS newval FROM test_table RETURNING val');
} 
my $result = $node_master->safe_psql('postgres',
		qq[SELECT 1 FROM test_table WHERE val = $newval]);
ok($result == 1, "master insert data success");

# select data in standby node, which hasn't been received by standby
$result = $node_standby->safe_psql('postgres',
		qq[SELECT 1 FROM test_table WHERE val = $newval]);
is($result, '', "select result is null because wal haven't been sent to standby");

# stop master and resume walreceiver process of standby
$node_master->stop;
$node_standby->resume_child('walreceiver');
# wait walstream establish in case walreceiver process exit due to timeout during stop period
$walpid = 0;
$walpid = $node_standby->wait_walstreaming_establish_timeout($wait_timeout);
ok($walpid != 0, "walstream between datamax and standby is normal");

# promote standby and check whether standby has received all data
$promote_ret = $node_standby->promote_constraint(0);
ok($promote_ret == 0, "promote success when master stops running and datamax runs normally");
$result = $node_standby->safe_psql('postgres',
		qq[SELECT 1 FROM test_table WHERE val = $newval]);
ok($result == 1, "standby has received all data before promote");
$node_standby->stop;
#$node_standby->clean_node;
#$node_datamax->polar_drop_slot($node_standby->name);

#### 5. promote when master stop and datamax restart, test bug#30762737
# init new standby1 
my $node_standby1 = PostgreSQL::Test::Cluster->new('standby1');
$node_standby1->polar_init_standby_no_recovery();
$node_standby1->polar_set_root_node($node_master);
$node_standby1->polar_standby_build_data;
$node_standby1->polar_standby_set_recovery($node_datamax);
$node_standby1->append_conf('postgresql.conf', "polar_enable_promote_wait_for_walreceive_done = on");

$node_datamax->polar_create_slot($node_standby1->name);
#restart datamax
$node_datamax->restart;
$node_standby1->start;
$walpid = 0;
$walpid = $node_standby1->wait_walstreaming_establish_timeout($wait_timeout);
ok($walpid != 0, "walstream between datamax and standby1 is normal");
$promote_ret = $node_standby1->promote_constraint(0);
ok($promote_ret == 0, "promote success when master stops running and datamax restart");
$node_standby1->stop;
$node_standby1->clean_node;
$node_datamax->polar_drop_slot($node_standby1->name);

#### 6. force promote when master and datamax runs normally, check whether standby received all data
my $node_standby2 = PostgreSQL::Test::Cluster->new('standby2');
$node_standby2->polar_init_standby_no_recovery();
$node_standby2->polar_set_root_node($node_master);
$node_standby2->polar_standby_build_data;
$node_standby2->polar_standby_set_recovery($node_datamax);
$node_standby2->append_conf('postgresql.conf', "polar_enable_promote_wait_for_walreceive_done = on");

$node_datamax->polar_create_slot($node_standby2->name);
$node_standby2->start;
$node_standby2->wait_walstreaming_establish_timeout($wait_timeout);

$node_master->start;
$node_datamax->wait_walstreaming_establish_timeout($wait_timeout);
# stop walreciver process of standby
$node_standby2->stop_child('walreceiver');
# insert new data in master
$node_master->safe_psql('postgres', "select pg_switch_wal();");	
for($i = 0; $i < 10; $i = $i + 1)
{
	$newval = $node_master->safe_psql('postgres',
		'INSERT INTO test_table(val) SELECT coalesce(max(val),0) + 1 AS newval FROM test_table RETURNING val');
}
$result = $node_master->safe_psql('postgres',
		qq[SELECT 1 FROM test_table WHERE val = $newval]);
ok($result == 1, "master insert data success");

# select data in standby node, which hasn't been received by standby
$result = $node_standby2->safe_psql('postgres',
		qq[SELECT 1 FROM test_table WHERE val = $newval]);
is($result, '', "select result is null because wal haven't been sent to standby");

# resume walreceiver process of standby
$node_standby2->resume_child('walreceiver');
$walpid = 0;
$walpid = $node_standby2->wait_walstreaming_establish_timeout($wait_timeout);
ok($walpid != 0, "walstream between datamax and standby2 is normal");

# promote standby and check whether standby has received all data
$promote_ret = $node_standby2->promote_constraint(1);
ok($promote_ret == 0, "force promote success when master and datamax runs normally");
$result = $node_standby2->safe_psql('postgres',
		qq[SELECT 1 FROM test_table WHERE val = $newval]);
print "result : $result\n";
# standby may have received all data after resume walstreaming
#is($result, '', "standby hasn't received all data when in force promote way");
$node_standby2->stop;
$node_standby2->clean_node;
$node_datamax->polar_drop_slot($node_standby2->name);

#### 7. force promote when master runs normally and datamax stops
my $node_standby3 = PostgreSQL::Test::Cluster->new('standby3');
$node_standby3->polar_init_standby_no_recovery();
$node_standby3->polar_set_root_node($node_master);
$node_standby3->polar_standby_build_data;
$node_standby3->polar_standby_set_recovery($node_datamax);
$node_standby3->append_conf('postgresql.conf', "polar_enable_promote_wait_for_walreceive_done = on");

$node_datamax->polar_create_slot($node_standby3->name);
$node_standby3->start;
$node_standby3->wait_walstreaming_establish_timeout($wait_timeout);
$node_datamax->stop;
$promote_ret = $node_standby3->promote_constraint(1);
ok($promote_ret == 0, "force promote success when master runs normally and datamax stops");
$node_standby3->stop;
$node_standby3->clean_node;
$node_datamax->start;
$node_datamax->polar_drop_slot($node_standby3->name);

#### 8. force promote when master and datamax stops
my $node_standby4 = PostgreSQL::Test::Cluster->new('standby4');
$node_standby4->polar_init_standby_no_recovery();
$node_standby4->polar_set_root_node($node_master);
$node_standby4->polar_standby_build_data;
$node_standby4->polar_standby_set_recovery($node_datamax);
$node_standby4->append_conf('postgresql.conf', "polar_enable_promote_wait_for_walreceive_done = on");

$node_datamax->polar_create_slot($node_standby4->name);
$node_standby4->start;
$node_standby4->wait_walstreaming_establish_timeout($wait_timeout);
$node_datamax->stop;
$node_master->stop;
$promote_ret = $node_standby4->promote_constraint(1);
ok($promote_ret == 0, "force promote success when master and datamax stops");
$node_standby4->stop;
$node_standby4->clean_node;
$node_datamax->start;
$node_datamax->polar_drop_slot($node_standby4->name);

#### 9. promote when master and datamax runs normally, datamax streaming in max_performance mode
$node_master->start;
$node_master->safe_psql('postgres', 'ALTER SYSTEM SET synchronous_commit = off;');
$node_master->safe_psql('postgres', "ALTER SYSTEM SET synchronous_standby_names = '';");
$node_master->reload;
my $node_standby5 = PostgreSQL::Test::Cluster->new('standby5');
$node_standby5->polar_init_standby_no_recovery();
$node_standby5->polar_set_root_node($node_master);
$node_standby5->polar_standby_build_data;
$node_standby5->polar_standby_set_recovery($node_datamax);
$node_standby5->append_conf('postgresql.conf', "polar_enable_promote_wait_for_walreceive_done = on");

$node_datamax->polar_create_slot($node_standby5->name);
$node_standby5->start;
$walpid = 0;
$walpid = $node_standby5->wait_walstreaming_establish_timeout($wait_timeout);
ok($walpid != 0, "walstream between datamax and standby5 is normal");

$promote_ret = $node_standby5->promote_constraint(0);
ok($promote_ret != 0, "promote is not allowed when master and datamax runs normally, datamax streaming in max_performance mode");
$cmdret = $node_standby5->psql('postgres',
		'INSERT INTO test_table(val) SELECT coalesce(max(val),0) + 1 AS newval FROM test_table RETURNING val',
		stdout => \$stdout, stderr => \$stderr);
ok($stderr =~ $error, "promote fail when master and datamax runs normally, standby read only");
$node_standby5->stop;
$node_standby5->clean_node;
$node_datamax->polar_drop_slot($node_standby5->name);

#### 10. promote when master stops and datamax runs normally, datamax streaming in max_performance mode
my $node_standby6 = PostgreSQL::Test::Cluster->new('standby6');
$node_standby6->polar_init_standby_no_recovery();
$node_standby6->polar_set_root_node($node_master);
$node_standby6->polar_standby_build_data;
$node_standby6->polar_standby_set_recovery($node_datamax);
$node_standby6->append_conf('postgresql.conf', "polar_enable_promote_wait_for_walreceive_done = on");

$node_datamax->polar_create_slot($node_standby6->name);
$node_standby6->start;
$node_standby6->wait_walstreaming_establish_timeout($wait_timeout);
# stop walreciver process of standby
$node_standby6->stop_child('walreceiver');
# insert new data in master
$node_master->safe_psql('postgres', "select pg_switch_wal();");	
for($i = 0; $i < 10; $i = $i + 1)
{
	$newval = $node_master->safe_psql('postgres',
		'INSERT INTO test_table(val) SELECT coalesce(max(val),0) + 1 AS newval FROM test_table RETURNING val');
} 
$result = $node_master->safe_psql('postgres',
		qq[SELECT 1 FROM test_table WHERE val = $newval]);
ok($result == 1, "master insert data success");

# select data in standby node, which hasn't been received by standby
$result = $node_standby6->safe_psql('postgres',
		qq[SELECT 1 FROM test_table WHERE val = $newval]);
is($result, '', "select result is null because wal haven't been sent to standby");

# wait for datamax has received all wal when in max_performance mode
my $insert_lsn = $node_master->lsn('insert');
print "insert_lsn: $insert_lsn\n";
# wait_for_catchup blocks until the datamax flushes past $insert_lsn and
# croaks on timeout, so reaching the next line means the catchup succeeded.
$node_master->wait_for_catchup($node_datamax, 'flush', $insert_lsn);
ok(1, "node_datamax catchup success");

# stop master and resume walreceiver process of standby
$node_master->stop;
$node_standby6->resume_child('walreceiver');
$walpid = 0;
$walpid = $node_standby6->wait_walstreaming_establish_timeout($wait_timeout);
ok($walpid != 0, "walstream between datamax and standby6 is normal");

# promote standby and check whether standby has received all data
$promote_ret = $node_standby6->promote_constraint(0);
ok($promote_ret == 0, "promote success when master stops running and datamax runs normally, datamax streaming in max_performance mode");
$result = $node_standby6->safe_psql('postgres',
		qq[SELECT 1 FROM test_table WHERE val = $newval]);
ok($result == 1, "standby has received all data before promote");
$node_standby6->stop;
$node_standby6->clean_node;
$node_datamax->polar_drop_slot($node_standby6->name);

$node_datamax->stop;


