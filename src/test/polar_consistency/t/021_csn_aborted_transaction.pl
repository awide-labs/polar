# 021_csn_aborted_transaction.pl
#	  Test that aborted transactions are not treated internally as
#	  indefinitely running.
#
#	  Reproducer for a bug:
#	    A transaction that aborts after enqueuing NOTIFY will block
#	    the queue forever.
#
# IDENTIFICATION
#	  src/test/polar_consistency/t/021_csn_aborted_transaction.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_fault_injector} eq 'no')
{
	plan skip_all => 'Fault injector not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION IF NOT EXISTS faultinjector');

# Start a background psql session that will LISTEN on channel c1.
# Async notifications it receives will be captured by the next query() call.
my $bg = $node->background_psql('postgres', on_error_stop => 0);
$bg->query_safe("LISTEN c1");

# faulty_notify: inject a fault that errors after the NOTIFY is enqueued,
# so the transaction aborts with the enqueued notify still pending.
my ($ret, $stdout, $stderr) = $node->psql(
	'postgres',
	q[
		BEGIN;
		SELECT inject_fault_infinite('polar_notify_after_enqueue', 'error');
		NOTIFY c1;
		COMMIT;
	],
	on_error_die => 0);
ok($ret != 0, 'faulty_notify fails');
like($stderr, qr/fault triggered.*polar_notify_after_enqueue/,
	'error matches expected');

# Reset the fault so subsequent NOTIFYs work.
$node->safe_psql('postgres',
	"SELECT inject_fault('polar_notify_after_enqueue', 'reset');");
ok(1, 'unset_fault succeeded');

# Send a clean NOTIFY -- this would hang forever if the aborted
# transaction had blocked the queue.
($ret, $stdout, $stderr) = $node->psql('postgres',
	"NOTIFY c1;", on_error_die => 0);
ok($ret == 0, 'notify1 succeeds after reset');

# Verify the listener actually received the notification.
# The async notification text from psql is captured by the next query()
# call alongside the query result.  We check for the channel name to
# confirm delivery.
my $out = $bg->query_safe('SELECT 1');
ok($out =~ qr/Asynchronous notification "c1"/,
	'notification delivered after abort');

$bg->quit;
$node->stop;
done_testing();
