# 015_outdate_kept_until_replay_reaches_record.pl
#	  A backend that consumes a buffer's OUTDATE flag while the record that
#	  armed it is still above lastReplayedEndRecPtr must not lose the record:
#	  the flag has to stay armed until page replay can reach it.
#
# Without the polar_outdate_lsn watermark fix, the backend read in step 3
# clears OUTDATE, its replay range excludes the parsed-but-unpublished
# update record, and nothing is left marking the page stale: the final
# SELECT still returns the old value. In production such a page is repaired
# by the next replay pass over it -- the page LSN never advanced past the
# missed record, so it stays inside the range of a later apply -- which is
# why this test disables background replay and does not touch the row
# again.
#
# The test suspends the replica's startup process between the logindex
# parse of a heap record and the lastReplayedEndRecPtr update (fault point
# polar_stall_replay_frontier_publish), which is exactly the window the
# mini transaction used to cover before it was skipped for single-page
# records. Background logindex replay is disabled for the duration so it
# cannot repair the page behind the backend's back.
#
# IDENTIFICATION
#	  src/test/polar_consistency/t/015_outdate_kept_until_replay_reaches_record.pl

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

# Step 1: pull the page into the replica's buffer pool and replay it to the
# current frontier, so the coming update hits the buffered-page OUTDATE path.
my $result =
  $node_replica->safe_psql($db, 'select val from test where id = 1;');
is($result, 'old', 'replica sees initial row');

# Step 2: keep background logindex replay away from the page, otherwise it
# would apply the update record and mask a lost OUTDATE flag.
$result = $node_replica->safe_psql($db,
	"select inject_fault('polar_logindex_bg_skip_replay', 'enable', '', '', 1, -1, -1);"
);
print "enable polar_logindex_bg_skip_replay: $result\n";

# Suspend the startup process at the first heap record it replays, after the
# logindex parse (OUTDATE armed, entry inserted) but before it publishes
# lastReplayedEndRecPtr.
$result = $node_replica->safe_psql($db,
	"select inject_fault('polar_stall_replay_frontier_publish', 'suspend', '', '', 1, 1, 0);"
);
print "suspend polar_stall_replay_frontier_publish: $result\n";

# The update record is parsed on the replica but stays above the frontier.
$node_primary->safe_psql($db, "update test set val = 'new' where id = 1;");

$result = $node_replica->safe_psql($db,
	"select wait_until_triggered_fault('polar_stall_replay_frontier_publish', 1);"
);
print "startup suspended in the parse/publish window: $result\n";

# Step 3: the critical read. The backend consumes OUTDATE and replays the
# page, but its replay range cannot include the suspended record. It must
# re-arm OUTDATE instead of losing it. The value is still 'old' here either
# way, because the record is not replayable yet.
$result = $node_replica->safe_psql($db, 'select val from test where id = 1;');
is($result, 'old',
	'read during the window sees the pre-update value');

# Step 4: let the startup publish the record and catch up.
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

# Step 5: the regression assertion. With the OUTDATE flag lost, the page
# would be served stale here (still 'old') even though replay has passed the
# update record. Background replay stays disabled until after this read: it
# applies records to buffered pages regardless of OUTDATE and would repair
# the very page a lost flag leaves stale.
$result = $node_replica->safe_psql($db, 'select val from test where id = 1;');
is($result, 'new',
	'update is applied once replay reaches the record that armed OUTDATE');

$result = $node_replica->safe_psql($db,
	"select inject_fault('polar_logindex_bg_skip_replay', 'reset');");
print "reset polar_logindex_bg_skip_replay: $result\n";

$node_replica->stop;
$node_primary->stop;

done_testing();
