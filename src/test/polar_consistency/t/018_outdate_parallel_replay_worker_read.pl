# 018_outdate_parallel_replay_worker_read.pl
#	  A parallel replay worker that reads a page in from storage must carry
#	  the record the startup process is still parsing for that page.
#
# On a standby past consistency the startup only parses; parallel replay
# workers apply. A worker needing a page that is not in the buffer pool reads
# it through the POLAR_REDO_MARK_OUTDATE branch of ReadBuffer_common(), which
# marks the buffer OUTDATE without replaying it. If that branch does not
# consult the in-flight publication the buffer carries no polar_outdate_lsn,
# and the next backend clears OUTDATE after replaying only to
# lastReplayedEndRecPtr - dropping the record the startup already inserted
# into the logindex.
#
# polar_logindex_bg_dispatch() stops at entries above
# polar_get_last_replayed_read_ptr(). An entry's lsn is its record's end and
# that bound is the start of the last replayed record, so an entry becomes
# dispatchable only once a *later* record has been replayed. Three updates are
# therefore needed, not two:
#
#   R0, R1  replayed and dispatchable
#   R2      parsed while the startup is suspended before publishing the
#           frontier, so its publication is live but its own entry is not yet
#           dispatchable
#
# With the startup parked on R2, a worker reads the page in for R0. A standby
# read then consumes OUTDATE. Once the startup resumes, R2 must still reach
# the page - and background replay is paused from then on, so only the
# watermark can carry it.
#
# IDENTIFICATION
#	  src/test/polar_consistency/t/018_outdate_parallel_replay_worker_read.pl

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
# keep background WAL out of the way of the occurrence-counted faults
$node_primary->append_conf('postgresql.conf', 'autovacuum=off');

# Parallel replay is the default on a standby; the worker paths under test
# exist only there. One worker keeps the hand-off below deterministic.
my $node_standby = PostgreSQL::Test::Cluster->new('standby1');
$node_standby->polar_init_standby($node_primary);
$node_standby->append_conf('postgresql.conf', 'logging_collector=off');
$node_standby->append_conf('postgresql.conf', 'autovacuum=off');
$node_standby->append_conf('postgresql.conf', 'polar_parallel_replay_proc_num=1');

$node_primary->start;
$node_primary->polar_create_slot($node_standby->name);

$node_primary->safe_psql($db, 'create extension faultinjector;');
$node_primary->safe_psql($db, 'create extension polar_monitor;');
$node_primary->safe_psql($db,
	'create table test(id int primary key, val text);');
$node_primary->safe_psql($db, "insert into test values(1, 'v0');");
$node_primary->safe_psql($db, 'checkpoint;');

$node_standby->start;

my $relfilenode = $node_primary->safe_psql($db,
	"select relfilenode from pg_class where relname = 'test';");
print "test relfilenode: $relfilenode\n";

my $lsn = $node_primary->safe_psql($db, 'select pg_current_wal_insert_lsn();');
$node_standby->poll_query_until($db,
	"select pg_last_wal_replay_lsn() >= '$lsn'::pg_lsn")
  or die 'standby did not catch up after initial load';

# Replaying the insert leaves the page in the standby's buffer pool, and a
# resident page is never read in by a worker. Restart to empty the pool.
$node_standby->restart;
$node_standby->poll_query_until($db, 'select true')
  or die 'standby did not come back';

# Deliberately no standby read of the page: it must still be unbuffered when
# the worker goes looking for it.

# Park the worker just before it reads a page that is not in the pool, so the
# read lands inside R2's parse window rather than before it.
my $result = $node_standby->safe_psql($db,
	"select inject_fault('polar_worker_read_absent_page', 'suspend', '', '$relfilenode', 1, 1, 0);"
);
print "suspend polar_worker_read_absent_page: $result\n";

# R0 and R1: replayed in full, so their logindex entries are dispatchable.
$node_primary->safe_psql($db, "update test set val = 'v1' where id = 1;");
$node_primary->safe_psql($db, "update test set val = 'v2' where id = 1;");
my $lsn_r1 = $node_primary->safe_psql($db, 'select pg_current_wal_insert_lsn();');
$node_standby->poll_query_until($db,
	"select pg_last_wal_replay_lsn() >= '$lsn_r1'::pg_lsn")
  or die 'standby did not parse R0 and R1';

$result = $node_standby->safe_psql($db,
	"select wait_until_triggered_fault('polar_worker_read_absent_page', 1);");
print "worker parked before reading the page in: $result\n";

# Suspend the startup after the parse of the next heap record and before it
# publishes the frontier. Commit records are not heap records, so this fires
# on R2 itself.
$result = $node_standby->safe_psql($db,
	"select inject_fault('polar_stall_replay_frontier_publish', 'suspend', '', '', 1, 1, 0);"
);
print "suspend polar_stall_replay_frontier_publish: $result\n";

# R2: parsed and published, frontier still behind it.
$node_primary->safe_psql($db, "update test set val = 'v3' where id = 1;");

$result = $node_standby->safe_psql($db,
	"select wait_until_triggered_fault('polar_stall_replay_frontier_publish', 1);"
);
print "startup parsed R2 and suspended: $result\n";

# Release the worker: it now reads the page in while R2 is in flight, marking
# the buffer OUTDATE - with the watermark only if the read consults the
# publication.
$result = $node_standby->safe_psql($db,
	"select inject_fault('polar_worker_read_absent_page', 'resume');");
print "resume polar_worker_read_absent_page: $result\n";
$result = $node_standby->safe_psql($db,
	"select inject_fault('polar_worker_read_absent_page', 'reset');");
print "reset polar_worker_read_absent_page: $result\n";

# The dispatcher forwards the background replayed lsn only once its tasks are
# done, so this waits for the worker to finish with the page.
$node_standby->poll_query_until($db,
	"select polar_replica_bg_replay_lsn() >= '$lsn_r1'::pg_lsn")
  or die 'worker did not finish R0 and R1';
print "worker applied R0 and R1\n";

# From here only the watermark may carry R2: no worker will pick it up.
$result = $node_standby->safe_psql($db,
	"select inject_fault('polar_logindex_bg_skip_replay', 'enable', '', '', 1, -1, -1);"
);
print "enable polar_logindex_bg_skip_replay: $result\n";

# This read consumes the OUTDATE flag the worker's read set. It replays no
# further than the frontier, which covers R1 but not R2.
$result = $node_standby->safe_psql($db, 'select val from test where id = 1;');
is($result, 'v2', 'read during the window sees R1 applied');

$result = $node_standby->safe_psql($db,
	"select inject_fault('polar_stall_replay_frontier_publish', 'resume');");
print "resume: $result\n";
$result = $node_standby->safe_psql($db,
	"select inject_fault('polar_stall_replay_frontier_publish', 'reset');");
print "reset: $result\n";

$lsn = $node_primary->safe_psql($db, 'select pg_current_wal_insert_lsn();');
$node_standby->poll_query_until($db,
	"select pg_last_wal_replay_lsn() >= '$lsn'::pg_lsn")
  or die 'standby did not catch up after resume';

# The regression assertion.
$result = $node_standby->safe_psql($db, 'select val from test where id = 1;');
is($result, 'v3',
	'record in flight during a worker page read is applied after catchup');

$result = $node_standby->safe_psql($db,
	"select inject_fault('polar_logindex_bg_skip_replay', 'reset');");
print "reset polar_logindex_bg_skip_replay: $result\n";

$node_standby->stop;
$node_primary->stop;

done_testing();
