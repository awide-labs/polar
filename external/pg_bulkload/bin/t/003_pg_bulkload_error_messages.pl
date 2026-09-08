# 003_pg_bulkload_error_messages.pl
#	  Tests that the pg_bulkload CLI always prints detailed error messages
#
# IDENTIFICATION
#	  external/pg_bulkload/bin/t/003_pg_bulkload_error_messages.pl
#
# This test locks in the fix for the bare "ERROR:" symptom: client-side
# ereport() call sites must route their message text into pgut's own
# error buffer (via the errcode/errmsg/errdetail macros in pgut.h)
# instead of silently binding at link time to the polar_vfs_fe stubs
# exported by libpolarvfs, which swallow the text.  Every failure mode
# below must print "ERROR: <detailed message>", never a bare "ERROR:".

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

#############################################
# Setup

my $tempdir = PostgreSQL::Test::Utils::tempdir;

# Setup: create a primary node
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;

# Configure primary for pg_bulkload testing
$node_primary->append_conf(
	'postgresql.conf', q[
    shared_preload_libraries='pg_bulkload'
    polar_stat_cache_mode=1
    restart_after_crash=off
    logging_collector=off
]);

$node_primary->start;

# Get pg_bulkload binary path
my $pg_bulkload = $node_primary->installed_command('pg_bulkload');

#############################################
# Test 1: control file line without '=' reports the offending line

my $ctl_no_equals = "$tempdir/no_equals.ctl";
open my $fh, '>', $ctl_no_equals or die "Cannot create control file: $!";
print $fh "THIS LINE HAS NO EQUALS SIGN\n";
close $fh;

$node_primary->command_fails_like(
	[ $pg_bulkload, '-d', 'postgres', $ctl_no_equals ],
	qr/ERROR: invalid input "THIS LINE HAS NO EQUALS SIGN"/,
	'control file line without = prints a detailed message');

#############################################
# Test 2: over-long control file line is reported by name

my $ctl_long_line = "$tempdir/long_line.ctl";
open $fh, '>', $ctl_long_line or die "Cannot create control file: $!";
print $fh ('x' x 2000), "\n";
close $fh;

$node_primary->command_fails_like(
	[ $pg_bulkload, '-d', 'postgres', $ctl_long_line ],
	qr/ERROR: too long line/,
	'over-long control file line prints a detailed message');

#############################################
# Test 3: unterminated quoted field is reported

my $ctl_unterminated = "$tempdir/unterminated.ctl";
open $fh, '>', $ctl_unterminated or die "Cannot create control file: $!";
print $fh "INPUT = \"unterminated\n";
close $fh;

$node_primary->command_fails_like(
	[ $pg_bulkload, '-d', 'postgres', $ctl_unterminated ],
	qr/ERROR: unterminated quoted field/,
	'unterminated quoted field prints a detailed message');

#############################################
# Test 4: too many positional arguments is reported

$node_primary->command_fails_like(
	[ $pg_bulkload, "$tempdir/a.ctl", "$tempdir/b.ctl" ],
	qr/ERROR: too many arguments/,
	'too many arguments prints a detailed message');

#############################################
# Test 5: missing control file and options is reported
# (same command shape as the original bug report)

$node_primary->command_fails_like(
	[ $pg_bulkload, '-E', 'INFO' ],
	qr/ERROR: requires control file or command line options/,
	'missing control file prints a detailed message');

#############################################
# Test 6: stderr never contains a bare "ERROR:" line

my ($stdout, $stderr) = $node_primary->run_command(
	[ $pg_bulkload, '-d', 'postgres', $ctl_no_equals ]);
like($stderr, qr/ERROR: invalid input/,
	'stderr carries the error text');
unlike($stderr, qr/^ERROR:\s*$/m,
	'stderr does not contain a bare ERROR: line');

#############################################
# Test 7: server-side (libpq) errors keep their detail as well

$node_primary->safe_psql('postgres', "CREATE DATABASE test_bulkload;");
$node_primary->safe_psql('test_bulkload', "CREATE EXTENSION pg_bulkload;");

my $data_file = "$tempdir/test_data.csv";
open $fh, '>', $data_file or die "Cannot create test data file: $!";
print $fh "1,test_data_1\n";
close $fh;

my $ctl_no_table = "$tempdir/no_table.ctl";
open $fh, '>', $ctl_no_table or die "Cannot create control file: $!";
print $fh <<EOF;
TYPE = CSV
INPUT = $data_file
TABLE = no_such_table
EOF
close $fh;

$node_primary->command_fails_like(
	[ $pg_bulkload, '-d', 'test_bulkload', $ctl_no_table ],
	qr/ERROR: query failed: ERROR:.*no_such_table/s,
	'server-side error prints a detailed message');

#############################################
# Cleanup

$node_primary->stop;

done_testing();
