# 016_outdate_unbuffered_page_during_parse.pl
#	  A record parsed while its page is NOT in the replica's buffer pool
#	  must not be lost when a backend faults the page in concurrently.
#
# The parse path only inserts the logindex entry when the buffer-table
# lookup misses -- there is no descriptor to mark OUTDATE on or to carry
# the polar_outdate_lsn watermark. If a backend reads the page from shared
# storage while the startup is still between that lookup and the
# lastReplayedEndRecPtr update, the read-in replay is bounded by the stale
# frontier, misses the record, and the buffer finishes valid with OUTDATE
# clear: the page is served stale until some later pass replays it again.
# This test keeps that from happening (background replay off, no further
# update to the row) so the missed record stays observable.
#
# The fix under test: the startup publishes the single-page record it is
# currently parsing (its page tags + EndRecPtr) before the parse begins, and a
# backend that faults the page (or its derived VM page) in concurrently arms
# the polar_outdate_lsn watermark on itself (polar_in_flight_buffer_arm), so
# the existing watermark logic re-applies the record once
# lastReplayedEndRecPtr advances past it. Without it the final assertion
# fails with the stale pre-update value.
#
# Choreography mirrors 015 with one essential difference: the replica must
# never touch the page before the race, so the buffered-page arm paths
# cannot fire; the read during the suspended window is the first access
# and faults the page in.
#
# IDENTIFICATION
#	  src/test/polar_consistency/t/016_outdate_unbuffered_page_during_parse.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_fault_injector} eq 'no')
{
	plan skip_all => 'Fault injector not supported by this build';
}

my $db = 'postgres';

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;
$node_primary->append_conf('postgresql.conf', 'synchronous_commit=on');
# keep background WAL out of the way of the occurrence-counted fault
$node_primary->append_conf('postgresql.conf', 'autovacuum=off');

my $node_replica = PostgreSQL::Test::Cluster->new('replica1');
$node_replica->polar_init_replica($node_primary);

$node_primary->start;
$node_primary->polar_create_slot($node_replica->name);

$node_primary->safe_psql($db, 'create extension faultinjector;');
$node_primary->safe_psql($db,
	'create table test(id int primary key, val text);');
$node_primary->safe_psql($db, "insert into test values(1, 'old');");
$node_primary->safe_psql($db, 'checkpoint;');

$node_replica->start;

my $lsn = $node_primary->safe_psql($db, 'select pg_current_wal_insert_lsn();');
$node_replica->poll_query_until($db,
	"select pg_last_wal_replay_lsn() >= '$lsn'::pg_lsn")
  or die 'replica did not catch up after initial load';

# Deliberately no replica read here: the page must stay out of the
# replica's buffer pool so the update's parse takes the buf_id < 0 branch.
my $result =
  $node_primary->safe_psql($db, 'select val from test where id = 1;');
is($result, 'old', 'primary sees initial row');

# Keep background logindex replay away from the page: once it is faulted
# in, bg replay would apply the missed record and mask a lost flag.
$result = $node_replica->safe_psql($db,
	"select inject_fault('polar_logindex_bg_skip_replay', 'enable', '', '', 1, -1, -1);"
);
print "enable polar_logindex_bg_skip_replay: $result\n";

# Prove that the backend fault-in arms the watermark for the in-flight
# record rather than passing through the buffered-page path of test 015.
$result = $node_replica->safe_psql($db,
	"select inject_fault('polar_in_flight_buffer_arm', 'enable', '', '', 1, -1, -1);"
);
print "enable polar_in_flight_buffer_arm: $result\n";

# Suspend the startup after the parse (logindex entry inserted, buffer
# table lookup missed) and before it publishes lastReplayedEndRecPtr.
$result = $node_replica->safe_psql($db,
	"select inject_fault('polar_stall_replay_frontier_publish', 'suspend', '', '', 1, 1, 0);"
);
print "suspend polar_stall_replay_frontier_publish: $result\n";

$node_primary->safe_psql($db, "update test set val = 'new' where id = 1;");

$result = $node_replica->safe_psql($db,
	"select wait_until_triggered_fault('polar_stall_replay_frontier_publish', 1);"
);
print "startup suspended in the parse/publish window: $result\n";

# The critical read: faults the page in from shared storage. The read-in
# replay is bounded by the stale frontier and cannot include the suspended
# record; the in-flight probe arms the watermark so OUTDATE is re-armed
# after the bounded replay. The value is correctly 'old' here either way.
$result = $node_replica->safe_psql($db, 'select val from test where id = 1;');
is($result, 'old',
	'fault-in read during the window sees the pre-update value');

$result = $node_replica->safe_psql($db,
	"select wait_until_triggered_fault('polar_in_flight_buffer_arm', 1);"
);
print "confirmed in-flight arm fired on the fault-in: $result\n";

$result = $node_replica->safe_psql($db,
	"select inject_fault('polar_stall_replay_frontier_publish', 'resume');");
print "resume: $result\n";
$result = $node_replica->safe_psql($db,
	"select inject_fault('polar_stall_replay_frontier_publish', 'reset');");
print "reset: $result\n";

$lsn = $node_primary->safe_psql($db, 'select pg_current_wal_insert_lsn();');
$node_replica->poll_query_until($db,
	"select pg_last_wal_replay_lsn() >= '$lsn'::pg_lsn")
  or die 'replica did not catch up after resume';

# The regression assertion. Without the in-flight watermark the page sits
# valid with OUTDATE clear and this still returns 'old'. Background replay
# stays disabled until after this read.
$result = $node_replica->safe_psql($db, 'select val from test where id = 1;');
is($result, 'new',
	'record parsed while the page was unbuffered is applied after catchup');

$result = $node_replica->safe_psql($db,
	"select inject_fault('polar_logindex_bg_skip_replay', 'reset');");
print "reset polar_logindex_bg_skip_replay: $result\n";

$result = $node_replica->safe_psql($db,
	"select inject_fault('polar_in_flight_buffer_arm', 'reset');");
print "reset polar_in_flight_buffer_arm: $result\n";

$node_replica->stop;
$node_primary->stop;

done_testing();
