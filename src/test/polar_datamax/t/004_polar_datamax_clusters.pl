# Test for build datamax clusters
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use Test::More tests=>33;

#### primary az in main region
# master
my $node_master = PostgreSQL::Test::Cluster->new('master');
$node_master->polar_init_primary();
my $primary_system_identifier = $node_master->polar_get_system_identifier;

# active datamax for master
my $node_active_datamax = PostgreSQL::Test::Cluster->new('active_datamax');
$node_active_datamax->polar_init_datamax($primary_system_identifier);

# backup datamax for master
my $node_backup_datamax = PostgreSQL::Test::Cluster->new('backup_datamax');
$node_backup_datamax->polar_init_datamax($primary_system_identifier);

#### standby az in main region
# standby
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->polar_init_standby_no_recovery();
$node_standby->polar_set_root_node($node_master);
$node_standby->polar_standby_build_data;

# datamax for standby
my $node_standby_datamax = PostgreSQL::Test::Cluster->new('standby_datamax');
$node_standby_datamax->polar_init_datamax($primary_system_identifier);

#### standby1 az in backup region
# standby1
my $node_standby1 = PostgreSQL::Test::Cluster->new('standby1');
$node_standby1->polar_init_standby_no_recovery();
$node_standby1->polar_set_root_node($node_master);
$node_standby1->polar_standby_build_data;

# datamax for standby1
my $node_standby1_datamax = PostgreSQL::Test::Cluster->new('standby1_datamax');
$node_standby1_datamax->polar_init_datamax($primary_system_identifier);

# create extension
$node_active_datamax->start;
$node_active_datamax->polar_create_slot($node_backup_datamax->name);
$node_active_datamax->polar_create_slot($node_standby->name);
$node_active_datamax->polar_create_slot($node_standby1->name);
$node_active_datamax->safe_psql('postgres','CREATE EXTENSION polar_monitor;');
$node_active_datamax->stop;

$node_backup_datamax->start;
$node_backup_datamax->safe_psql('postgres','CREATE EXTENSION polar_monitor;');
$node_backup_datamax->stop;

$node_standby_datamax->start;
$node_standby_datamax->safe_psql('postgres','CREATE EXTENSION polar_monitor;');
$node_standby_datamax->stop;

$node_standby1_datamax->start;
$node_standby1_datamax->safe_psql('postgres','CREATE EXTENSION polar_monitor;');
$node_standby1_datamax->stop;

# set recovery.conf
$node_active_datamax->polar_datamax_set_recovery($node_master);
$node_standby->polar_standby_set_recovery($node_active_datamax);
$node_standby1->polar_standby_set_recovery($node_active_datamax);
$node_backup_datamax->polar_datamax_set_recovery($node_active_datamax);
$node_standby_datamax->polar_datamax_set_recovery($node_standby);
$node_standby1_datamax->polar_datamax_set_recovery($node_standby1);

# start clusters
$node_master->append_conf('postgresql.conf', "synchronous_commit = on");
$node_master->append_conf('postgresql.conf', "synchronous_standby_names='".$node_active_datamax->name."'");
$node_master->append_conf('postgresql.conf', "polar_enable_promote_wait_for_walreceive_done = on");
$node_master->start;
$node_master->polar_create_slot($node_active_datamax->name);
$node_active_datamax->append_conf('postgresql.conf', "polar_enable_promote_wait_for_walreceive_done = on");
$node_active_datamax->start;
$node_backup_datamax->start;

$node_standby->append_conf('postgresql.conf', "synchronous_commit = on");
$node_standby->append_conf('postgresql.conf', "synchronous_standby_names='".$node_standby_datamax->name."'");
$node_standby->append_conf('postgresql.conf', "polar_enable_promote_wait_for_walreceive_done = on");
$node_standby->start;
$node_standby->polar_create_slot($node_standby_datamax->name);
$node_standby_datamax->append_conf('postgresql.conf', "polar_enable_promote_wait_for_walreceive_done = on");
$node_standby_datamax->start;

$node_standby1->append_conf('postgresql.conf', "synchronous_commit = on");
$node_standby1->append_conf('postgresql.conf', "synchronous_standby_names='".$node_standby1_datamax->name."'");
$node_standby1->start;
$node_standby1->polar_create_slot($node_standby1_datamax->name);
$node_standby1_datamax->append_conf('postgresql.conf', "polar_enable_promote_wait_for_walreceive_done = on");
$node_standby1_datamax->start;

# wait for last node start
my $wait_time = 30;
$node_standby1_datamax->polar_wait_for_startup($wait_time);

# 1.check whether wal streaming is normal
my $wait_timeout = 50;
my $adm_walpid = $node_active_datamax->wait_walstreaming_establish_timeout($wait_timeout);
print "walreceiver pid of active_datamax: $adm_walpid\n";
ok($adm_walpid != 0, "walstream between master and active_datamax is normal");
my $bdm_walpid = $node_backup_datamax->wait_walstreaming_establish_timeout($wait_timeout);
print "walreceiver pid of backup_datamax: $bdm_walpid\n";
ok($bdm_walpid != 0, "walstream between active_datamax and backup_datamax is normal");
my $standby_walpid = $node_standby->wait_walstreaming_establish_timeout($wait_timeout);
print "walreceiver pid of standby: $standby_walpid\n";
ok($standby_walpid != 0, "walstream between active_datamax and standby is normal");
my $standbydm_walpid = $node_standby_datamax->wait_walstreaming_establish_timeout($wait_timeout);
print "walreceiver pid of standby_datamax: $standbydm_walpid\n";
ok($standbydm_walpid != 0, "walstream between standby_datamax and standby is normal");
my $standby1_walpid = $node_standby1->wait_walstreaming_establish_timeout($wait_timeout);
print "walreceiver pid of standby1: $standby1_walpid\n";
ok($standby1_walpid != 0, "walstream between active_datamax and standby1 is normal");
my $standby1dm_walpid = $node_standby1_datamax->wait_walstreaming_establish_timeout($wait_timeout);
print "walreceiver pid of standby1_datamax: $standby1dm_walpid\n";
ok($standby1dm_walpid != 0, "walstream between standby1_datamax and standby1 is normal");

# insert data in master and check whether other node received the data
$node_master->safe_psql('postgres', 'CREATE TABLE test_table(val integer);');
my $newval = $node_master->safe_psql('postgres',
		'INSERT INTO test_table(val) SELECT coalesce(max(val),0) + 1 AS newval FROM test_table RETURNING val');
my $result = $node_master->safe_psql('postgres',
		qq[SELECT 1 FROM test_table WHERE val = $newval]);
ok($result == 1, "master insert data success");

my $insert_lsn = $node_master->lsn('insert');
print "insert_lsn: $insert_lsn\n";
# there may will be new wal after insert action
$result = 0;
$result = $node_active_datamax->safe_psql('postgres',
		"SELECT last_received_lsn >= '$insert_lsn' FROM polar_get_datamax_info()");
ok($result eq "t", "active_datamax received the wal");
# wait for async node receive the wal, $stanby1_datamax is the node with longest chain
$node_standby1->wait_for_catchup($node_standby1_datamax, 'flush', $insert_lsn);
$result = 0;
$result = $node_backup_datamax->safe_psql('postgres',
		"SELECT last_received_lsn >= '$insert_lsn' FROM polar_get_datamax_info()");
ok($result eq "t", "backup_datamax received the wal");
$result = 0;
$result = $node_standby->safe_psql('postgres',
		qq[SELECT 1 FROM test_table WHERE val = $newval]);
ok($result == 1, "standby replayed the data");
$result = 0;
$result = $node_standby1->safe_psql('postgres',
		qq[SELECT 1 FROM test_table WHERE val = $newval]);
ok($result == 1, "standby1 replayed the data");
$result = 0;
$result = $node_standby_datamax->safe_psql('postgres',
		"SELECT last_received_lsn >= '$insert_lsn' FROM polar_get_datamax_info()");
ok($result eq "t", "standby_datamax received the wal");
$result = 0;
$result = $node_standby1_datamax->safe_psql('postgres',
		"SELECT last_received_lsn >= '$insert_lsn' FROM polar_get_datamax_info()");
ok($result eq "t", "standby1_datamax received the wal");

# 2.promote standby when master stops
$node_master->stop;
$node_standby->promote_constraint(0);
$node_standby->polar_wait_for_startup($wait_time, 0);

# wait for standby_datamax received all wal
$insert_lsn = $node_standby->lsn('insert');
$node_standby->wait_for_catchup($node_standby_datamax, 'flush', $insert_lsn);
$node_standby1->stop;
$node_standby_datamax->polar_create_slot($node_standby1->name);
$node_standby1->polar_standby_set_recovery($node_standby_datamax);
$node_standby1->start;

# build new standby node from original node_master
my $node_standby2 = PostgreSQL::Test::Cluster->new('standby2');
$node_standby2->polar_init_standby_no_recovery();
$node_standby2->polar_set_root_node($node_master);
$node_standby2->polar_standby_build_data;
$node_standby2->polar_standby_set_recovery($node_standby_datamax);
$node_standby2->append_conf('postgresql.conf', "polar_enable_promote_wait_for_walreceive_done = on");
$node_standby2->append_conf('postgresql.conf', "synchronous_standby_names='".$node_active_datamax->name."'");

$node_standby_datamax->polar_create_slot($node_standby2->name);
$node_active_datamax->stop;
$node_standby2->start;

# wait for last node start
$node_active_datamax->polar_datamax_set_recovery($node_standby2);
$node_active_datamax->start;
$node_active_datamax->polar_wait_for_startup($wait_time);

# check whether wal streaming is normal after promote
$adm_walpid = $node_active_datamax->wait_walstreaming_establish_timeout($wait_timeout);
print "walreceiver pid of active_datamax: $adm_walpid\n";
ok($adm_walpid != 0, "walstream between standby2 and active_datamax is normal");
$bdm_walpid = $node_backup_datamax->wait_walstreaming_establish_timeout($wait_timeout);
print "walreceiver pid of backup_datamax: $bdm_walpid\n";
ok($bdm_walpid != 0, "walstream between active_datamax and backup_datamax is normal");
$standbydm_walpid = $node_standby_datamax->wait_walstreaming_establish_timeout($wait_timeout);
print "walreceiver pid of standby_datamax: $standbydm_walpid\n";
ok($standbydm_walpid != 0, "walstream between standby_datamax and standby is normal");
$standby1_walpid = $node_standby1->wait_walstreaming_establish_timeout($wait_timeout);
print "walreceiver pid of standby1: $standby1_walpid\n";
ok($standby1_walpid != 0, "walstream between standby_datamax and standby1 is normal");
$standby1dm_walpid = $node_standby1_datamax->wait_walstreaming_establish_timeout($wait_timeout);
print "walreceiver pid of standby1_datamax: $standby1dm_walpid\n";
ok($standby1dm_walpid != 0, "walstream between standby1_datamax and standby1 is normal");
my $standby2_walpid = $node_standby2->wait_walstreaming_establish_timeout($wait_timeout);
print "walreceiver pid of standby2: $standby2_walpid\n";
ok($standby1_walpid != 0, "walstream between standby_datamax and standby2 is normal");

# insert data in new master and check whether other node received the data
$newval = $node_standby->safe_psql('postgres',
		'INSERT INTO test_table(val) SELECT coalesce(max(val),0) + 1 AS newval FROM test_table RETURNING val');
$result = $node_standby->safe_psql('postgres',
		qq[SELECT 1 FROM test_table WHERE val = $newval]);
ok($result == 1, "new master insert data success");

$insert_lsn = $node_standby->lsn('insert');
print "insert_lsn: $insert_lsn\n";
$result = 0;
$result = $node_standby_datamax->safe_psql('postgres',
		"SELECT last_received_lsn >= '$insert_lsn' FROM polar_get_datamax_info()");
ok($result eq "t", "standby_datamax received the wal");

# wait for receiving wal in async mode,backup_datamax is the node with longest chain
$node_active_datamax->wait_for_catchup($node_backup_datamax, 'flush', $insert_lsn);
$result = 0;
$result = $node_active_datamax->safe_psql('postgres',
		"SELECT last_received_lsn >= '$insert_lsn' FROM polar_get_datamax_info()");
ok($result eq "t", "active_datamax received the wal");
$result = 0;
$result = $node_backup_datamax->safe_psql('postgres',
		"SELECT last_received_lsn >= '$insert_lsn' FROM polar_get_datamax_info()");
ok($result eq "t", "backup_datamax received the wal");
$result = 0;
$result = $node_standby1->safe_psql('postgres',
		qq[SELECT 1 FROM test_table WHERE val = $newval]);
ok($result == 1, "standby1 replayed the data");
$result = 0;
$result = $node_standby1_datamax->safe_psql('postgres',
		"SELECT last_received_lsn >= '$insert_lsn' FROM polar_get_datamax_info()");
ok($result eq "t", "standby1_datamax received the wal");
$result = 0;
$result = $node_standby2->safe_psql('postgres',
		qq[SELECT 1 FROM test_table WHERE val = $newval]);
ok($result == 1, "standby2 replayed the data");

# 3. stop master(which is promoted from original standby), and promote standby2, stop another standby during this time
# stop standby1 and insert data in master
$node_standby1->stop;
my $val_before_promote = $node_standby->safe_psql('postgres',
		'INSERT INTO test_table(val) SELECT coalesce(max(val),0) + 1 AS newval FROM test_table RETURNING val');
$result = $node_standby->safe_psql('postgres',
		qq[SELECT 1 FROM test_table WHERE val = $val_before_promote]);
print "new master insert data result: $result\n";
ok($result == 1, "new master insert data success");

# stop master and promote standby2
$node_standby->stop;
$node_standby2->promote_constraint(0);
$node_standby2->polar_wait_for_startup($wait_time, 0);
$node_standby_datamax->stop;

# check whether standby has received all data before promote
$result = 0;
$result = $node_standby2->safe_psql('postgres',
		qq[SELECT 1 FROM test_table WHERE val = $val_before_promote]);
print "standby2 select result before promote: $result\n";
ok($result == 1, "standby2 received all data before promote");

# insert data in new master node
$result = 0;
my $val_after_promote = $node_standby2->safe_psql('postgres',
		'INSERT INTO test_table(val) SELECT coalesce(max(val),0) + 1 AS newval FROM test_table RETURNING val');
$result = $node_standby2->safe_psql('postgres',
		qq[SELECT 1 FROM test_table WHERE val = $val_after_promote]);
$insert_lsn = $node_standby2->lsn('insert');
print "standby2 insert data result after promote: $result\n";
ok($result == 1, "standby2 insert data success after promote");

# start standby1 and check whether it received data inserted before/after promote in master 
$node_standby1->polar_standby_set_recovery($node_active_datamax);
$node_standby1->start;
$node_standby1->polar_wait_for_startup($wait_time); 

# check whether wal streaming is normal after promote
$standby1_walpid = $node_standby1->wait_walstreaming_establish_timeout($wait_timeout);
print "walreceiver pid of standby1: $standby1_walpid\n";
ok($standby1_walpid != 0, "walstream between active_datamax and standby1 is normal");
$standby1dm_walpid = $node_standby1_datamax->wait_walstreaming_establish_timeout($wait_timeout);
print "walreceiver pid of standby1_datamax: $standby1dm_walpid\n";
ok($standby1dm_walpid != 0, "walstream between standby1_datamax and standby1 is normal");

# check whether standby has received the data inserted before/after promote in master
$node_active_datamax->wait_for_catchup($node_standby1, 'replay', $insert_lsn);
$result = 0;
$result = $node_standby1->safe_psql('postgres',
		qq[SELECT 1 FROM test_table WHERE val = $val_before_promote]);
print "standby1 select val_before_promote result: $result\n";
ok($result == 1, "standby1 received data inserted before promote");
$result = 0;
$result = $node_standby1->safe_psql('postgres',
		qq[SELECT 1 FROM test_table WHERE val = $val_after_promote]);
print "standby1 select val_after_promote result: $result\n";
ok($result == 1, "standby1 received data inserted after promote");

# stop clusters
$node_standby1_datamax->stop;
$node_standby1->stop;
$node_backup_datamax->stop;
$node_active_datamax->stop;
$node_standby2->stop;


