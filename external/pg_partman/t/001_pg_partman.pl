# Run the pgTAP suites in test/ against a PostgreSQL::Test::Cluster instance.
#
# The suites produce their own TAP output, so there is no expected/ output
# for pg_regress to compare.  Both `make check` and `make installcheck` run
# the same script; only the PostgreSQL binaries used to start the cluster
# differ.

use strict;
use warnings FATAL => 'all';

use File::Basename qw(basename dirname);

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;

use Test::More;

my $srcdir = dirname(__FILE__) . '/..';
my $testdir = "$srcdir/test";
my $dbname = 'partman_db';

my @tests = sort glob("$testdir/*.sql");

die "no tests found in $testdir" unless @tests;

my $node = PostgreSQL::Test::Cluster->new('partman');
$node->init;

# The subpartitioning suites create hundreds of tables in a single
# transaction.  The upstream test suite recommends 128.
$node->append_conf('postgresql.conf', 'max_locks_per_transaction = 128');
$node->start;

$node->safe_psql('postgres', "CREATE DATABASE $dbname");
$node->safe_psql(
	$dbname,
	'CREATE SCHEMA partman;
	 CREATE EXTENSION pg_partman WITH SCHEMA partman;
	 CREATE EXTENSION pgtap WITH SCHEMA public;'
);

for my $test (@tests)
{
	my $name = basename($test);

	# Remove leftovers from a previously failed suite so the next one can
	# start from a clean slate.
	$node->safe_psql($dbname, 'DROP ROLE IF EXISTS partman_basic');

	my ($ret, $stdout, $stderr) =
	  $node->psql($dbname, slurp_file($test), on_error_stop => 1);

	ok($ret == 0, $name) or diag("psql exited with $ret\n$stderr");
	unlike($stdout, qr/^not ok |^# Looks like /m, $name)
	  or diag($stdout);
}

$node->stop;

done_testing();
