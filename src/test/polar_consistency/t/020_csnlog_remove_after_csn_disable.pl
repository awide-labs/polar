# 020_csnlog_remove_after_csn_disable.pl
#	  Test that disabling CSN and restarting cleanly removes leftover
#	  CSNLOG segment files without crashing.
#
#	  Reproducer for two bugs:
#	  1. SIGSEGV in polar_vfs_file_handle_node_type when vfs_unlink
#	     passes a stack-local vfs_vfd with uninitialized file_name
#	     to the VFS hook chain.  Crashes in both localfs and PFS modes.
#	  2. Replica attempts to unlink CSNLOG files from shared storage,
#	     violating the rule that only the primary may write to it.
#
# IDENTIFICATION
#	  src/test/polar_consistency/t/020_csnlog_remove_after_csn_disable.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Step 1: Start primary + replica with CSN enabled
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;

my $node_replica = PostgreSQL::Test::Cluster->new('replica');
$node_replica->polar_init_replica($node_primary);

$node_primary->append_conf('postgresql.conf', "polar_csn_enable = on");
$node_replica->append_conf('postgresql.conf', "polar_csn_enable = on");

$node_primary->start;
$node_primary->polar_create_slot($node_replica->name);
$node_replica->start;

# Step 2: Generate transactions to create CSNLOG segment files
$node_primary->safe_psql('postgres',
	'CREATE TABLE csn_test(id int)');
$node_primary->safe_psql('postgres',
	'INSERT INTO csn_test SELECT generate_series(1,1000)');
$node_primary->wait_for_catchup($node_replica);

my $result = $node_replica->safe_psql('postgres',
	'SELECT count(*) FROM csn_test');
ok($result == 1000, "replica has CSN-committed data");

# Verify CSNLOG files exist on shared storage
my $csnlog_dir = $node_primary->polar_get_datadir . '/pg_csnlog';
my @csnlog_files = glob("$csnlog_dir/*");
ok(scalar @csnlog_files > 0,
	"CSNLOG segment files exist after CSN transactions");

# Step 3: Stop only replica, disable CSN on it, restart.
# Primary stays running — CSNLOG files remain on shared storage.
# The replica's StartupXLOG calls polar_csnlog_remove_all which
# iterates shared pg_csnlog and calls durable_unlink on each file.
# In PFS mode this triggers the VFS hook crash (SIGSEGV in strlen
# due to uninitialized vfdP.file_name in vfs_unlink).
$node_replica->stop;

$node_replica->adjust_conf('postgresql.conf',
	'polar_csn_enable', 'off');

# CSNLOG files are still on shared storage (primary is still running)
@csnlog_files = glob("$csnlog_dir/*");
ok(scalar @csnlog_files > 0,
	"CSNLOG files still on shared storage while primary runs");

$node_replica->start;
ok(1, "replica starts with polar_csn_enable=off (no crash)");

# Replica must NOT remove CSNLOG files from shared storage —
# only the primary owns shared storage writes.
@csnlog_files = glob("$csnlog_dir/*");
ok(scalar @csnlog_files > 0,
	"CSNLOG files on shared storage untouched by replica");

# Sanity check: data is still accessible
$result = $node_replica->safe_psql('postgres',
	'SELECT count(*) FROM csn_test');
ok($result == 1000, "data accessible after CSN disable and restart");

# Step 4: Restart primary with CSN disabled — it cleans up shared CSNLOG
$node_replica->stop;
$node_primary->stop;

$node_primary->adjust_conf('postgresql.conf',
	'polar_csn_enable', 'off');
$node_primary->start;

@csnlog_files = glob("$csnlog_dir/*");
ok(scalar @csnlog_files == 0,
	"CSNLOG files removed from shared storage by primary");

$node_primary->stop;

done_testing();
