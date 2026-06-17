# 018_heap_stale_relfilenode_cascading_subxact.pl
#	  Verify that the cascading sync-DDL barrier also protects downstream
#	  replicas when the file-dropping DDL is executed inside a
#	  sub-transaction (SAVEPOINT / PL exception block).
#
# IDENTIFICATION
#	  src/test/polar_consistency/t/018_heap_stale_relfilenode_cascading_subxact.pl
#
# DESCRIPTION
#
# Topology:  primary  -->  standby1  -->  replica2
#              (shared-storage)  (streaming)  (shared-storage)
#
# This is the sub-transaction variant of 017. The destructive DDL (here a
# TRUNCATE, which swaps in a new relfilenode and drops the old one at commit)
# is run inside a SAVEPOINT, so the XLOG_STANDBY_LOCK record that precedes the
# deletion carries a *sub*-transaction xid rather than the top-level xid.
#
# The cascading barrier records the lock LSN keyed by xid when standby1 replays
# XLOG_STANDBY_LOCK, and waits on it before DropRelationFiles() at commit. If
# the sub-xid is not resolved to the same key used at commit time, the lookup
# misses, the barrier is skipped, and standby1 deletes the old relfilenode from
# shared storage while the lagging replica2 is still resolving the table to it.
# A new backend on replica2 then hits ENOENT ("could not open file").
#
# Note on why the sub-xid key cannot be resolved during replay: standby1 only
# learns a sub-xid's top-level parent from an XLOG_XACT_ASSIGNMENT record, which
# the primary emits lazily (after PGPROC_MAX_CACHED_SUBXIDS sub-xids, or under
# wal_level=logical). With a single SAVEPOINT under wal_level=replica no such
# record precedes the lock record, so pg_subtrans has no parent entry yet.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $db = 'postgres';
my ($node_primary, $node_standby1, $node_replica2);

END
{
	return unless defined $node_primary;

	# Resume replay so standby1 is not left blocked in the cascading barrier
	# at shutdown (relevant once the barrier is honoured for the sub-xact).
	$node_replica2->safe_psql($db, 'SELECT pg_wal_replay_resume()',
		on_error_die => 0)
	  if defined $node_replica2;

	$node_replica2->stop('fast', fail_ok => 1)
	  if defined $node_replica2;
	$node_standby1->stop('fast', fail_ok => 1)
	  if defined $node_standby1;
	$node_primary->stop('fast', fail_ok => 1);
}

# Set up primary -> standby1 -> replica2.
$node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;

$node_standby1 = PostgreSQL::Test::Cluster->new('standby1');
$node_standby1->polar_init_standby($node_primary);

# Report replay position upstream promptly so the final wait_for_catchup
# converges in milliseconds instead of waiting out the 10s default interval.
$node_standby1->append_conf('postgresql.conf',
	"wal_receiver_status_interval = '100ms'\n");

$node_primary->start;
$node_primary->polar_create_slot($node_standby1->name);

$node_standby1->start;
$node_standby1->polar_drop_all_slots;

$node_replica2 = PostgreSQL::Test::Cluster->new('replica2');
$node_replica2->polar_init_replica($node_standby1);

# Tiny buffer pool on replica2, smaller than heap_race below, so a seqscan of
# the table cannot be served from cache and must open its file on storage.
# Low status interval so the final cascade wait_for_catchup converges quickly.
$node_replica2->append_conf('postgresql.conf',
	"shared_buffers = 256kB\nwal_receiver_status_interval = '100ms'\n");

$node_standby1->polar_create_slot($node_replica2->name);
$node_replica2->start;

# heap_race is the victim table. 5000 rows is ~80 pages, several times the
# 32-page (256kB) buffer pool, so reads always touch storage.
$node_primary->safe_psql($db, q{
	CREATE TABLE heap_race AS
		SELECT i, repeat(chr(65 + (i % 26)), 100) AS pad
		FROM generate_series(1, 5000) AS g(i)
});

# Wait for full cascade catch-up.
$node_primary->wait_for_catchup($node_standby1, 'replay',
	$node_primary->lsn('insert'));
$node_standby1->wait_for_catchup($node_replica2, 'replay',
	$node_standby1->lsn('replay'));

# Sanity check: replica2 can read the regular table.
my $result =
  $node_replica2->safe_psql($db, 'SELECT count(*) FROM heap_race');
is($result, 5000, 'replica2 reads regular table');

# Phase 1: pause replay on replica2, then run the destructive DDL on primary.
$node_replica2->safe_psql($db, 'SELECT pg_wal_replay_pause()');

# Nudge WAL flow so the pause request is observed.
$node_primary->safe_psql($db, 'SELECT txid_current()');

my $paused = 0;
for (my $i = 0; $i < 100; $i++)
{
	my $state = $node_replica2->safe_psql($db,
		'SELECT pg_get_wal_replay_pause_state()');
	if ($state =~ /pause/i)
	{
		$paused = 1;
		last;
	}
	usleep(100_000);
}
ok($paused, 'WAL replay paused on replica2');
BAIL_OUT('could not pause replay on replica2') unless $paused;

# TRUNCATE inside a SAVEPOINT: the AccessExclusiveLock (and therefore the
# XLOG_STANDBY_LOCK record) is taken under the sub-transaction xid, while the
# old relfilenode is dropped by DropRelationFiles() at the top-level commit.
$node_primary->safe_psql($db, q{
	BEGIN;
	SAVEPOINT sp1;
	TRUNCATE heap_race;
	RELEASE SAVEPOINT sp1;
	COMMIT;
});

my $commit_lsn = $node_primary->lsn('flush');

# Synchronize to the race window before reading replica2: wait until standby1's
# startup process has either parked in the cascading-DDL barrier (fixed: it
# blocks before deleting the old relfilenode until the paused replica2 acks the
# lock LSN, so replay never reaches the commit) or replayed past the commit
# (buggy: the barrier was keyed wrong for the sub-xact and missed, so it has
# already deleted the relfilenode). Either branch resolves in milliseconds, so
# the read below exercises the real, user-visible outcome quickly whether or not
# the fix is present.
$node_standby1->poll_query_until($db, qq{
	SELECT pg_last_wal_replay_lsn() >= '$commit_lsn'::pg_lsn
		OR EXISTS (SELECT 1 FROM pg_stat_activity
				   WHERE backend_type = 'startup'
					 AND wait_event = 'PolarCascadingSyncDDL')});

# Phase 2: while replica2 is still paused its catalog points at the old
# relfilenode. Reading the table must not fail: with the sub-xact barrier in
# place standby1 has not (yet) removed that relfilenode from shared storage.
my ($stdout, $stderr);
my $rc = $node_replica2->psql($db,
	'SELECT count(*) FROM heap_race',
	stdout  => \$stdout,
	stderr  => \$stderr,
	timeout => 10);

note("psql rc=$rc stdout=[$stdout] stderr=[$stderr]") if $rc != 0;

is($rc, 0,
	'replica2 reads table after TRUNCATE-in-SAVEPOINT on lagging cascade');

# Resume replica2 and let the whole cascade converge, so teardown is clean.
$node_replica2->safe_psql($db, 'SELECT pg_wal_replay_resume()');
$node_primary->wait_for_catchup($node_standby1, 'replay',
	$node_primary->lsn('insert'));
$node_standby1->wait_for_catchup($node_replica2, 'replay',
	$node_standby1->lsn('replay'));

# After replay, the table resolves to the new (empty) relfilenode everywhere.
$result = $node_replica2->safe_psql($db, 'SELECT count(*) FROM heap_race');
is($result, 0, 'replica2 reads truncated table after cascade catch-up');

done_testing();
