# 016_relmap_stale_cache_cascading.pl
#	  Verify that a cascading PolarDB replica can read relmap-managed
#	  catalog tables after VACUUM FULL on the primary, even when
#	  the replica lags behind.
#
# IDENTIFICATION
#	  src/test/polar_consistency/t/016_relmap_stale_cache_cascading.pl
#
# DESCRIPTION
#
# Topology:  primary  -->  standby1  -->  replica2
#              (shared-storage)  (streaming)  (shared-storage)
#
# polar_enable_sync_ddl makes the primary wait for its direct replicas
# (those with polar_replica_lock_lsn set in their replication slot).
# Standby slots have lock_lsn = InvalidXLogRecPtr, so the primary does
# NOT wait for standby1 or any node downstream of it.
#
# The test pauses WAL replay on replica2, runs VACUUM FULL pg_database
# on the primary (which reassigns the relfilenode), waits for standby1
# to replay, then checks that a new backend on replica2 can still
# read pg_database correctly despite its local relmap cache being
# seeded before the VACUUM FULL.

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
	# (it waits for replica2 to acknowledge the DDL lock LSN before truncating).
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

# Report replay position upstream promptly so the cascade wait_for_catchup
# calls converge in milliseconds instead of waiting out the 10s default
# interval. Safe here: this test induces no WAL backpressure.
$node_standby1->append_conf('postgresql.conf',
	"wal_receiver_status_interval = '100ms'\n");

$node_primary->start;
$node_primary->polar_create_slot($node_standby1->name);

$node_standby1->start;
$node_standby1->polar_drop_all_slots;

$node_replica2 = PostgreSQL::Test::Cluster->new('replica2');
$node_replica2->polar_init_replica($node_standby1);

# Tiny buffer pool on replica2 so old pages can be evicted.
$node_replica2->append_conf('postgresql.conf',
	"shared_buffers = 2MB\nwal_receiver_status_interval = '100ms'\n");

$node_standby1->polar_create_slot($node_replica2->name);
$node_replica2->start;

# Sanity check.
my $result =
  $node_replica2->safe_psql($db, 'SELECT count(*) FROM pg_database');
ok($result > 0, 'replica2 reads pg_database');

# Filler table for buffer eviction on replica2 (~830 pages).
$node_primary->safe_psql($db, q{
	CREATE TABLE filler AS
		SELECT i, repeat(chr(65 + (i % 26)), 100) AS pad
		FROM generate_series(1, 50000) AS g(i)
});

# Wait for the full cascade to catch up.
$node_primary->wait_for_catchup($node_standby1, 'replay',
	$node_primary->lsn('insert'));
$node_standby1->wait_for_catchup($node_replica2, 'replay',
	$node_standby1->lsn('replay'));

# Phase 1: seed replica2's local relmap cache (V1 -> R1).
$node_primary->safe_psql($db, 'VACUUM FULL pg_catalog.pg_database');
$node_primary->wait_for_catchup($node_standby1, 'replay',
	$node_primary->lsn('insert'));
$node_standby1->wait_for_catchup($node_replica2, 'replay',
	$node_standby1->lsn('replay'));

$result =
  $node_replica2->safe_psql($db, 'SELECT count(*) FROM pg_database');
ok($result > 0, 'replica2 reads pg_database after VACUUM FULL #1');

# Phase 2: pause replay on replica2, then VACUUM FULL on primary.
# polar_enable_sync_ddl is ON (default) — the primary waits for its
# direct replicas but NOT for standby1 or replica2.
$node_replica2->safe_psql($db, 'SELECT pg_wal_replay_pause()');

# Nudge standby1's startup so it forwards the pause-triggering WAL
# to replica2's startup process.
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

# VACUUM FULL #2 with sync_ddl ON.  The primary only waits for its
# direct replica slots — standby1's slot has lock_lsn = Invalid, so
# no wait.  After commit, smgrDoPendingDeletes truncates R1 on the
# primary's shared storage.  Standby1 replays the commit and
# truncates R1 on its own shared storage too.  Replica2 is paused.
$node_primary->safe_psql($db, 'VACUUM FULL pg_catalog.pg_database');

my $commit_lsn = $node_primary->lsn('flush');

# Synchronize to the race window before reading replica2: wait until standby1's
# startup process has either parked in the cascading-DDL barrier (fixed: it
# blocks before truncating R1 until the paused replica2 acks the lock LSN, so
# replay never reaches the commit) or replayed past the commit (buggy: no
# barrier, so it has already truncated R1). Either branch resolves in
# milliseconds, so the read below exercises the real, user-visible outcome
# quickly whether or not the fix is present.
$node_standby1->poll_query_until($db, qq{
	SELECT pg_last_wal_replay_lsn() >= '$commit_lsn'::pg_lsn
		OR EXISTS (SELECT 1 FROM pg_stat_activity
				   WHERE backend_type = 'startup'
					 AND wait_event = 'SyncRep')});

# Evict R1 from replica2's buffer pool.
$node_replica2->safe_psql($db, q{
	DO $$ BEGIN
		FOR i IN 1..6 LOOP
			PERFORM count(*) FROM filler;
		END LOOP;
	END $$;
});

# Phase 3: new connection on replica2 — the local relmap cache was
# seeded before VACUUM FULL #2.  The system must resolve pg_database
# to the current relfilenode despite the stale cache.
my ($stdout, $stderr);
my $rc = $node_replica2->psql($db,
	'SELECT count(*) FROM pg_catalog.pg_database',
	stdout  => \$stdout,
	stderr  => \$stderr,
	timeout => 10);

note("psql rc=$rc  stderr=[$stderr]") if $rc != 0;

is($rc, 0,
	'replica2 reads pg_database after VACUUM FULL on paused replica');

# Resume replica2 and let the whole cascade converge, so teardown is clean.
$node_replica2->safe_psql($db, 'SELECT pg_wal_replay_resume()');
$node_primary->wait_for_catchup($node_standby1, 'replay',
	$node_primary->lsn('insert'));
$node_standby1->wait_for_catchup($node_replica2, 'replay',
	$node_standby1->lsn('replay'));

$result =
  $node_replica2->safe_psql($db, 'SELECT count(*) FROM pg_catalog.pg_database');
ok($result > 0, 'replica2 reads pg_database after cascade catch-up');

done_testing();
