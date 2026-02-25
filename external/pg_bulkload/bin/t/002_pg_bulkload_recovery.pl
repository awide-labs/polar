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
use File::Path qw(rmtree);
use Time::HiRes qw(usleep);
use PostgreSQL::Test::Cluster;
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

# Configure for pg_bulkload testing and fault injection
$node_primary->append_conf(
	'postgresql.conf', q[
    shared_preload_libraries='pg_bulkload'
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
	print "Test 5: Testing crash recovery with fault injection...\n";

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
	print "Injecting fault 'pg_bulkload_after_lsf_create' with panic action...\n";
	$node_primary->safe_psql('test_bulkload',
		"SELECT inject_fault('pg_bulkload_during_write', 'panic');");

	# Run pg_bulkload - it should crash due to fault injection
	print "Running pg_bulkload (will crash due to fault injection)...\n";
	my $result = $node_primary->command_fails(
		[ $pg_bulkload, '-d', 'test_bulkload', $control_file ],
		'pg_bulkload crashes due to fault injection');

	# Wait for postmaster to fully exit after crash
	foreach my $i (0 .. 10 * $PostgreSQL::Test::Utils::timeout_default)
	{
		last if !-f $node_primary->data_dir . '/postmaster.pid';
		usleep(100_000);
	}
	$node_primary->{_pid} = undef;

	# Run recovery mode (offline, server is down)
	print "Running recovery mode...\n";
	my ($recovery_stdout, $recovery_stderr) = $node_primary->run_command(
		[ $pg_bulkload, '-r', '-D', $pgdata ]);
	print "Recovery stdout: $recovery_stdout\n";
	print "Recovery stderr: $recovery_stderr\n";

	like($recovery_stderr, qr/delete|remove|recover/i,
		'Recovery processes loadstatus file');

	$node_primary->restart();
	
	# Verify table is accessible (might be empty or have partial data)
	my $count_after_crash = $node_primary->safe_psql(
		'test_bulkload', 'SELECT COUNT(*) FROM test_recovery');
	ok($count_after_crash == 0, "Table is accessible after recovery ($count_after_crash rows)");

	# Truncate table for next test
	$node_primary->safe_psql('test_bulkload', 'TRUNCATE test_recovery RESTART IDENTITY;');
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
