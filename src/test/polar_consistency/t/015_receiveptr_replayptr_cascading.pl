# 015_receiveptr_replayptr_cascading.pl
#	  Test that GetStandbyFlushRecPtr bounds replica WAL sends by
#	  replayPtr, not receivePtr, in cascading shared-storage topology.
#
#
# IDENTIFICATION
#	  src/test/polar_consistency/t/015_receiveptr_replayptr_cascading.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $db = 'postgres';

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;

my $node_standby1 = PostgreSQL::Test::Cluster->new('standby1');
$node_standby1->polar_init_standby($node_primary);
$node_standby1->append_conf('postgresql.conf',
	"wal_buffers = '4MB'\npolar_xlog_queue_buffers = 16\n");

$node_primary->start;
$node_primary->polar_create_slot($node_standby1->name);
$node_standby1->start;
$node_standby1->polar_drop_all_slots;

my $node_replica2 = PostgreSQL::Test::Cluster->new('replica2');
$node_replica2->polar_init_replica($node_standby1);
$node_replica2->append_conf('postgresql.conf',
	"shared_buffers = '2MB'\nwal_receiver_status_interval = '100ms'\n");
$node_standby1->polar_create_slot($node_replica2->name);
$node_replica2->start;

# Resume paused standby1 and stop nodes in dependency order so the
# shutdown checkpoint can complete and pg_checksums post-check passes.
END
{
	return unless defined $node_primary;
	$node_replica2->stop('immediate', fail_ok => 1) if defined $node_replica2;
	if (defined $node_standby1)
	{
		eval { $node_standby1->safe_psql($db, 'SELECT pg_wal_replay_resume()') };
		$node_standby1->stop('fast', fail_ok => 1);
	}
	$node_primary->stop('fast', fail_ok => 1);
}

# Create the WAL-load table on all three nodes.
$node_primary->safe_psql($db,
	'CREATE TABLE test_tbl (id int PRIMARY KEY, val text)');
$node_primary->wait_for_catchup($node_standby1, 'replay',
	$node_primary->lsn('insert'));
$node_standby1->wait_for_catchup($node_replica2, 'replay',
	$node_standby1->lsn('replay'));

# Phase A: stop replica2 so its slot freezes; then insert ids 1..100000
# in five chunks to overflow standby1's 16 MB xlog queue past that
# frozen position. standby1 replays everything, so its shared storage
# holds the heap pages for these ids.
$node_replica2->stop('fast');
for my $chunk (1 .. 5)
{
	my $lo = ($chunk - 1) * 20000 + 1;
	my $hi = $chunk * 20000;
	$node_primary->safe_psql($db, qq{
		INSERT INTO test_tbl
			SELECT i, repeat('x', 200) FROM generate_series($lo, $hi) g(i);
	});
}
$node_primary->wait_for_catchup($node_standby1, 'replay',
	$node_primary->lsn('insert'));

# Phase B: pause standby1 replay, then insert ids 100001..200000.
# These rows go to heap pages that standby1 will not extend on its
# shared storage until replay resumes.
$node_standby1->safe_psql($db, 'SELECT pg_wal_replay_pause()');
for (my $i = 0; $i < 100; $i++)
{
	last if $node_standby1->safe_psql($db,
		'SELECT pg_get_wal_replay_pause_state()') =~ /pause/i;
	usleep(100_000);
}
my $standby_replay = $node_standby1->safe_psql($db,
	'SELECT pg_last_wal_replay_lsn()');

$node_primary->safe_psql($db, q{
	INSERT INTO test_tbl
		SELECT i, repeat('b', 200)
		FROM generate_series(100001, 200000) g(i);
});
my $tx_lsn = $node_primary->safe_psql($db, 'SELECT pg_current_wal_lsn()');
$node_primary->wait_for_catchup($node_standby1, 'flush', $tx_lsn);

my $gap = $node_standby1->safe_psql($db,
	"SELECT pg_last_wal_receive_lsn() - '$standby_replay'::pg_lsn");
ok($gap > 1048576, "standby receive-replay gap is large enough ($gap bytes)");

# Phase C: start replica2 -> walsender falls back to XLogSendPhysicalExt
# -> GetStandbyFlushRecPtr (the buggy code path).
$node_replica2->start;
my $streaming = 0;
for (my $i = 0; $i < 150; $i++)
{
	my $row = eval { $node_replica2->safe_psql($db,
		'SELECT status FROM pg_stat_wal_receiver') } // '';
	if ($row eq 'streaming') { $streaming = 1; last; }
	usleep(200_000);
}
ok($streaming, 'replica2 is streaming');

# Wait until replica2's receive_lsn stops growing (walsender has
# delivered everything it intends to deliver under the current
# upstream bound). Without this, we may sample receive_lsn before
# the buggy 'p'-message overshoot has propagated and miss the bug.
my $prev = '';
for (my $i = 0; $i < 30; $i++)
{
	my $cur = $node_replica2->safe_psql($db,
		'SELECT pg_last_wal_receive_lsn()');
	last if $cur eq $prev;
	$prev = $cur;
	usleep(200_000);
}

# Assertion 1 (root cause): replica2 flushedUpto must not race past
# standby1's paused replayPtr.
my $delta = $node_replica2->safe_psql($db,
	"SELECT pg_last_wal_receive_lsn() - '$standby_replay'::pg_lsn");
ok($delta < 1048576,
	"replica2 receive_lsn does not exceed standby1 replay_lsn ($delta bytes)");

# Assertion 2 (user-visible impact): if replica2 publicly advertises
# (via pg_last_wal_receive_lsn) that the commit LSN was received,
# the Phase B rows must be readable. In a cascading shared-storage
# topology standby1 owns its own shared storage and is the sole writer
# of data files there; its startup process extends them as it replays
# WAL. With replay paused, the heap pages for ids 100001..200000 are
# not extended on that storage, so any receive_lsn advertised past
# replayPtr is a lie that HA tooling and read-routing layers act on.
my $received = $node_replica2->safe_psql($db,
	"SELECT pg_last_wal_receive_lsn() >= '$tx_lsn'");
my $visible = $node_replica2->safe_psql($db,
	'SELECT count(*) FROM test_tbl WHERE id > 100000');

ok(!($received eq 't' && $visible ne '100000'),
	"Phase B rows are readable on replica2 when receive_lsn covers the commit (received=$received, visible=$visible)");

# Phase D: resume; replica2 must catch up cleanly.
$node_standby1->safe_psql($db, 'SELECT pg_wal_replay_resume()');
$node_primary->wait_for_catchup($node_standby1, 'replay', $tx_lsn);
$node_standby1->wait_for_catchup($node_replica2, 'replay',
	$node_standby1->lsn('replay'));
is($node_replica2->safe_psql($db,
	'SELECT count(*) FROM test_tbl WHERE id > 100000'),
	'100000', 'replica2 reads Phase B rows after full catchup');

done_testing();
