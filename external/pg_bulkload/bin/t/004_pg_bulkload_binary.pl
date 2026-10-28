# 004_pg_bulkload_binary.pl
#	  Tests the BINARY writer under PolarDB's shared-storage VFS
#
# IDENTIFICATION
#	  external/pg_bulkload/bin/t/004_pg_bulkload_binary.pl
#
# This test locks in the fix for writer_binary.c: the BINARY output writer
# used to call POSIX open/write/fsync/close on a PolarDB VFS handle (a
# virtual fd carrying POLAR_VFS_FD_MASK, not a kernel fd).  Every raw
# write/fsync/close failed with EBADF, but because the failures were
# reported as WARNING the load still printed "N Rows successfully loaded"
# while the .bin/.bin.ctl files stayed empty and the target table got no
# rows - silent data loss.  With shared storage enabled (always the case
# via polar_init_primary), the bug reproduced on every run.
#
# The test drives the full two-step BINARY flow and asserts both the
# physical output files are non-empty and the reloaded table holds the
# expected rows, which the bug would break.

use strict;
use warnings;

use File::Spec;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $tempdir = PostgreSQL::Test::Utils::tempdir;

# Setup: create a primary node with shared-storage VFS enabled
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;

$node_primary->append_conf(
	'postgresql.conf', q[
    shared_preload_libraries='pg_bulkload'
    polar_stat_cache_mode=1
    restart_after_crash=off
    logging_collector=off
]);

$node_primary->start;

my $pg_bulkload = $node_primary->installed_command('pg_bulkload');

# Create test database and extension
$node_primary->safe_psql('postgres', "CREATE DATABASE test_bulkload;");
$node_primary->safe_psql('test_bulkload', "CREATE EXTENSION pg_bulkload;");
$node_primary->safe_psql('test_bulkload',
	q{CREATE TABLE binout1 (
		val1 smallint NOT NULL,
		val2 integer NOT NULL,
		val3 bigint NOT NULL,
		val4 integer NOT NULL,
		val5 bigint NOT NULL,
		val6 real NOT NULL,
		val7 double precision NOT NULL,
		val8 text NOT NULL
	)});

# Write a small, fully-valid CSV (no parse errors) so the expected row
# count is unambiguous.
my $csv = "$tempdir/binout1.csv";
open my $fh, '>', $csv or die "Cannot create $csv: $!";
print $fh join("\n",
	"11,12,13,14,15,1.1,1.2,test1",
	"21,22,23,24,25,2.1,2.2,test2",
	"31,32,33,34,35,3.1,3.2,test3") . "\n";
close $fh;

# Control file for the BINARY output step (mirrors data/binout1.ctl).
my $ctl = "$tempdir/binout1.ctl";
open $fh, '>', $ctl or die "Cannot create $ctl: $!";
print $fh <<EOF;
WRITER = BINARY
PARSE_ERRORS = -1
OUT_COL = SMALLINT
OUT_COL = INTEGER
OUT_COL = BIGINT
OUT_COL = UNSIGNED SMALLINT
OUT_COL = UNSIGNED INTEGER
OUT_COL = FLOAT
OUT_COL = DOUBLE
OUT_COL = CHAR (10)
EOF
close $fh;

my $out_bin = "$tempdir/binout1.bin";
my $out_ctl = "$tempdir/binout1.bin.ctl";
my $logfile = "$tempdir/binout1.log";
my $prsfile = "$tempdir/binout1.prs";

# Step 1: produce the BINARY output files.
$node_primary->command_ok(
	[ $pg_bulkload, '-d', 'test_bulkload', $ctl,
	  '-i', $csv,
	  '-o', 'TYPE=CSV',
	  '-O', $out_bin,
	  '-l', $logfile,
	  '-P', $prsfile ],
	'pg_bulkload writes the BINARY output files');

# The regression: the output files must be non-empty.  With the VFS bug
# they were 0 bytes despite the "successfully loaded" notice.
ok(-e $out_bin && -s $out_bin > 0,
	'binary output file is non-empty');
ok(-e $out_ctl && -s $out_ctl > 0,
	'sample control file is non-empty');

# Step 2: reload the generated control file back into the table.
$node_primary->command_ok(
	[ $pg_bulkload, '-d', 'test_bulkload', $out_ctl ],
	'pg_bulkload reloads the BINARY output into the table');

# The table must hold all three rows.  With the VFS bug it held zero.
my $count = $node_primary->safe_psql('test_bulkload',
	'SELECT COUNT(*) FROM binout1');
is($count, '3', 'table holds all 3 rows after BINARY reload');

my $rows = $node_primary->safe_psql('test_bulkload',
	'SELECT val1, val2, val3, val8 FROM binout1 ORDER BY val1');
is($rows, "11|12|13|test1\n21|22|23|test2\n31|32|33|test3",
	'BINARY reload preserves the row values');

$node_primary->stop;

done_testing();
