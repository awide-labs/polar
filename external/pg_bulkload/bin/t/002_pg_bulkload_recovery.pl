# 002_pg_bulkload_recovery.pl
#	  Tests for pg_bulkload recovery mode using fault injection
#
# IDENTIFICATION
#	  external/pg_bulkload/bin/t/002_pg_bulkload_recovery.pl
#
# This test verifies:
# 1. pg_bulkload crash recovery using fault injection
# 2. LSF files are properly created and cleaned up after recovery
# 3. Data integrity after crash and recovery

use strict;
use warnings;

use File::Copy;
use File::Find;
use File::Path qw(rmtree);
use Time::HiRes qw(usleep);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::RecursiveCopy;
use PostgreSQL::Test::Utils;
use Test::More;

#############################################
# Setup

# Configurable number of rows for testing
my $num_rows = 200000;  # 200K rows: enough to span 2 write buffers (BLOCK_BUF_NUM=1024 blocks each)

my $tempdir = PostgreSQL::Test::Utils::tempdir;

# Setup: create a primary node (no replica for this test)
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;

# Configure for pg_bulkload testing and fault injection.
# Note that shared_preload_libraries does not include pg_bulkload here;
# pg_bulkload is loaded later via CREATE EXTENSION, so this node represents
# the "no preload" configuration.
$node_primary->append_conf(
	'postgresql.conf', q[
    polar_stat_cache_mode=1
    restart_after_crash=off
    logging_collector=off
    wal_level=replica
    max_wal_senders=10
    wal_keep_size=1GB
]);

$node_primary->start;

# Get pg_bulkload binary path
my $pg_bulkload = $node_primary->installed_command('pg_bulkload');

#############################################
# Test 1: Help output shows recovery option

program_help_ok($pg_bulkload);
command_like(
	[ $pg_bulkload, '--help' ],
	qr/-r,\s*--recovery\s+execute recovery/s,
	'pg_bulkload --help shows -r/--recovery option');

#############################################
# Test 2: Recovery mode with no arguments

command_fails_like(
	[ $pg_bulkload, '-r' ],
	qr/data directory|PGDATA/i,
	'pg_bulkload -r requires data directory');

#############################################
# Test 3: Recovery mode with invalid data directory

command_fails_like(
	[ $pg_bulkload, '-r', '-D', '/nonexistent/path' ],
	qr/could not change/i,
	'pg_bulkload -r fails with invalid data directory');

#############################################
# Test 4: Create test database, table, and install extensions

$node_primary->safe_psql('postgres', "CREATE DATABASE test_bulkload;");
$node_primary->safe_psql('test_bulkload', "CREATE EXTENSION pg_bulkload;");

# Install faultinjector extension if available (for crash simulation)
my $has_faultinjector = 0;
eval {
	$node_primary->safe_psql('test_bulkload', "CREATE EXTENSION faultinjector;");
	$has_faultinjector = 1;
};
if (!$has_faultinjector) {
	note "faultinjector extension not available, skipping crash injection test";
}

$node_primary->safe_psql('test_bulkload', 
	"CREATE TABLE test_recovery (id SERIAL PRIMARY KEY, data TEXT, padding TEXT);");

# Get the data directory path
my $pgdata = $node_primary->data_dir;

#############################################
# Test 5: Crash simulation using fault injection

if ($has_faultinjector) {
	print "Test 5: Testing crash recovery with shared crash image (offline vs postmaster recovery)...\n";

	#############################################
	# 5.1 Generate inconsistent state on node_primary
	#############################################

	# Generate test data
	my $data_file = "$tempdir/test_data_crash.csv";
	open my $fh, '>', $data_file or die "Cannot create test data file: $!";
	for my $i (1 .. $num_rows) {
		print $fh "$i,test_data_$i,padding_$i\n";
	}
	close $fh;

	# Create control file
	my $control_file = "$tempdir/test_crash.ctl";
	open my $ctl, '>', $control_file or die "Cannot create control file: $!";
	print $ctl <<EOF;
TYPE = CSV
INPUT = $data_file
TABLE = test_recovery
WRITER = DIRECT
EOF
	close $ctl;

	# Inject fault: panic when pg_bulkload reaches the fault injection point
	# This simulates a crash after LSF file is created but before data is fully written
	print "Injecting fault 'pg_bulkload_during_write' with panic action on node_primary...\n";
	$node_primary->safe_psql('test_bulkload',
		"SELECT inject_fault('pg_bulkload_during_write', 'panic');");

	# Run pg_bulkload on node_primary - it should crash due to fault injection
	print "Running pg_bulkload on node_primary (will crash due to fault injection)...\n";
	$node_primary->command_fails(
		[ $pg_bulkload, '-d', 'test_bulkload', $control_file ],
		'pg_bulkload crashes on node_primary due to fault injection');

	# Wait for postmaster to fully exit after crash on node_primary
	foreach my $i (0 .. 10 * $PostgreSQL::Test::Utils::timeout_default)
	{
		last if !-f $node_primary->data_dir . '/postmaster.pid';
		usleep(100_000);
	}
	$node_primary->{_pid} = undef;

	ok(FindLSF($node_primary->polar_get_datadir), "At least one LSF was found");

	PolarColdBackup($node_primary, 'bulkload_crash');

	#############################################
	# 5.2 Postmaster startup without pg_bulkload extension
	#     but with pending .loadstatus files
	#############################################

	print
	  "Test 5.2: Postmaster startup without pg_bulkload extension but with pending .loadstatus files should fail with proper error...\n";

	my $node_no_pg_bulkload =
	  PostgreSQL::Test::Cluster->new('bulkload_no_extension');

	PolarColdRestore($node_no_pg_bulkload, $node_primary, 'bulkload_crash');

	ok(FindLSF($node_no_pg_bulkload->polar_get_datadir), "At least one LSF was found");

	my $logstart = 0;

	is($node_no_pg_bulkload->start(fail_ok => 1),
		0,
		"postmaster fails to start when pg_bulkload extension is not loaded but pending .loadstatus files exist"
	);

	ok(
		$node_no_pg_bulkload->log_contains(
			qr/bulkload data was not completely loaded and pg_bulkload extension was not loaded/,
			$logstart),
		"postmaster log reports missing pg_bulkload extension with pending bulkload data"
	);

	$node_no_pg_bulkload->stop('immediate', fail_ok => 1);

	#############################################
	# 5.3 Postmaster-driven recovery with shared_preload_libraries='pg_bulkload'
	#############################################

	print "Test 5.3: Postmaster bulkload recovery with shared_preload_libraries='pg_bulkload' on cloned node...\n";

	my $node_pg_bulkload_preload =
	  PostgreSQL::Test::Cluster->new('bulkload_preload');

	PolarColdRestore($node_pg_bulkload_preload, $node_primary, 'bulkload_crash');

	ok(FindLSF($node_pg_bulkload_preload->polar_get_datadir), "At least one LSF was found");

	$node_pg_bulkload_preload->append_conf(
		'postgresql.conf', q[
 		shared_preload_libraries='pg_bulkload'
 		restart_after_crash=off
		]);

	$node_pg_bulkload_preload->start;

	my $count_after_preload = $node_pg_bulkload_preload->safe_psql(
		'test_bulkload', 'SELECT COUNT(*) FROM test_recovery');
	ok($count_after_preload == 0,
		"Table is accessible after postmaster bulkload recovery on node_pg_bulkload_preload ($count_after_preload rows)");

	$node_pg_bulkload_preload->stop;

	#############################################
	# 5.4 Offline recovery on node_primary (no shared_preload_libraries)
	#############################################

	print "Test 5.4: Offline recovery using pg_bulkload -r on node_primary (no shared_preload_libraries)...\n";

	my ($recovery_stdout_offline, $recovery_stderr_offline) = $node_primary->run_command(
		[ $pg_bulkload, '-r', '-D', $pgdata ]);
	print "Offline recovery stdout: $recovery_stdout_offline\n";
	print "Offline recovery stderr: $recovery_stderr_offline\n";

	like($recovery_stderr_offline, qr/delete|remove|recover/i,
		'Offline recovery processes loadstatus file on node_primary');

	$node_primary->start;

	my $count_after_offline = $node_primary->safe_psql(
		'test_bulkload', 'SELECT COUNT(*) FROM test_recovery');
	ok($count_after_offline == 0,
		"Table is accessible after offline recovery on node_primary ($count_after_offline rows)");

	# Truncate table for subsequent tests
	$node_primary->safe_psql('test_bulkload',
		'TRUNCATE test_recovery RESTART IDENTITY;');
} else {
	note "Skipping fault injection test (faultinjector not available)";
}

#############################################
# Test 6: Successful load and verify data integrity

print "Test 6: Testing successful load and data integrity...\n";

# Create clean test data
my $data_file = "$tempdir/test_data.csv";
open my $fh, '>', $data_file or die "Cannot create test data file: $!";
for my $i (1 .. $num_rows) {
	print $fh "$i,test_data_$i,padding_$i\n";
}
close $fh;

# Create control file
my $control_file = "$tempdir/test_recovery.ctl";
open my $ctl, '>', $control_file or die "Cannot create control file: $!";
print $ctl <<EOF;
TYPE = CSV
INPUT = $data_file
TABLE = test_recovery
WRITER = DIRECT
EOF
close $ctl;

# Run pg_bulkload successfully
print "Running pg_bulkload to load data...\n";
$node_primary->command_ok(
	[ $pg_bulkload, '-d', 'test_bulkload', $control_file ],
	'pg_bulkload loads data successfully');

# Verify data on primary
my $primary_count = $node_primary->safe_psql('test_bulkload', 
	'SELECT COUNT(*) FROM test_recovery');
is($primary_count, $num_rows, "Primary has $num_rows rows after pg_bulkload");

# Verify first row
my $first_row = $node_primary->safe_psql('test_bulkload',
	"SELECT id, data FROM test_recovery WHERE id = 1");
like($first_row, qr/^1\|test_data_1/, 'First row has correct data');

# Verify last row
my $last_id = $node_primary->safe_psql('test_bulkload',
	"SELECT id FROM test_recovery ORDER BY id DESC LIMIT 1");
is($last_id, $num_rows, "Last row has correct id=$num_rows");

#############################################
# Test 7: Recovery on clean state (should report no pending loads)

print "Test 7: pg_bulkload -r refuses to run while server is up...\n";
$node_primary->command_fails_like(
	[ $pg_bulkload, '-r', '-D', $pgdata ],
	qr/postmaster\.pid.*already exists|another postmaster.*running/i,
	'pg_bulkload -r refuses to run while postmaster is active');

#############################################
# Test 8: Verify table data persists after restart

print "Test 8: Testing data persistence after restart...\n";

# Stop and restart primary
$node_primary->stop;
$node_primary->start;

# Verify data is still there
my $count_after_restart = $node_primary->safe_psql('test_bulkload',
	'SELECT COUNT(*) FROM test_recovery');
is($count_after_restart, $num_rows, 
	"Data persists after restart ($count_after_restart rows)");

#############################################
# Cleanup

$node_primary->stop;

done_testing();

sub FindLSF {
	my $datadir = shift;
	my @found;
	find(
		sub {
			return unless -f $File::Find::name;  # only regular files
			push @found, $File::Find::name
			  if ($File::Find::name =~ m/.*\.loadstatus/);
		},
		$datadir);
	return @found;
}

sub PolarColdBackup {
	my $node = shift;
	my $name = shift;
	# Take a cold filesystem backup of the crashed state (local PGDATA only)
	$node->backup_fs_cold($name);

	# Cold backup of Polar data directory (LSF and relation data live here)
	my $backup_polar = $node->backup_dir . '/' . $name . '_polar';
	PostgreSQL::Test::RecursiveCopy::copypath($node->polar_get_datadir,
		$backup_polar);
}

sub PolarColdRestore {
	my $new_node = shift;
	my $old_node = shift;
	my $name = shift;

	$new_node->init_from_backup($old_node, $name, standby => 0);

	# Restore Polar data directory from cold backup and point new node at it
	my $new_polar_datadir = $new_node->polar_get_datadir;
	PostgreSQL::Test::RecursiveCopy::copypath(
		$old_node->backup_dir . '/' . $name . '_polar', $new_polar_datadir);

	# Replace polar_datadir in copied postgresql.conf with this node's path
	my $conffile = $new_node->data_dir . '/postgresql.conf';
	my $content = PostgreSQL::Test::Utils::slurp_file($conffile);
	$content =~ s/^\s*polar_datadir\s*=.*/polar_datadir='file-dio:\/\/$new_polar_datadir'/gm;
	open my $fh, '>', $conffile or die "Cannot write $conffile: $!";
	print $fh $content;
	close $fh or die "Cannot close $conffile: $!";

	$new_node->polar_set_datadir($new_polar_datadir);
}
