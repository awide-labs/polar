# 017_heap_stale_relfilenode_cascading.pl
#	  Verify that a cascading PolarDB replica can read a regular
#	  heap table after VACUUM FULL on the primary, even when the
#	  replica lags behind.
#
#
# IDENTIFICATION
#	  src/test/polar_consistency/t/017_heap_stale_relfilenode_cascading.pl
#
# DESCRIPTION
#
# Topology:  primary  -->  standby1  -->  replica2
#              (shared-storage)  (streaming)  (shared-storage)
#
# Unlike tests 015/016, this case does not rely on relmap-managed catalogs.
# It uses a regular user table. Replica2 pauses WAL replay before VACUUM FULL,
# so its catalog still points to the old relfilenode while standby1 already
# replays deletion of that relfilenode from shared storage.

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
	# (it waits for replica2 to acknowledge the DDL lock LSN before deleting).
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

# Sanity check: replica2 can read regular table.
my $result =
  $node_replica2->safe_psql($db, 'SELECT count(*) FROM heap_race');
ok($result > 0, 'replica2 reads regular table');

# Pause replay on replica2, then VACUUM FULL on primary.
$node_replica2->safe_psql($db, 'SELECT pg_wal_replay_pause()');

# Nudge WAL flow.
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

$node_primary->safe_psql($db, 'VACUUM FULL heap_race');

my $commit_lsn = $node_primary->lsn('flush');

# Synchronize to the race window before reading replica2: wait until standby1's
# startup process has either parked in the cascading-DDL barrier (fixed: it
# blocks before deleting the old relfilenode until the paused replica2 acks the
# lock LSN, so replay never reaches the commit) or replayed past the commit
# (buggy: no barrier, so it has already deleted the relfilenode). Either branch
# resolves in milliseconds, so the read below exercises the real, user-visible
# outcome quickly whether or not the fix is present.
$node_standby1->poll_query_until($db, qq{
	SELECT pg_last_wal_replay_lsn() >= '$commit_lsn'::pg_lsn
		OR EXISTS (SELECT 1 FROM pg_stat_activity
				   WHERE backend_type = 'startup'
					 AND wait_event = 'SyncRep')});

# New backend on replica2 should still read heap_race correctly while replay is
# paused and its catalog still points at the old relfilenode.
my ($stdout, $stderr);
my $rc = $node_replica2->psql($db,
	'SELECT count(*) FROM heap_race',
	stdout  => \$stdout,
	stderr  => \$stderr,
	timeout => 10);

note("psql rc=$rc  stderr=[$stderr]") if $rc != 0;

is($rc, 0,
	'replica2 reads regular table after VACUUM FULL on paused replica');

# Resume replica2 and let the whole cascade converge, so teardown is clean.
$node_replica2->safe_psql($db, 'SELECT pg_wal_replay_resume()');
$node_primary->wait_for_catchup($node_standby1, 'replay',
	$node_primary->lsn('insert'));
$node_standby1->wait_for_catchup($node_replica2, 'replay',
	$node_standby1->lsn('replay'));

$result = $node_replica2->safe_psql($db, 'SELECT count(*) FROM heap_race');
ok($result > 0, 'replica2 reads regular table after cascade catch-up');

done_testing();
