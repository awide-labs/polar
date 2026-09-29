# Test for datamax get wal from backup set
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use File::Path 'rmtree';
use Test::More tests=>17;

# master
my $node_master = PostgreSQL::Test::Cluster->new('master');
$node_master->polar_init_primary();
my $backup_dir = $node_master->archive_dir;
$node_master->append_conf('postgresql.conf', "archive_mode = on");
$node_master->append_conf('postgresql.conf', "archive_command = 'cp \%p $backup_dir/\%f'");
$node_master->append_conf('postgresql.conf', "wal_keep_size = 0");
$node_master->append_conf('postgresql.conf', "polar_wal_pipeline_enable = false");
$node_master->append_conf('postgresql.conf', "polar_logindex_mem_size = 0");

# standby
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->polar_init_standby_no_recovery();
$node_standby->polar_set_root_node($node_master);
$node_standby->polar_standby_build_data;
$node_standby->append_conf('postgresql.conf', "polar_wal_pipeline_enable = false");
$node_standby->append_conf('postgresql.conf', "polar_logindex_mem_size = 0");

# standby1
my $node_standby1 = PostgreSQL::Test::Cluster->new('standby1');
$node_standby1->polar_init_standby_no_recovery();
$node_standby1->polar_set_root_node($node_master);
$node_standby1->polar_standby_build_data;
$node_standby1->append_conf('postgresql.conf', "polar_wal_pipeline_enable = false");
$node_standby1->append_conf('postgresql.conf', "polar_logindex_mem_size = 0");

# get current wal file 
sub polar_get_walfile
{
	my ($node, $is_datamax) = @_;
	my $polar_waldir = $node->polar_get_datadir;
	my $name = $node->name;
	if ($is_datamax == 1)
	{
		$polar_waldir = "$polar_waldir/polar_datamax/pg_wal";
	}
	else
	{
		$polar_waldir = "$polar_waldir/pg_wal";
	}
	my $walfile = readpipe("ls $polar_waldir");
	my @wal_array = split('\n', $walfile);
	print "current walfile of node $name:\n";
	foreach my $i (@wal_array)  
	{ 
		print "$i\n";
	}
	return @wal_array;
}

# judge whether wal file is the same
sub polar_walfile_compare
{
	my ($walfile1, $walfile2, $waldir1, $waldir2, $last_segno) = @_;
	my $ret = 0;
	foreach my $i (@$walfile1)  
	{ 
		if ($i le $last_segno)
		{
			if(grep { $_ eq $i } @$walfile2)
			{
				my $wal1 = $waldir1 . "/" . $i;
                my $md51 = readpipe("md5sum $wal1");
                $md51 = (split(" ",$md51))[0];
                $wal1 = $waldir2 . "/" . $i;
                my $md52 = readpipe("md5sum $wal1");
                $md52 = (split(" ",$md52))[0];
                if ($md51 eq $md52)
                {
                    $ret = 1;
                }
				else
				{
					$ret = 0;
					last;
				}
			}
			else
			{
				$ret = 0;
				last;
			}
		}
	}
	return $ret;
}

sub get_wal_from_backup
{
	my ($self, $backup, $datamax_pfs, $backup_pfs) = @_;
	my $pgdata  = $self->polar_get_datadir;
	my $name    = $self->name; 
	print "### node \"$name\" get wal from backup_set \"$backup\" \n";
	my $ret = system("polar_tools datamax-get-wal -D $pgdata -M $datamax_pfs -b $backup -m $backup_pfs");
	return $ret;
}

# clear content of archive_test
rmtree($backup_dir) or warn "cannot rmtree '$backup_dir'";
mkdir $backup_dir, 0755 or warn "cannot mkdir '$backup_dir'";

$node_master->start;
$node_master->safe_psql('postgres', 'CREATE TABLE test_table(val integer);');
# insert data to generate wal segment
my $val_before;
my $val_after;
my $insertlsn_before;
my $result;
my $count = 0;
my $total_count = 1;
do
{
	if ($count == 0)
	{
		$val_before = $node_master->safe_psql('postgres',
			'INSERT INTO test_table(val) SELECT coalesce(max(val),0) + 1 AS newval FROM test_table RETURNING val');
		$insertlsn_before = $node_master->lsn('insert');
	}
	# pg_switch_wal() to generate wal 
	$node_master->safe_psql('postgres', "select pg_switch_wal();");
	$count = $count + 1;
}while($count < $total_count);
$val_after = $node_master->safe_psql('postgres',
		'INSERT INTO test_table(val) SELECT coalesce(max(val),0) + 1 AS newval FROM test_table RETURNING val');
my $insert_lsn = $node_master->lsn('insert');

my $last_segno = $node_master->safe_psql('postgres', "select pg_walfile_name('$insert_lsn');");
print "last_segno: $last_segno\n";

### 1. create new datamax, get wal from backup_set before start datamax and streaming from master
my $primary_system_identifier = $node_master->polar_get_system_identifier;
my $node_datamax = PostgreSQL::Test::Cluster->new('datamax');
$node_datamax->polar_init_datamax($primary_system_identifier);
$node_master->polar_create_slot($node_datamax->name);
$node_datamax->start;
$node_datamax->safe_psql('postgres','CREATE EXTENSION polar_monitor;');
# Freeze so polar_get_datamax_info() stays resolvable after the datamax
# node is restarted below.
$node_datamax->safe_psql('postgres','VACUUM (FREEZE);');
$node_datamax->stop;
$node_datamax->polar_datamax_set_recovery($node_master);

# test for some exception scenario
my $datamax_dirfile = $node_datamax->polar_get_datadir;
$datamax_dirfile .= "/polar_datamax";
system("touch $datamax_dirfile");
get_wal_from_backup($node_datamax, $backup_dir, 'l', 'l');
system("rm -rf $datamax_dirfile");

# get wal from backup set
my $copy_ret = get_wal_from_backup($node_datamax, $backup_dir, 'l', 'l');
print "copy wal from backup set result: $copy_ret\n";
ok($copy_ret == 0, "datamax copy from backup set success");
my $backup_walfile = readpipe("ls $backup_dir");
my @backup_wal = split('\n', $backup_walfile);
my @datamax_wal = polar_get_walfile($node_datamax, 1);
my $datamax_waldir = $node_datamax->polar_get_datadir;
$datamax_waldir = "$datamax_waldir/polar_datamax/pg_wal";
$result = 0;
$result = polar_walfile_compare(\@backup_wal, \@datamax_wal, $backup_dir, $datamax_waldir, $last_segno);
print "datamax wal compares with backup set result: $result\n";
ok($result == 1, "datamax wal is the same as backup set\n"); 

# start datamax and check whether datamax can get wal via walstreaming normaly
my $wait_timeout = 40;
my $catchup_timeout = 500;
$node_datamax->start;
$node_datamax->wait_walstreaming_establish_timeout($wait_timeout);
$result = 0;
my $write_lsn = $node_master->lsn('write');
$result = $node_master->wait_for_catchup($node_datamax, 'flush',
	$write_lsn, return_failed => 1);
ok($result == 1, "datamax catchup success");
$result = 0;
$result = $node_datamax->safe_psql('postgres',
		"SELECT min_received_lsn < '$insertlsn_before' FROM polar_get_datamax_info()");
ok($result eq "t", "datamax streaming from master current wal success");
$result = 0;
$result = $node_datamax->safe_psql('postgres',
		"SELECT last_received_lsn >= '$write_lsn' FROM polar_get_datamax_info()");
ok($result eq "t", "datamax get all wal of master success");

### 2. start standby streaming from datamax, check whether standby received all data
# which is also to check whether datamax get all wal from backup_set and primary
$node_standby->polar_standby_set_recovery($node_datamax);
$node_datamax->polar_create_slot($node_standby->name);
$node_standby->start;
$node_standby->wait_walstreaming_establish_timeout($wait_timeout);
# wait for standby catchup
$result = 0;
$result = $node_datamax->wait_for_catchup($node_standby, 'replay',
	$insert_lsn, timeout => $catchup_timeout, return_failed => 1);
ok($result == 1, "standby catchup success");

$result = 0;
$result = $node_standby->safe_psql('postgres',
		qq[SELECT 1 FROM test_table WHERE val = $val_before]);
print "select val_before result in standby: $result\n";
ok($result == 1, "receive val_before pgbench in standby node");
$result = 0;
$result = $node_standby->safe_psql('postgres',
		qq[SELECT 1 FROM test_table WHERE val = $val_after]);
print "select val_after result in standby: $result\n";
ok($result == 1, "receive val_after pgbench in standby node");

# stop standby and datamax
$node_standby->stop;
$node_datamax->stop;

### 3. create new datamax1, get wal from backup_set after streaming from master
# do checkpoint
$node_master->safe_psql('postgres', "checkpoint");
sleep 5;

my $node_datamax1 = PostgreSQL::Test::Cluster->new('datamax1');
$node_datamax1->polar_init_datamax($primary_system_identifier);
$node_master->polar_create_slot($node_datamax1->name);
$node_datamax1->start;
$node_datamax1->safe_psql('postgres','CREATE EXTENSION polar_monitor;');
# Freeze so polar_get_datamax_info() stays resolvable after the datamax
# node is restarted below.
$node_datamax1->safe_psql('postgres','VACUUM (FREEZE);');
$node_datamax1->stop;
$node_datamax1->append_conf('postgresql.conf', "polar_logindex_mem_size = 0");
$node_datamax1->append_conf('postgresql.conf', "polar_wal_pipeline_enable = false");
$node_datamax1->polar_datamax_set_recovery($node_master);

# start datamax1 and streaming from master, check the wal received from master
$node_datamax1->start;
$node_datamax1->wait_walstreaming_establish_timeout($wait_timeout);
# wait for datamax1 catchup
$result = 0;
$write_lsn = $node_master->lsn('write');
$result = $node_master->wait_for_catchup($node_datamax1, 'flush',
	$write_lsn, return_failed => 1);
ok($result == 1, "datamax1 catchup success");
$result = 0;
$result = $node_datamax1->safe_psql('postgres',
		"SELECT min_received_lsn > '$insertlsn_before' FROM polar_get_datamax_info()");
ok($result eq "t", "datamax1 streaming from master current wal success");
$result = 0;
$result = $node_datamax1->safe_psql('postgres',
		"SELECT last_received_lsn >= '$write_lsn' FROM polar_get_datamax_info()");
ok($result eq "t", "datamax1 get all wal of master success");

# get wal from backup set after datamax1 streaming from master
$copy_ret = 1;
$copy_ret = get_wal_from_backup($node_datamax1, $backup_dir, 'l', 'l');
print "datamax1 copy wal from backup set result: $copy_ret\n";
ok($copy_ret == 0, "datamax1 copy from backup set success");
# restart datamax1 to reload meta info
$node_datamax1->restart;
# check whether datamax1 has gotten the wal which has been removed in master
$result = 0;
$result = $node_datamax1->safe_psql('postgres',
		"SELECT min_received_lsn < '$insertlsn_before' FROM polar_get_datamax_info()");
ok($result eq "t", "datamax1 get wal which has been removed in master success");
$result = 0;
$result = $node_datamax1->safe_psql('postgres',
		"SELECT last_received_lsn >= '$write_lsn' FROM polar_get_datamax_info()");
ok($result eq "t", "datamax1 don't update last received info after getting wal from backup and there is wal in datamax before");

# start standby1 to check whether it can receive all data from datamax1
$node_standby1->polar_standby_set_recovery($node_datamax1);
$node_datamax1->polar_create_slot($node_standby1->name);
$node_standby1->start;
$node_standby1->wait_walstreaming_establish_timeout($wait_timeout);
# wait for standby1 catchup
$result = 0;
$result = $node_datamax1->wait_for_catchup($node_standby1, 'replay',
	$insert_lsn, timeout => $catchup_timeout, return_failed => 1);
ok($result == 1, "standby1 catchup success");

$result = 0;
$result = $node_standby1->safe_psql('postgres',
		qq[SELECT 1 FROM test_table WHERE val = $val_before]);
print "select val_before result in standby1: $result\n";
ok($result == 1, "receive val_before pgbench in standby1 node");
$result = 0;
$result = $node_standby1->safe_psql('postgres',
		qq[SELECT 1 FROM test_table WHERE val = $val_after]);
print "select val_after result in standby1: $result\n";
ok($result == 1, "receive val_after pgbench in standby1 node");

$node_standby1->stop;
$node_datamax1->stop;
$node_master->stop;
