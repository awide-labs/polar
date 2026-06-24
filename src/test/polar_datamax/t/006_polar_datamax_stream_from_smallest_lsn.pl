# Test for initial datamax start streaming from the smallest lsn of primary
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use Test::More tests=>4;

# master
my $node_master = PostgreSQL::Test::Cluster->new('master');
$node_master->polar_init_primary();
$node_master->append_conf('postgresql.conf', "wal_keep_size = 16");
$node_master->append_conf('postgresql.conf', "polar_wal_pipeline_enable = false");
$node_master->append_conf('postgresql.conf', "polar_logindex_mem_size = 0");
$node_master->start;

# datamax
my $primary_system_identifier = $node_master->polar_get_system_identifier;
my $node_datamax = PostgreSQL::Test::Cluster->new('datamax');
$node_datamax->polar_init_datamax($primary_system_identifier);
$node_master->polar_create_slot($node_datamax->name);

$node_datamax->start;
$node_datamax->safe_psql('postgres','CREATE EXTENSION polar_monitor;');
$node_datamax->stop;

# set datamax recovery.conf
$node_datamax->polar_datamax_set_recovery($node_master);
$node_datamax->append_conf('postgresql.conf', "polar_wal_pipeline_enable = false");
$node_datamax->append_conf('postgresql.conf', "polar_logindex_mem_size = 0");
$node_datamax->start;
$node_datamax->wait_walstreaming_establish_timeout(20);

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

# insert data to generate wal
my $wait_timeout = 40;
$node_master->safe_psql('postgres', 'CREATE TABLE test_table(val integer);');
my $count = 0;
my $total_count = 1;
do
{
	# pg_switch_wal() to generate wal 
	$node_master->safe_psql('postgres', "select pg_switch_wal();");	
	$count = $count + 1;
}while($count < $total_count);
$node_master->safe_psql('postgres',
	'INSERT INTO test_table(val) SELECT coalesce(max(val),0) + 1 AS newval FROM test_table RETURNING val');
my $insert_lsn = $node_master->lsn('insert');
my $last_segno = $node_master->safe_psql('postgres', "select pg_walfile_name('$insert_lsn');");
print "last_segno: $last_segno\n";

# do checkpoint to update redoptr
$node_master->safe_psql('postgres', "checkpoint");
sleep 10;

### 1. create new datamax, streaming from master
my $node_datamax1 = PostgreSQL::Test::Cluster->new('datamax1');
$node_datamax1->polar_init_datamax($primary_system_identifier);
$node_master->polar_create_slot($node_datamax1->name);

$node_datamax1->start;
$node_datamax1->safe_psql('postgres','CREATE EXTENSION polar_monitor;');
$node_datamax1->stop;
$node_datamax1->polar_datamax_set_recovery($node_master);
$node_datamax1->append_conf('postgresql.conf', "polar_wal_pipeline_enable = false");
$node_datamax1->append_conf('postgresql.conf', "polar_logindex_mem_size = 0");
$node_datamax1->start;
$node_datamax1->wait_walstreaming_establish_timeout($wait_timeout);
# check whether datamax start streaming from the smallest lsn of primary
# if so, the wal datamax received should be the same as those in primary
#wait for datamax received the wal
$node_master->wait_for_catchup($node_datamax1, 'flush', $insert_lsn);
my @master_wal = polar_get_walfile($node_master, 0);
my @datamax_wal = polar_get_walfile($node_datamax1, 1);
my $master_waldir = $node_master->polar_get_datadir;
$master_waldir = "$master_waldir/pg_wal";
my $datamax_waldir = $node_datamax1->polar_get_datadir;
$datamax_waldir = "$datamax_waldir/polar_datamax/pg_wal";
my $result = polar_walfile_compare(\@master_wal, \@datamax_wal, $master_waldir, $datamax_waldir, $last_segno);
ok($result == 1, "initial datamax start streaming from the smallest lsn of primary\n"); 

### 2. create new datamax, streaming from datamax
my $node_datamax2 = PostgreSQL::Test::Cluster->new('datamax2');
$node_datamax2->polar_init_datamax($primary_system_identifier);
$node_datamax->polar_create_slot($node_datamax2->name);

$node_datamax2->start;
$node_datamax2->safe_psql('postgres','CREATE EXTENSION polar_monitor;');
$node_datamax2->stop;
$node_datamax2->polar_datamax_set_recovery($node_datamax);
$node_datamax2->append_conf('postgresql.conf', "polar_wal_pipeline_enable = false");
$node_datamax2->append_conf('postgresql.conf', "polar_logindex_mem_size = 0");
$node_datamax2->start;
$node_datamax2->wait_walstreaming_establish_timeout($wait_timeout);

# wait for datamax received the wal
$node_datamax->wait_for_catchup($node_datamax2, 'flush', $insert_lsn);
@datamax_wal = polar_get_walfile($node_datamax, 1);
my @datamax_wal2 = polar_get_walfile($node_datamax2, 1);
my $datamax_waldir2 = $node_datamax2->polar_get_datadir;
$datamax_waldir2 = "$datamax_waldir2/polar_datamax/pg_wal";
$result = 0;
$result = polar_walfile_compare(\@datamax_wal, \@datamax_wal2, $datamax_waldir, $datamax_waldir2, $last_segno);
ok($result == 1, "initial datamax2 start streaming from the smallest lsn of datamax\n"); 

### 3. insert data to generate wal, don't remove wal during this time
$node_master->safe_psql('postgres', 'ALTER SYSTEM SET checkpoint_timeout = 600;');
$node_master->reload;
# insert data 
$count = 0;
do
{
	# pg_switch_wal() to generate wal 
	$node_master->safe_psql('postgres', "select pg_switch_wal();");	
	$count = $count + 1;
}while($count < $total_count);
$node_master->safe_psql('postgres',
	'INSERT INTO test_table(val) SELECT coalesce(max(val),0) + 1 AS newval FROM test_table RETURNING val');
$insert_lsn = $node_master->lsn('insert');
$last_segno = $node_master->safe_psql('postgres', "select pg_walfile_name('$insert_lsn');");
print "last_segno: $last_segno\n";

# create two new datamax node
my $node_datamax3 = PostgreSQL::Test::Cluster->new('datamax3');
$node_datamax3->polar_init_datamax($primary_system_identifier);
$node_master->polar_create_slot($node_datamax3->name);

$node_datamax3->start;
$node_datamax3->safe_psql('postgres','CREATE EXTENSION polar_monitor;');
$node_datamax3->stop;
$node_datamax3->polar_datamax_set_recovery($node_master);
$node_datamax3->append_conf('postgresql.conf', "polar_wal_pipeline_enable = false");
$node_datamax3->append_conf('postgresql.conf', "polar_logindex_mem_size = 0");

my $node_datamax4 = PostgreSQL::Test::Cluster->new('datamax4');
$node_datamax4->polar_init_datamax($primary_system_identifier);
$node_master->polar_create_slot($node_datamax4->name);

$node_datamax4->start;
$node_datamax4->safe_psql('postgres','CREATE EXTENSION polar_monitor;');
$node_datamax4->stop;
$node_datamax4->polar_datamax_set_recovery($node_master);
$node_datamax4->append_conf('postgresql.conf', "polar_wal_pipeline_enable = false");
$node_datamax4->append_conf('postgresql.conf', "polar_logindex_mem_size = 0");

# set checkpoint_timeout smaller to make wal removal frequently, and start new datamax
$node_master->safe_psql('postgres', 'ALTER SYSTEM SET checkpoint_timeout = 1;');
$node_master->reload;
$node_datamax3->start;
$node_datamax4->start;
$node_datamax3->wait_walstreaming_establish_timeout($wait_timeout);
$node_datamax4->wait_walstreaming_establish_timeout($wait_timeout);
# wait for datamax received the wal
$node_master->wait_for_catchup($node_datamax3, 'flush', $insert_lsn);
@master_wal = polar_get_walfile($node_master, 0);
@datamax_wal = polar_get_walfile($node_datamax3, 1);
$datamax_waldir = $node_datamax3->polar_get_datadir;
$datamax_waldir = "$datamax_waldir/polar_datamax/pg_wal";
$result = 0;
$result = polar_walfile_compare(\@master_wal, \@datamax_wal, $master_waldir, $datamax_waldir, $last_segno);
ok($result == 1, "initial datamax3 start streaming from the smallest lsn of datamax\n"); 

@datamax_wal = polar_get_walfile($node_datamax4, 1);
$datamax_waldir = $node_datamax4->polar_get_datadir;
$datamax_waldir = "$datamax_waldir/polar_datamax/pg_wal";
$result = 0;
$result = polar_walfile_compare(\@master_wal, \@datamax_wal, $master_waldir, $datamax_waldir, $last_segno);
ok($result == 1, "initial datamax4 start streaming from the smallest lsn of datamax\n"); 

$node_datamax4->stop;
$node_datamax3->stop;
$node_datamax2->stop;
$node_datamax1->stop;
$node_datamax->stop;
$node_master->stop;