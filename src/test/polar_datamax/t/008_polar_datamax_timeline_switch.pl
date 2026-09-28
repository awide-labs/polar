# Test for whether datamax work normally after timeline switch
use strict;
use warnings;
use FindBin;
use lib $FindBin::RealBin;
use PolarDatamaxUtils;
use PostgreSQL::Test::Cluster;
use File::Path 'rmtree';
use Test::More tests=>22;

#### 1rw 1datamax 2standby promote standby and datamax streaming from new rw
#### check whether standby1 can receive data normally

# master
my $node_master = PostgreSQL::Test::Cluster->new('master');
$node_master->polar_init_primary();

# datamax
my $primary_system_identifier = $node_master->polar_get_system_identifier;
my $node_datamax = PostgreSQL::Test::Cluster->new('datamax');
$node_datamax->polar_init_datamax($primary_system_identifier);
$node_datamax->append_conf('postgresql.conf', "polar_enable_promote_wait_for_walreceive_done = on");
$node_datamax->append_conf('postgresql.conf', "polar_wal_pipeline_enable = false");
$node_datamax->append_conf('postgresql.conf', "polar_logindex_mem_size = 0");
$node_datamax->start;
$node_datamax->safe_psql('postgres','CREATE EXTENSION polar_monitor;');
$node_datamax->stop;
$node_datamax->polar_datamax_set_recovery($node_master);

# standby
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->polar_init_standby_no_recovery();
$node_standby->polar_set_root_node($node_master);
$node_standby->polar_standby_build_data;
$node_standby->append_conf('postgresql.conf', "polar_enable_promote_wait_for_walreceive_done = on");
$node_standby->append_conf('postgresql.conf', "wal_keep_size = 80");
$node_standby->append_conf('postgresql.conf', "polar_wal_pipeline_enable = false");
$node_standby->append_conf('postgresql.conf', "polar_logindex_mem_size = 0");
$node_standby->polar_standby_set_recovery($node_datamax);

# standby1
my $node_standby1 = PostgreSQL::Test::Cluster->new('standby1');
$node_standby1->polar_init_standby_no_recovery();
$node_standby1->polar_set_root_node($node_master);
$node_standby1->polar_standby_build_data;
$node_standby1->append_conf('postgresql.conf', "polar_enable_promote_wait_for_walreceive_done = on");
$node_standby1->append_conf('postgresql.conf', "wal_keep_size = 80");
$node_standby1->append_conf('postgresql.conf', "polar_wal_pipeline_enable = false");
$node_standby1->append_conf('postgresql.conf', "polar_logindex_mem_size = 0");
$node_standby1->polar_standby_set_recovery($node_datamax);

# start cluster
$node_master->append_conf('postgresql.conf', "synchronous_commit = on");
$node_master->append_conf('postgresql.conf', "synchronous_standby_names='".$node_datamax->name."'");
$node_master->append_conf('postgresql.conf', "polar_enable_promote_wait_for_walreceive_done = on");
$node_master->append_conf('postgresql.conf', "polar_wal_pipeline_enable = false");
$node_master->append_conf('postgresql.conf', "polar_logindex_mem_size = 0");
$node_master->start;
$node_master->polar_create_slot($node_datamax->name);
$node_datamax->start;
$node_datamax->polar_create_slot($node_standby->name);
$node_datamax->polar_create_slot($node_standby1->name);
$node_standby->start;
$node_standby1->start;

# insert data to generate wal segment
$node_master->safe_psql('postgres', 'CREATE TABLE test_table(val integer);');
my $val_before;
my $val_after;
my $result;
my $count = 0;
my $total_count = 1;
do
{
	if ($count == 0)
	{
		$val_before = $node_master->safe_psql('postgres',
			'INSERT INTO test_table(val) SELECT coalesce(max(val),0) + 1 AS newval FROM test_table RETURNING val');
	}
	# pg_switch_wal() to generate wal 
	$node_master->safe_psql('postgres', "select pg_switch_wal();");	
	$count = $count + 1;
}while($count < $total_count);
$val_after = $node_master->safe_psql('postgres',
	'INSERT INTO test_table(val) SELECT coalesce(max(val),0) + 1 AS newval FROM test_table RETURNING val');

# promote standby, check whether standby received all data before promote
$node_master->stop;
my $wait_timeout = 50;
my $walpid = 0;
$walpid = $node_standby->wait_walstreaming_establish_timeout($wait_timeout);
ok($walpid != 0, "walstream between datamax and standby is normal");
my $promote_ret = $node_standby->promote_constraint(0);
ok($promote_ret == 0, "promote success when master stops running and datamax runs normally");
$result = 0;
$result = $node_standby->safe_psql('postgres',
		qq[SELECT 1 FROM test_table WHERE val = $val_before]);
ok($result == 1, "standby has received data before switch_wal before promote");
$result = 0;
$result = $node_standby->safe_psql('postgres',
		qq[SELECT 1 FROM test_table WHERE val = $val_after]);
ok($result == 1, "standby has received data after switch_wal before promote");

# change datamax streaming from new master(old standby)
$node_standby->polar_create_slot($node_datamax->name);
$node_standby->safe_psql('postgres', 'ALTER SYSTEM SET synchronous_commit = on;');
$node_standby->safe_psql('postgres', "ALTER SYSTEM SET synchronous_standby_names='".$node_datamax->name."';");
$node_standby->reload;
$node_datamax->stop;
$node_datamax->polar_datamax_set_recovery($node_standby);
$node_datamax->start;

# check whether walstreaming is normal
my $catchup_timeout = 500;
my $datamax_walpid = $node_datamax->wait_walstreaming_establish_timeout($wait_timeout);
print "walreceiver pid of datamax: $datamax_walpid\n";
ok($datamax_walpid != 0, "walstreaming between new master and datamax is normal");
my $standby1_walpid = $node_standby1->wait_walstreaming_establish_timeout($wait_timeout);
print "walreceiver pid of standby1: $standby1_walpid\n";
ok($standby1_walpid != 0, "walstreaming between datamax and standby1 is normal");

# insert data in new master(old standby)
$count = 0;
do
{
	if ($count == 0)
	{
		$val_before = $node_standby->safe_psql('postgres',
			'INSERT INTO test_table(val) SELECT coalesce(max(val),0) + 1 AS newval FROM test_table RETURNING val');
	}
	# pg_switch_wal() to generate wal 
	$node_standby->safe_psql('postgres', "select pg_switch_wal();");
	$count = $count + 1;
}while($count < $total_count);
$result = 0;
my $val_newrw = $node_standby->safe_psql('postgres',
		'INSERT INTO test_table(val) SELECT coalesce(max(val),0) + 1 AS newval FROM test_table RETURNING val');
$result = $node_standby->safe_psql('postgres',
		qq[SELECT 1 FROM test_table WHERE val = $val_newrw]);
ok($result == 1, "insert success in new master(old standby) node");
my $insert_lsn = $node_standby->lsn('insert');

# check whether standby1 receive all data, and whether wal of standby1 is the same as new master(old standby)
# wait for standby1 catchup
$result = 0;
$result = $node_datamax->wait_for_catchup($node_standby1, 'replay',
	$insert_lsn, timeout => $catchup_timeout, return_failed => 1);
ok($result == 1, "standby1 catchup success");
$result = 0;
$result = $node_standby1->safe_psql('postgres',
		qq[SELECT 1 FROM test_table WHERE val = $val_newrw]);
ok($result == 1, "standby1 received all data success");
# the new master can write more wal at any time, so compare the wal up to
# the lsn it has flushed now, once standby1 has flushed it too
my $cmp_lsn = $node_standby->lsn('flush');
print "cmp_lsn: $cmp_lsn\n";
$result = 0;
if ($node_datamax->wait_for_catchup($node_standby1, 'flush', $cmp_lsn, 1, 't', $catchup_timeout))
{
	polar_get_walfile($node_standby, 0);
	polar_get_walfile($node_standby1, 0);
	$result = polar_walfile_compare(polar_waldir($node_standby, 0),
		polar_waldir($node_standby1, 0), $cmp_lsn, polar_wal_segment_size($node_standby));
}
print "standby1 wal compares with standby wal result: $result\n";
ok($result == 1, "standby wal is the same as standby1 wal\n"); 

$node_standby1->stop;
$node_standby1->clean_node;
$node_standby->stop;
$node_standby->clean_node;
$node_datamax->stop;
$node_datamax->clean_node;
$node_master->stop;
$node_master->clean_node;


#### 1rw 1datamax 2standby, promote standby1 and rebuild datamax node getting wal from backupset
#### check whether standby2 can receive data normally
# master1
my $node_master1 = PostgreSQL::Test::Cluster->new('master1');
$node_master1->polar_init_primary();
my $backup_dir = $node_master1->archive_dir;
$node_master1->append_conf('postgresql.conf', "archive_mode = on");
$node_master1->append_conf('postgresql.conf', "archive_command = 'cp \%p $backup_dir/\%f'");

# datamax1
$primary_system_identifier = $node_master1->polar_get_system_identifier;
my $node_datamax1 = PostgreSQL::Test::Cluster->new('datamax1');
$node_datamax1->polar_init_datamax($primary_system_identifier);
$node_datamax1->append_conf('postgresql.conf', "polar_enable_promote_wait_for_walreceive_done = on");
$node_datamax1->append_conf('postgresql.conf', "polar_wal_pipeline_enable = false");
$node_datamax1->append_conf('postgresql.conf', "polar_logindex_mem_size = 0");
$node_datamax1->start;
$node_datamax1->safe_psql('postgres','CREATE EXTENSION polar_monitor;');
$node_datamax1->stop;
$node_datamax1->polar_datamax_set_recovery($node_master1);

# standby2
my $node_standby2 = PostgreSQL::Test::Cluster->new('standby2');
$node_standby2->polar_init_standby_no_recovery();
$node_standby2->polar_set_root_node($node_master1);
$node_standby2->polar_standby_build_data;
$node_standby2->append_conf('postgresql.conf', "polar_enable_promote_wait_for_walreceive_done = on");
$node_standby2->append_conf('postgresql.conf', "wal_keep_size = 80");
$node_standby2->append_conf('postgresql.conf', "polar_wal_pipeline_enable = false");
$node_standby2->append_conf('postgresql.conf', "polar_logindex_mem_size = 0");
$node_standby2->polar_standby_set_recovery($node_datamax1);

# standby3
my $node_standby3 = PostgreSQL::Test::Cluster->new('standby3');
$node_standby3->polar_init_standby_no_recovery();
$node_standby3->polar_set_root_node($node_master1);
$node_standby3->polar_standby_build_data;
$node_standby3->append_conf('postgresql.conf', "polar_enable_promote_wait_for_walreceive_done = on");
$node_standby3->append_conf('postgresql.conf', "wal_keep_size = 80");
$node_standby3->append_conf('postgresql.conf', "polar_wal_pipeline_enable = false");
$node_standby3->append_conf('postgresql.conf', "polar_logindex_mem_size = 0");
$node_standby3->polar_standby_set_recovery($node_datamax1);

# start cluster
$node_master1->append_conf('postgresql.conf', "synchronous_commit = on");
$node_master1->append_conf('postgresql.conf', "synchronous_standby_names='".$node_datamax1->name."'");
$node_master1->append_conf('postgresql.conf', "polar_enable_promote_wait_for_walreceive_done = on");
$node_master1->append_conf('postgresql.conf', "polar_wal_pipeline_enable = false");
$node_master1->append_conf('postgresql.conf', "polar_logindex_mem_size = 0");
$node_master1->start;
$node_master1->polar_create_slot($node_datamax1->name);
$node_datamax1->start;
$node_datamax1->polar_create_slot($node_standby2->name);
$node_datamax1->polar_create_slot($node_standby3->name);
$node_standby2->start;
$node_standby3->start;

# clear content of archive_test
rmtree($backup_dir) or warn "cannot rmtree '$backup_dir'";
mkdir $backup_dir, 0755 or warn "cannot mkdir '$backup_dir'";

# insert data to generate wal
$node_master1->safe_psql('postgres', 'CREATE TABLE test_table(val integer);');
$count = 0;
do
{
	if ($count == 0)
	{
		$val_before = $node_master1->safe_psql('postgres',
			'INSERT INTO test_table(val) SELECT coalesce(max(val),0) + 1 AS newval FROM test_table RETURNING val');
	}
	# pg_switch_wal() to generate wal 
	$node_master1->safe_psql('postgres', "select pg_switch_wal();");	
	$count = $count + 1;
}while($count < $total_count);
$val_after = $node_master1->safe_psql('postgres',
	'INSERT INTO test_table(val) SELECT coalesce(max(val),0) + 1 AS newval FROM test_table RETURNING val');

# promote standby2, check whether standby received all data before promote
# do checkpoint
$node_master1->safe_psql('postgres', "checkpoint");
sleep 5;
$node_master1->stop;
$walpid = 0;
$walpid = $node_standby2->wait_walstreaming_establish_timeout($wait_timeout);
ok($walpid != 0, "walstream between datamax1 and standby2 is normal");
$promote_ret = $node_standby2->promote_constraint(0);
ok($promote_ret == 0, "promote success when master stops running and datamax runs normally");
$result = 0;
$result = $node_standby2->safe_psql('postgres',
		qq[SELECT 1 FROM test_table WHERE val = $val_before]);
ok($result == 1, "standby2 has received data before switch_wal before promote");
$result = 0;
$result = $node_standby2->safe_psql('postgres',
		qq[SELECT 1 FROM test_table WHERE val = $val_after]);
ok($result == 1, "standby2 has received data after switch_wal before promote");

# set archive_mode on new master(old standby)
$node_datamax1->stop;
$node_standby3->stop;
my $standby_backup_dir = $node_standby2->archive_dir;
rmtree($standby_backup_dir) or warn "cannot rmtree '$standby_backup_dir'";
mkdir $standby_backup_dir, 0755 or warn "cannot mkdir '$standby_backup_dir'";

$node_standby2->stop;
$node_standby2->append_conf('postgresql.conf', "archive_mode = on");
$node_standby2->append_conf('postgresql.conf', "archive_command = 'cp \%p $standby_backup_dir/\%f'");
$node_standby2->start;

# insert data in new master(old standby)
$count = 0;
do
{
	if ($count == 0)
	{
		$val_before = $node_standby2->safe_psql('postgres',
			'INSERT INTO test_table(val) SELECT coalesce(max(val),0) + 1 AS newval FROM test_table RETURNING val');
	}
	# pg_switch_wal() to generate wal 
	$node_standby2->safe_psql('postgres', "select pg_switch_wal();");	
	$count = $count + 1;
}while($count < $total_count);

# build new datamax node, get wal from backup_dir and standby_backup_dir
$primary_system_identifier = $node_standby2->polar_get_system_identifier;
my $node_datamax2 = PostgreSQL::Test::Cluster->new('datamax2');
$node_datamax2->polar_init_datamax($primary_system_identifier);
$node_datamax2->append_conf('postgresql.conf', "polar_wal_pipeline_enable = false");
$node_datamax2->append_conf('postgresql.conf', "polar_logindex_mem_size = 0");
$node_datamax2->start;
$node_datamax2->safe_psql('postgres','CREATE EXTENSION polar_monitor;');
$node_datamax2->stop;
$node_datamax2->polar_datamax_set_recovery($node_standby2);

# datamax2 get wal from standby_backup_dir, which is the backup_set of new master(old standby2)
# get wal from standby_backup_dir first, because the wal of standby2 is greater than those of master
# if get wal from backup_set of master first, we will get nothing from standby_backup_dir later
my $copy_ret = get_wal_from_backup($node_datamax2, $standby_backup_dir, 'l', 'l');
print "copy wal from standby backup set result: $copy_ret\n";
ok($copy_ret == 0, "datamax copy from standby backup set success");

# datamax2 get wal from backup_dir, which is the backup_set of old master
$copy_ret = get_wal_from_backup($node_datamax2, $backup_dir, 'l', 'l');
print "copy wal from backup set result: $copy_ret\n";
ok($copy_ret == 0, "datamax copy from backup set success");

$node_standby2->polar_create_slot($node_datamax2->name);
$node_standby2->safe_psql('postgres', "ALTER SYSTEM SET synchronous_standby_names='".$node_datamax2->name."';");
$node_standby2->reload;

# start datamax2 and standby3, check whether wal streaming is normal
$node_datamax2->start;
$node_datamax2->polar_create_slot($node_standby3->name);
$node_standby3->polar_standby_set_recovery($node_datamax2);
$node_standby3->start;
$datamax_walpid = $node_datamax2->wait_walstreaming_establish_timeout($wait_timeout);
print "walreceiver pid of datamax2: $datamax_walpid\n";
ok($datamax_walpid != 0, "walstreaming between new master and datamax2 is normal");
$standby1_walpid = $node_standby3->wait_walstreaming_establish_timeout($wait_timeout);
print "walreceiver pid of standby1: $standby1_walpid\n";
ok($standby1_walpid != 0, "walstreaming between datamax2 and standby3 is normal");

# insert new data in standby2
$result = 0;
$val_newrw = $node_standby2->safe_psql('postgres',
		'INSERT INTO test_table(val) SELECT coalesce(max(val),0) + 1 AS newval FROM test_table RETURNING val');
$result = $node_standby2->safe_psql('postgres',
		qq[SELECT 1 FROM test_table WHERE val = $val_newrw]);
ok($result == 1, "insert success in new master(old standby2) node");
$insert_lsn = $node_standby2->lsn('write');

# check whether standby3 receive all data, and whether wal of standby1 is the same as new master(old standby)
# wait for standby1 catchup
$result = 0;
$result = $node_datamax2->wait_for_catchup($node_standby3, 'replay',
	$insert_lsn, timeout => $catchup_timeout, return_failed => 1);
ok($result == 1, "standby3 catchup success");
# the new master can write more wal at any time, so compare the wal up to
# the lsn it has flushed now, once standby3 has flushed it too
$cmp_lsn = $node_standby2->lsn('flush');
print "cmp_lsn: $cmp_lsn\n";
$result = 0;
if ($node_datamax2->wait_for_catchup($node_standby3, 'flush', $cmp_lsn, 1, 't', $catchup_timeout))
{
	polar_get_walfile($node_standby2, 0);
	polar_get_walfile($node_standby3, 0);
	$result = polar_walfile_compare(polar_waldir($node_standby2, 0),
		polar_waldir($node_standby3, 0), $cmp_lsn, polar_wal_segment_size($node_standby2));
}
print "standby3 wal compares with standby2 wal result: $result\n";
ok($result == 1, "standby3 wal is the same as standby2 wal\n"); 

$result = 0;
$result = $node_standby3->poll_query_until('postgres', qq[SELECT 1 FROM test_table WHERE val = $val_newrw], '1');
ok($result == 1, "standby3 received all data success");

$node_standby3->stop;
$node_standby2->stop;
$node_datamax2->stop;
