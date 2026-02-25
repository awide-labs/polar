# 001_pg_bulkload.pl
#	  Tests for pg_bulkload with primary-replica setup
#
# IDENTIFICATION
#	  external/pg_bulkload/bin/t/001_pg_bulkload.pl

use strict;
use warnings;

use File::Copy;
use File::Path qw(rmtree);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

#############################################
# Test: pg_bulkload with primary-replica setup
#
# This test verifies that pg_bulkload works correctly in a
# primary-replica (streaming replication) setup:
# 1. Start primary - replica setup
# 2. Create a table on primary
# 3. Generate large CSV test data (~200-300MB)
# 4. Load data using pg_bulkload DIRECT mode
# 5. Verify primary sees the data
# 6. Wait for replica to catch up
# 7. Verify replica sees the same data

my $tempdir = PostgreSQL::Test::Utils::tempdir;

# Target data size: ~200MB
# Each row will be ~400 bytes, so we need ~500K rows
my $num_rows = 500000;

# Setup: create primary node
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;

# Configure primary for pg_bulkload testing and streaming replication
$node_primary->append_conf(
	'postgresql.conf', q[
    shared_preload_libraries='pg_bulkload'
    polar_stat_cache_mode=1
    restart_after_crash=off
    logging_collector=off
    wal_level=replica
    max_wal_senders=10
    hot_standby=on
]);

$node_primary->start;

# Create replication slot for replica
$node_primary->polar_create_slot('replica1');

# Setup: create replica1 node
my $node_replica1 = PostgreSQL::Test::Cluster->new('replica1');
$node_replica1->polar_init_replica($node_primary);

# Configure replica1
$node_replica1->append_conf(
	'postgresql.conf', q[
    restart_after_crash=off
    logging_collector=off
    hot_standby=on
    polar_stat_cache_mode=1
]);

$node_replica1->start;

# Get pg_bulkload binary path
my $pg_bulkload = $node_primary->installed_command('pg_bulkload');

#############################################
# Basic checks for pg_bulkload

program_help_ok($pg_bulkload);
program_version_ok($pg_bulkload);
program_options_handling_ok($pg_bulkload);

#############################################
# Create test database and table on primary

$node_primary->safe_psql('postgres', "CREATE DATABASE test_bulkload;");
$node_primary->safe_psql('test_bulkload', "CREATE EXTENSION pg_bulkload;");
$node_primary->safe_psql('test_bulkload', "CREATE TABLE test_table (id SERIAL, data TEXT, padding TEXT);");

# Generate large CSV test data (~200MB)
print "Generating $num_rows rows of test data...\n";
my $data_file = "$tempdir/test_data_large.csv";

# Generate a pattern string for padding (approximately 350 bytes)
my $padding_base = "0123456789" x 35;  # 350 bytes when repeated

open my $fh, '>', $data_file or die "Cannot create test data file: $!";
for my $i (1 .. $num_rows) {
	# Alternate padding to create different rows
	my $padding = $padding_base . ($i % 100);
	# CSV: id, data, padding (matching table columns)
	print $fh "$i,test_data_$i,$padding\n";
}
close $fh;

# Check file size
my $file_size = -s $data_file;
my $file_size_mb = int($file_size / 1024 / 1024);
print "Generated data file: ${file_size_mb}MB ($num_rows rows)\n";

#############################################
# Create pg_bulkload control file with DIRECT mode

my $control_file = "$tempdir/test_table.ctl";

open my $ctl, '>', $control_file or die "Cannot create control file: $!";
print $ctl <<EOF;
TYPE = CSV
INPUT = $data_file
TABLE = test_table
WRITER = DIRECT
EOF
close $ctl;

#############################################
# Run pg_bulkload to load data in DIRECT mode

print "Loading data with pg_bulkload (DIRECT mode)...\n";
$node_primary->command_ok(
	[ $pg_bulkload, '-d', 'test_bulkload', $control_file ],
	'pg_bulkload loads large data successfully in DIRECT mode');

# pg_bulkload bypasses the sequence; advance it to avoid future insert conflicts
$node_primary->safe_psql('test_bulkload',
	"SELECT setval(pg_get_serial_sequence('test_table', 'id'), $num_rows);"
);

#############################################
# Verify data was loaded on primary

my $primary_count = $node_primary->safe_psql('test_bulkload', 'SELECT COUNT(*) FROM test_table');
is($primary_count, $num_rows, "Primary has $num_rows rows after pg_bulkload");

# Verify data content on primary
my $primary_sample = $node_primary->safe_psql('test_bulkload',
	"SELECT id, SUBSTRING(data FROM 1 FOR 20) FROM test_table WHERE id = 1");
like($primary_sample, qr/^1/, 'Primary has correct data for id=1');

# Verify last row
my $primary_last = $node_primary->safe_psql('test_bulkload',
	"SELECT id FROM test_table ORDER BY id DESC LIMIT 1");
is($primary_last, $num_rows, "Primary has correct last row id=$num_rows");

#############################################
# Wait for replica to catch up

print "Waiting for replica to catch up...\n";

my $primary_lsn = $node_primary->lsn('flush');
$node_primary->wait_for_catchup($node_replica1, 'replay', $primary_lsn);
print "Replica caught up!\n";

#############################################
# Verify data on replica

my $replica_count = $node_replica1->safe_psql('test_bulkload', 'SELECT COUNT(*) FROM test_table');
is($replica_count, $num_rows, "Replica has $num_rows rows (caught up via streaming)");

my $replica_sample = $node_replica1->safe_psql('test_bulkload',
	"SELECT id, SUBSTRING(data FROM 1 FOR 20) FROM test_table WHERE id = 1");
like($replica_sample, qr/^1/, 'Replica has correct data for id=1');

# Verify last row on replica
my $replica_last = $node_replica1->safe_psql('test_bulkload',
	"SELECT id FROM test_table ORDER BY id DESC LIMIT 1");
is($replica_last, $num_rows, "Replica has correct last row id=$num_rows");

#############################################
# Verify data integrity - check random samples

print "Verifying data integrity on replica...\n";
my @sample_ids = (100, 10000, 100000, 250000, 400000, 500000);
foreach my $sample_id (@sample_ids) {
	my $primary_val = $node_primary->safe_psql('test_bulkload',
		"SELECT SUBSTRING(data FROM 1 FOR 50) FROM test_table WHERE id = $sample_id");
	my $replica_val = $node_replica1->safe_psql('test_bulkload',
		"SELECT SUBSTRING(data FROM 1 FOR 50) FROM test_table WHERE id = $sample_id");
	ok($primary_val ne '', "Primary has data for id=$sample_id");
	is($replica_val, $primary_val, "Replica data matches primary for id=$sample_id");
}

print "All data verified successfully!\n";

#############################################
# Cleanup

$node_primary->stop;
$node_replica1->stop;

done_testing();
