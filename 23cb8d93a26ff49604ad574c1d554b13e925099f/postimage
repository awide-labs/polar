# Test for initial datamax start streaming from the smallest lsn of primary
use strict;
use warnings;
use FindBin;
use lib $FindBin::RealBin;
use PolarDatamaxUtils;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More tests=>6;

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

# True if a plain connection to the node still works (its postmaster is up).
# With restart_after_crash=off (set in the TAP default conf) a walreceiver
# SIGSEGV takes the whole node down, so this going false is the UAF signal.
sub polar_node_reachable
{
	my ($node) = @_;
	my ($rc) = $node->psql('postgres', 'SELECT 1', on_error_die => 0, timeout => 15);
	return (defined($rc) && $rc == 0);
}

# Secondary signal: the node's server log shows a walreceiver crash signature.
# Scans both the -l logfile and the logging_collector files under data/log.
sub polar_datamax_walrcv_crashed
{
	my ($node) = @_;
	my $log = slurp_file($node->logfile);
	my $logdir = $node->data_dir . '/log';
	$log .= slurp_file($_) foreach (sort glob("$logdir/*.log"));
	return scalar($log =~ /
		  was\ terminated\ by\ signal\ 11
		| Segmentation\ fault
		| coredump\ signal\(11\)
		| polar_datamax_update_cur_valid_lsn
		/x);
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
# the master can write more wal at any time, so compare the wal up to the
# lsn it has flushed now, once datamax1 has flushed it too
my $segsize = polar_wal_segment_size($node_master);
my $cmp_lsn = $node_master->lsn('flush');
print "cmp_lsn: $cmp_lsn\n";
$node_master->wait_for_catchup($node_datamax1, 'flush', $cmp_lsn);
polar_get_walfile($node_master, 0);
polar_get_walfile($node_datamax1, 1);
my $master_waldir = polar_waldir($node_master, 0);
my $datamax_waldir = polar_waldir($node_datamax1, 1);
my $result = polar_walfile_compare($master_waldir, $datamax_waldir, $cmp_lsn, $segsize);
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

# datamax keeps receiving wal from the master, so compare the wal up to the
# lsn the master has flushed now, once datamax and datamax2 have flushed it
$cmp_lsn = $node_master->lsn('flush');
print "cmp_lsn: $cmp_lsn\n";
$node_master->wait_for_catchup($node_datamax, 'flush', $cmp_lsn);
$node_datamax->wait_for_catchup($node_datamax2, 'flush', $cmp_lsn);
polar_get_walfile($node_datamax, 1);
polar_get_walfile($node_datamax2, 1);
$datamax_waldir = polar_waldir($node_datamax, 1);
my $datamax_waldir2 = polar_waldir($node_datamax2, 1);
$result = 0;
$result = polar_walfile_compare($datamax_waldir, $datamax_waldir2, $cmp_lsn, $segsize);
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

# Make WAL removal happen so the new datamax nodes must stream from the smallest
# reserved lsn of the primary. TP15's PolarDB build allowed checkpoint_timeout
# down to 1s and relied on the background checkpointer; PG17 keeps upstream's 30s
# floor, so use the minimum valid timeout and force removal explicitly with a
# checkpoint (deterministic, and independent of the timer).
$node_master->safe_psql('postgres', 'ALTER SYSTEM SET checkpoint_timeout = 30;');
$node_master->reload;
$node_master->safe_psql('postgres', 'CHECKPOINT;');
$node_datamax3->start;
$node_datamax4->start;
$node_datamax3->wait_walstreaming_establish_timeout($wait_timeout);
$node_datamax4->wait_walstreaming_establish_timeout($wait_timeout);
# the master can write more wal at any time, so compare the wal up to the
# lsn it has flushed now, once datamax3 and datamax4 have flushed it too
$cmp_lsn = $node_master->lsn('flush');
print "cmp_lsn: $cmp_lsn\n";
$node_master->wait_for_catchup($node_datamax3, 'flush', $cmp_lsn);
$node_master->wait_for_catchup($node_datamax4, 'flush', $cmp_lsn);
polar_get_walfile($node_master, 0);
polar_get_walfile($node_datamax3, 1);
$datamax_waldir = polar_waldir($node_datamax3, 1);
$result = 0;
$result = polar_walfile_compare($master_waldir, $datamax_waldir, $cmp_lsn, $segsize);
ok($result == 1, "initial datamax3 start streaming from the smallest lsn of datamax\n"); 

polar_get_walfile($node_datamax4, 1);
$datamax_waldir = polar_waldir($node_datamax4, 1);
$result = 0;
$result = polar_walfile_compare($master_waldir, $datamax_waldir, $cmp_lsn, $segsize);
ok($result == 1, "initial datamax4 start streaming from the smallest lsn of datamax\n");

### 5. Regression: walreceiver flush-on-exit must not touch a freed valid-LSN list
#
# A datamax walreceiver that exits while holding unflushed WAL
# (LogstreamResult.Flush < LogstreamResult.Write) runs XLogWalRcvFlush() from
# WalRcvDie() (on_shmem_exit). Before the fix the valid-LSN list had already
# been pfree()d by an *earlier* before_shmem_exit callback, so
# polar_datamax_update_cur_valid_lsn() dereferenced freed memory -> SIGSEGV
# under --enable-cassert (silent corruption in production builds). In the
# field the trigger is the master terminating the datamax walsender to release
# its slot under WAL pressure; here we reach the identical walreceiver exit
# path (and the identical client FATAL) by terminating the walsender while the
# datamax is mid-drain -- see the mechanism comment on the loop below.

my $node_uaf = PostgreSQL::Test::Cluster->new('datamax_uaf');
$node_uaf->polar_init_datamax($primary_system_identifier);
$node_master->polar_create_slot($node_uaf->name);
$node_uaf->start;
$node_uaf->safe_psql('postgres', 'CREATE EXTENSION polar_monitor;');
$node_uaf->stop;
$node_uaf->polar_datamax_set_recovery($node_master);
$node_uaf->append_conf('postgresql.conf', "polar_wal_pipeline_enable = false");
$node_uaf->append_conf('postgresql.conf', "polar_logindex_mem_size = 0");
# Reconnect the walreceiver quickly after each forced termination so the loop
# below paces in ~100ms rather than the 5s default.
$node_uaf->append_conf('postgresql.conf', "wal_retrieve_retry_interval = 100ms");
$node_uaf->start;
$node_uaf->wait_walstreaming_establish_timeout($wait_timeout);

# Make sure streaming is really established (startpointTLI valid -> the
# flush-on-exit in WalRcvDie() runs) before we start terminating.
$node_master->safe_psql('postgres',
	'INSERT INTO test_table(val) SELECT coalesce(max(val),0)+1 FROM test_table');
$node_master->wait_for_catchup($node_uaf, 'flush', $node_master->lsn('insert'));

# The crash needs the connection error to surface *inside* the walreceiver's
# inner drain loop (walreceiver.c: the walrcv_receive() at the bottom of the
# for(;;) that follows the top-level receive), where LogstreamResult.Write has
# advanced past .Flush -- the per-batch flush only runs after that loop breaks.
# If the datamax is caught up/idle when the walsender goes away, the error
# instead surfaces at the top-level receive with Flush == Write and the
# flush-on-exit is a harmless no-op.
#
# On this (tmpfs) setup the datamax drains streamed WAL faster than we can
# poll, so it is never caught mid-drain while merely streaming. To open a long,
# reliable mid-drain window we stop the datamax, pile up a large backlog on the
# master while it is down, then restart it: on restart it must drain that whole
# backlog in one long streaming pass. While it drains we repeatedly terminate
# its walsender; a termination that lands mid-drain makes the walreceiver exit
# from inside the inner loop with Flush < Write, exercising WalRcvDie()'s
# flush-on-exit against the valid-LSN list. pg_terminate_backend does not
# invalidate the slot, so this is non-destructive and retryable. A UAF hit
# SIGSEGVs the walreceiver which, with restart_after_crash=off, takes the whole
# datamax node down for good -- so "node no longer reachable" is the signal.
my $slot = $node_uaf->name;
my $crashed = 0;
my $kills = 0;
my $maxgap = 0;
for (my $round = 0; $round < 2 && !$crashed; $round++)
{
	$node_uaf->stop;
	# Accumulate a multi-hundred-MB backlog while the datamax is disconnected;
	# the slot retains this WAL for it to drain on restart.
	$node_master->safe_psql('postgres',
		'INSERT INTO test_table(val) SELECT g FROM generate_series(1, 20000000) g');
	$node_uaf->start;

	# Hammer the walsender while the datamax drains the backlog.
	for (my $k = 0; $k < 50 && !$crashed; $k++)
	{
		my $st = $node_master->safe_psql('postgres',
			"SELECT coalesce(s.active_pid::text,''), "
		  . "coalesce((r.sent_lsn - r.flush_lsn)::text,'0') "
		  . "FROM pg_replication_slots s "
		  . "LEFT JOIN pg_stat_replication r ON r.application_name = s.slot_name "
		  . "WHERE s.slot_name = '$slot'");
		my ($ap, $gap) = split(/\|/, $st, 2);
		$maxgap = $gap if defined($gap) && $gap =~ /^\d+$/ && $gap > $maxgap;
		if (defined($ap) && $ap ne '')
		{
			$node_master->safe_psql('postgres', "SELECT pg_terminate_backend($ap)");
			$kills++;
			note("round $round k $k: terminated $ap (sent-flush=$gap)") if $k < 8;
		}
		# A real UAF crash takes the node down for good (restart_after_crash=off);
		# confirm a suspected death to avoid a false positive from one slow reply.
		if (!polar_node_reachable($node_uaf))
		{
			select(undef, undef, undef, 1.0);
			$crashed = 1 unless polar_node_reachable($node_uaf);
		}
		select(undef, undef, undef, 0.05) unless $crashed;	# 50ms nap
	}
}
note("total walsender terminations: $kills; max sent-flush gap: $maxgap; crashed=$crashed");
$crashed ||= !polar_node_reachable($node_uaf);
$crashed ||= polar_datamax_walrcv_crashed($node_uaf);

ok(!$crashed,
	'datamax walreceiver flush-on-exit does not dereference a freed valid-LSN list');
ok(polar_node_reachable($node_uaf),
	'datamax node survives repeated mid-drain walreceiver termination');

$node_uaf->stop;

$node_datamax4->stop;
$node_datamax3->stop;
$node_datamax2->stop;
$node_datamax1->stop;
$node_datamax->stop;
$node_master->stop;