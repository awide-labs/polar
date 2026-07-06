# Test for datamax keep walfiles which haven't been removed in upstream node
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use Test::More tests=>2;

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
$node_standby->append_conf('postgresql.conf', "polar_wal_pipeline_enable = false");
$node_standby->append_conf('postgresql.conf', "polar_logindex_mem_size = 0");

$node_master->append_conf('postgresql.conf', "synchronous_commit = on");
$node_master->append_conf('postgresql.conf', "synchronous_standby_names='".$node_datamax->name."'");
$node_master->append_conf('postgresql.conf', "archive_mode = always");
$node_master->append_conf('postgresql.conf', "polar_wal_pipeline_enable = false");
$node_master->append_conf('postgresql.conf', "polar_logindex_mem_size = 0");

$node_master->start;
$node_master->polar_create_slot($node_datamax->name);

$node_datamax->start;
$node_datamax->polar_create_slot($node_standby->name);
$node_datamax->safe_psql('postgres','CREATE EXTENSION polar_monitor;');
$node_datamax->stop;

# set datamax recovery.conf
$node_datamax->polar_datamax_set_recovery($node_master);
$node_datamax->append_conf('postgresql.conf', "polar_datamax_remove_archivedone_wal_timeout = 10000");
$node_datamax->append_conf('postgresql.conf', "polar_wal_pipeline_enable = false");
$node_datamax->append_conf('postgresql.conf', "polar_logindex_mem_size = 0");
$node_datamax->start;
$node_standby->start;
$node_datamax->wait_walstreaming_establish_timeout(20);
$node_standby->wait_walstreaming_establish_timeout(20);

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

# Compare only the valid, already-flushed prefix ($len bytes) of a wal segment
# both nodes hold in common.  Once the primary recycles old wal, the only
# segment master and datamax both retain is the one currently being received:
# its unused tail differs between the nodes (a recycled segment keeps stale
# bytes past the write point, not zeroes) and its head keeps growing as the
# primary emits background wal.  Wal bytes below the flushed lsn are immutable
# and are streamed verbatim, so comparing that prefix is race-free.
sub polar_walfile_prefix_compare
{
	my ($waldir1, $waldir2, $walfile, $len) = @_;
	my $f1 = "$waldir1/$walfile";
	my $f2 = "$waldir2/$walfile";
	return 0 unless (-f $f1 && -f $f2 && $len > 0);
	# cmp -s -n LEN exits 0 iff the first LEN bytes are identical
	return (system("cmp", "-s", "-n", $len, $f1, $f2) == 0) ? 1 : 0;
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
print "insert_lsn: $insert_lsn\n";

# wait for standby replay
$node_datamax->wait_for_catchup($node_standby, 'replay', $insert_lsn, 300, 1);
# checkpoint after having replayed all wal
$node_master->safe_psql('postgres', "checkpoint");
sleep 10;
$node_standby->safe_psql('postgres', "checkpoint");
sleep 10;

my $last_segno = $node_master->safe_psql('postgres', "select pg_walfile_name('$insert_lsn');");
print "last_segno: $last_segno\n";
my @master_wal = polar_get_walfile($node_master, 0);
my @datamax_wal = polar_get_walfile($node_datamax, 1);
my @standby_wal = polar_get_walfile($node_standby, 0);
my $master_waldir = $node_master->polar_get_datadir;
$master_waldir = "$master_waldir/pg_wal";
my $datamax_waldir = $node_datamax->polar_get_datadir;
$datamax_waldir = "$datamax_waldir/polar_datamax/pg_wal";
my $result = polar_walfile_compare(\@master_wal, \@datamax_wal, $master_waldir, $datamax_waldir, $last_segno);
ok($result == 1, "datamax keep the wal files those haven't been removed by master\n"); 

# test delete wal file in master
$node_master->stop;
$node_master->append_conf('postgresql.conf', "archive_mode = off");
$node_master->start;
$node_datamax->safe_psql('postgres', 'ALTER SYSTEM SET polar_datamax_remove_archivedone_wal_timeout = 3000;');
$node_datamax->reload;
$node_datamax->wait_walstreaming_establish_timeout($wait_timeout);
# generate some wal, then let the primary recycle/remove the wal it no longer
# needs (archive_mode is now off and the datamax slot is the only thing pinning
# older segments)
$node_master->safe_psql('postgres',
	'INSERT INTO test_table(val) SELECT coalesce(max(val),0) + 1 AS newval FROM test_table RETURNING val');
$node_master->safe_psql('postgres', "checkpoint");
sleep 30;

# After recycling, master and datamax share only the segment currently being
# received.  Its whole-file contents legitimately differ (recycled tail + the
# primary's ongoing background wal), so compare just the flushed prefix that
# both nodes are guaranteed to hold identically.  Wait for datamax to durably
# store everything up to that flush lsn first.
$insert_lsn = $node_master->lsn('flush');
print "flush_lsn: $insert_lsn\n";
$node_master->wait_for_catchup($node_datamax, 'flush', $insert_lsn, 300, 1);
my ($cmp_walfile, $cmp_offset) = split(/\|/,
	$node_master->safe_psql('postgres',
		"select file_name, file_offset from pg_walfile_name_offset('$insert_lsn')"));
print "compare walfile $cmp_walfile, first $cmp_offset bytes\n";
@master_wal = polar_get_walfile($node_master, 0);
@datamax_wal = polar_get_walfile($node_datamax, 1);
$result = polar_walfile_prefix_compare($master_waldir, $datamax_waldir, $cmp_walfile, $cmp_offset);
ok($result == 1, "master deletes walfile, datamax keep the wal files those haven't been removed by master\n");

$node_standby->stop;
$node_datamax->stop;
$node_master->stop;