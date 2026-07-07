# 021_dbase_drop_cascading_concurrent_reader.pl
#	  A backend on replica2 actively reading the to-be-dropped database must be
#	  conflict-resolved without wedging the cascade (and without missing-file
#	  errors or crashes) when standby1 drops that database.
#
# IDENTIFICATION
#	  src/test/polar_consistency/t/021_dbase_drop_cascading_concurrent_reader.pl
#
# DESCRIPTION
#
# Topology:  primary  -->  standby1  -->  replica2
#              (shared-storage)  (streaming)  (shared-storage)
#
# This exercises the barrier with a live reader in the loop -- the one case
# where releasing the barrier depends on conflict resolution: standby1's
# dbase_redo() waits until replica2 has applied XLOG_DBASE_DROP, and replica2
# cannot finish applying it until ResolveRecoveryConflictWithDatabase() has
# terminated the active reader. If the drop record is not delivered (the
# polar_max_sendable_lsn barrier lift broken), or the replica fails to kill
# the session, or the apply never completes, the cascade wedges here while
# the reader-less tests (019/020) still pass.
#
# The missing-file/PANIC assertions are secondary crash canaries: on localfs
# an already-open fd survives the unlink and rereads may be served from the
# buffer cache, so a too-early barrier is only probabilistically detectable
# through them.

use strict;
use warnings;
use IPC::Run qw(start timeout);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $db = 'postgres';
my ($node_primary, $node_standby1, $node_replica2);

END
{
	return unless defined $node_primary;

	$node_replica2->stop('fast', fail_ok => 1) if defined $node_replica2;
	$node_standby1->stop('fast', fail_ok => 1) if defined $node_standby1;
	$node_primary->stop('fast', fail_ok => 1)  if defined $node_primary;
}

$node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;

$node_standby1 = PostgreSQL::Test::Cluster->new('standby1');
$node_standby1->polar_init_standby($node_primary);
$node_standby1->append_conf('postgresql.conf',
	"wal_receiver_status_interval = '100ms'\n");

$node_primary->start;
$node_primary->polar_create_slot($node_standby1->name);

$node_standby1->start;
$node_standby1->polar_drop_all_slots;

$node_replica2 = PostgreSQL::Test::Cluster->new('replica2');
$node_replica2->polar_init_replica($node_standby1);
$node_replica2->append_conf('postgresql.conf',
	"wal_receiver_status_interval = '100ms'\n");

$node_standby1->polar_create_slot($node_replica2->name);
$node_replica2->start;

# victim_db with a multi-page table so reads touch storage.
$node_primary->safe_psql($db, 'CREATE DATABASE victim_db');
$node_primary->safe_psql('victim_db',
	q{CREATE TABLE t AS SELECT i, repeat('x', 100) AS pad
	  FROM generate_series(1, 20000) g(i)});
$node_primary->wait_for_catchup($node_standby1, 'replay',
	$node_primary->lsn('insert'));
$node_standby1->wait_for_catchup($node_replica2, 'replay',
	$node_standby1->lsn('replay'));

# Sustained reader inside victim_db on replica2: ~14s of repeated storage reads.
# When replica2 applies the DROP it is canceled by recovery-conflict resolution;
# the point is to keep reads in flight across the drop.
my $reader_sql = q{DO $$ BEGIN
  FOR i IN 1..70 LOOP
    PERFORM count(*) FROM t;
    PERFORM pg_sleep(0.2);
  END LOOP;
END $$;};

# Pass the SQL via -c, not stdin: IPC::Run delivers a stdin scalar only while
# the harness is pumped, and this test does not pump until finish -- fed via
# stdin, psql would sit connected but idle, and the drop would race nothing.
my ($stdout, $stderr);
my $stdin = '';
my $reader = start(
	[$node_replica2->installed_command('psql'), '-XAtq',
	 '-d',
	 $node_replica2->connstr('victim_db') . " application_name=victim_reader",
	 '-c', $reader_sql],
	'<', \$stdin, '>', \$stdout, '2>', \$stderr,
	timeout(60));
note("launched concurrent reader on replica2 in victim_db");

# Wait until the reader backend is actually connected to victim_db and running
# before dropping, so the race window is reliably exercised. Filter by the
# reader's application_name: the polling backend itself sits in victim_db with
# state = 'active', so a bare count would be satisfied by the poller alone.
$node_replica2->poll_query_until('victim_db',
	"SELECT count(*) > 0 FROM pg_stat_activity "
	  . "WHERE application_name = 'victim_reader' AND state = 'active'")
  or do
{
	$reader->kill_kill;
	die "reader backend did not become active in victim_db; "
	  . "reader stdout=[$stdout] stderr=[$stderr]";
};
$node_primary->safe_psql($db, 'DROP DATABASE victim_db');
my $drop_lsn = $node_primary->lsn('flush');

# Cascade must replay past the drop. standby1 parks in the barrier until
# replica2 has applied the drop (the lifted send bound makes its EndRecPtr
# reachable).
$node_primary->wait_for_catchup($node_standby1, 'replay', $drop_lsn);
$node_standby1->wait_for_catchup($node_replica2, 'replay', $drop_lsn);

# Reap the reader (recovery-conflict FATAL closes its connection; otherwise it
# finishes its loop). Either way it must not have hit a missing file.
$reader->finish;
note("reader stderr=[$stderr]");

# replica2 survived and the database is gone.
is($node_replica2->safe_psql($db, 'SELECT 1'), '1',
	'replica2 still up after concurrent DROP DATABASE');
is($node_replica2->safe_psql($db,
		"SELECT count(*) FROM pg_database WHERE datname = 'victim_db'"),
	'0', 'victim_db gone on replica2');

# The safety property: no missing-file error and no crash.
unlike($stderr, qr/could not open file/i,
	'reader did not hit a missing file during the drop');
unlike($stderr, qr/PANIC/i, 'reader did not crash');

done_testing();
