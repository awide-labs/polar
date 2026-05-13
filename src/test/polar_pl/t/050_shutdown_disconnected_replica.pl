#!/usr/bin/perl
# 050_shutdown_disconnected_replica.pl
#	  Regression test for the shutdown-checkpoint hang triggered by a stale
#	  oldest_apply_lsn left behind by a disconnected replica.
#
#	  Bug being tested:
#	    1. PolarDB primary + replica with shared storage, slot is in_use.
#	    2. Replica replays some WAL and feedback updates the slot's
#	       polar_replica_apply_lsn.
#	    3. Replica is stopped. Slot stays in_use, active_pid == 0, the slot's
#	       polar_replica_apply_lsn freezes -- no walsender is updating it any
#	       more, but polar_compute_and_set_replica_lsn() used to still pick
#	       it up, pinning XLogCtl->oldest_apply_lsn at the stale value.
#	    4. Primary inserts more rows so the buffer pool has dirty pages with
#	       latest_lsn > frozen apply_lsn.
#	    5. Fast shutdown of the primary. The shutdown checkpoint runs
#	       polar_flush_buffer_for_shutdown(); the fullpage-snapshot escape
#	       hatch is disabled when CHECKPOINT_IS_SHUTDOWN is set, so dirty
#	       buffers cannot be flushed past oldest_apply_lsn, consistent_lsn
#	       cannot advance past checkpoint.redo, polar_is_checkpoint_legal()
#	       stays false and the loop spins forever.
#
#	  The fix recomputes oldest_apply_lsn at the start of
#	  polar_flush_buffer_for_shutdown() with skip_inactive=true so slots
#	  whose walsender is gone (active_pid == 0) are dropped from the
#	  reduction. With the fix the shutdown checkpoint completes; without
#	  it the primary hangs and pg_ctl times out.
#
#	  Variants exercised in this file:
#	    - Variant 1: the canonical scenario described above -- single
#	                 replica, fast shutdown (`pg_ctl stop -m fast`) --
#	                 followed by an end-to-end restart phase: the primary
#	                 is restarted, the replica is restarted, and the
#	                 replica catches up to the post-shutdown row count,
#	                 validating that the replica re-syncs on next
#	                 startup. The two phases share one cluster setup to
#	                 avoid a redundant initdb/start/shutdown cycle.
#	    - Variant 2: same scenario under `pg_ctl stop -m smart`.
#	    - Variant 3: two replicas where one is stopped pre-shutdown and the
#	                 other is still streaming when the primary shutdown
#	                 starts (its walsender exits during shutdown). Both
#	                 slots end up inactive with stale (different) apply
#	                 LSNs; the fix must drop both.
#	    - Variant 4: paused walreceiver instead of a stopped replica. The
#	                 replica process is alive but its walreceiver has been
#	                 SIGSTOPped, so its apply_lsn is frozen and the slot
#	                 becomes inactive only when the primary's walsender
#	                 hits wal_sender_timeout during shutdown. Once the
#	                 primary is down, a SELECT on the replica must read
#	                 pages whose LSN is past the replica's replayed_lsn
#	                 with no fullpage entry available, so the replica
#	                 backend is expected to FATAL with "Read a future page
#	                 due to timeout" -- i.e. the fix never serves silently
#	                 wrong data, it just lets the primary shut down.
#	    - Variant 5: residual race -- the slot's walsender is still
#	                 ACTIVE when polar_flush_buffer_for_shutdown() makes
#	                 its initial polar_compute_and_set_replica_lsn(true)
#	                 call at the top of the function (so the slot's
#	                 frozen apply_lsn IS pinned on oldest_apply_lsn), and
#	                 only exits AFTER that call, inside the flush-wait
#	                 loop. Without an additional re-evaluation inside the
#	                 loop body, the slot turning inactive mid-loop is
#	                 invisible to the loop and the checkpoint hangs at
#	                 PGCTLTIMEOUT. We reproduce this deterministically
#	                 with wal_sender_timeout = 0 (walsender stays alive
#	                 on its own), a paused replica startup process
#	                 (replay_lsn frozen, but the walreceiver keeps
#	                 streaming feedback so the slot's apply_lsn is
#	                 frozen at L_seed), and a log-synchronized
#	                 background SIGTERM to the walsender driven by the
#	                 "polar_shutdown_flush_loop" fault, which logs from
#	                 inside polar_flush_buffer_for_shutdown's flush-wait
#	                 loop -- so the SIGTERM is guaranteed to land in the
#	                 loop. This variant is therefore skipped on builds
#                    without --enable-fault-injector.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(time usleep);
use POSIX ();

my $regress_db = 'postgres';

# Cap pg_ctl shutdown wait so a regression manifests as a bounded test
# failure rather than a hang. With the fix the shutdown checkpoint
# completes well within this window; without the fix pg_ctl will return
# non-zero after this many seconds.
my $shutdown_timeout = 30;
$ENV{PGCTLTIMEOUT} = $shutdown_timeout;

# Track every primary we spin up so an unexpected die in the middle of
# any subtest still tears the cluster down -- otherwise the framework's
# END block runs pg_checksums against a still-running data dir and
# BAIL_OUTs.
my @primaries_for_cleanup;
END
{
	for my $p (@primaries_for_cleanup)
	{
		if (defined $p && $p->polar_postmaster_is_alive)
		{
			$p->stop('immediate', fail_ok => 1);
		}
	}
}

# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------

# Common GUC tweaks: keep autovacuum / checkpointing quiet so the gap
# between the replica's frozen apply_lsn and the dirty buffers we
# produce after the replica is stopped is deterministic.
sub configure_quiet
{
	my ($node) = @_;

	$node->append_conf('postgresql.conf', q[autovacuum = off]);
	$node->append_conf('postgresql.conf', q[checkpoint_timeout = 1h]);
	# Default; set explicitly so the regression is unambiguous: the bug
	# escape hatch (polar_force_flush_buffer = on) MUST stay off.
	$node->append_conf('postgresql.conf', q[polar_force_flush_buffer = off]);
	# Send all server log lines to the file pg_ctl was launched with
	# (so $node->logfile actually contains "Checkpoint blocked" and
	# "checkpoint starting: shutdown immediate", not just the
	# pre-collector bootstrap. Otherwise assert_no_checkpoint_blocked
	# is silently toothless and v6's log-driven SIGTERM has nothing
	# to grep against.
	$node->append_conf('postgresql.conf', q[logging_collector = off]);
}

# On the primary, suppress the regular bgwriter's LRU sweep so we
# don't accidentally clean the seed-insert buffers between sessions,
# and shrink bgwriter_delay so the per-iteration sleep inside
# polar_flush_buffer_for_shutdown's flush-wait loop is short.
#
# We do NOT need to throttle the bgwriter to keep the future-page
# buffers around: by construction those buffers all have
# page_latest_lsn > oldest_apply_lsn (the slot's frozen value), so
# polar_buffer_can_be_flushed() refuses to flush them on the basic
# path, and our small WAL volume (~hundreds of KB) is well under
# polar_fullpage_snapshot_oldest_lsn_delay_threshold so the fullpage
# path doesn't fire either. The bug itself keeps them dirty.
#
# About bgwriter_delay specifically: the same GUC is reused as the
# sleep duration inside polar_flush_buffer_for_shutdown's
# flush-wait loop (xlog.c: `pg_usleep(BgWriterDelay * 1000L)`).
# At the default 200 ms, every variant spends >2 s sleeping in the
# loop even though the actual flush work is single-digit
# milliseconds (v1's "checkpoint complete" line shows write=0.005 s,
# sync=0.001 s, total=2.472 s). At 10 ms (the GUC minimum) the loop
# iterates ~20x faster. The earlier risk we worried about -- a LARGE
# bgwriter_delay timing out the loop -- does not apply in the other
# direction: a small value strictly speeds the loop up. We avoid
# going below 10 ms because that's the GUC's lower bound.
sub configure_slow_bgwriter
{
	my ($node) = @_;

	$node->append_conf('postgresql.conf', q[bgwriter_lru_maxpages = 0]);
	$node->append_conf('postgresql.conf', q[bgwriter_delay = 10]);
}

# Build a fresh polar primary on shared storage. Disable the framework's
# pg_checksums sweep at END for this primary: in the regression path the
# primary has to be torn down with immediate-stop (because the bug is
# precisely "the graceful shutdown checkpoint hangs"), which leaves
# pg_control marked "in production" and then pg_checksums refuses to
# run, masking our actual test failure with a BAIL_OUT.
sub new_primary
{
	my ($name) = @_;

	my $node = PostgreSQL::Test::Cluster->new($name);
	$node->polar_init_primary;
	$node->{_enable_data_checksums} = 0;
	configure_quiet($node);
	configure_slow_bgwriter($node);
	push @primaries_for_cleanup, $node;
	return $node;
}

sub new_replica
{
	my ($name, $primary) = @_;

	my $node = PostgreSQL::Test::Cluster->new($name);
	$node->polar_init_replica($primary);
	configure_quiet($node);
	return $node;
}

# Wait until the slot's polar_replica_apply_lsn has actually been
# recorded on the primary (the wal-feedback round-trip lags slightly
# behind replay catchup).
sub wait_for_apply_feedback
{
	my ($primary, $tag) = @_;

	my $ok = $primary->poll_query_until(
		$regress_db,
		qq[SELECT polar_oldest_apply_lsn() <> '0/0'::pg_lsn]);
	ok($ok, "[$tag] replica feedback recorded a valid oldest_apply_lsn");
	return $primary->safe_psql($regress_db,
		q[SELECT polar_oldest_apply_lsn()]);
}

# Produce dirty buffers on the primary whose latest_lsn is strictly
# greater than the slot's frozen apply_lsn. We deliberately do NOT issue
# an online CHECKPOINT here -- with the replica gone, an online
# checkpoint can also stall, and we want this test focused on the
# shutdown path the fix actually changes.
sub produce_future_pages
{
	my ($primary) = @_;

	$primary->safe_psql($regress_db,
		q[INSERT INTO t SELECT g, repeat('y', 100)
		  FROM generate_series(1, 1000) g]);
	$primary->safe_psql($regress_db,
		q[UPDATE t SET payload = repeat('z', 100) WHERE id <= 500]);
}

# Fast/smart shutdown the primary, asserting it completes inside our
# timeout. Returns elapsed seconds so the caller can sanity-check.
sub shutdown_primary_ok
{
	my ($primary, $mode, $tag) = @_;

	my $t0 = time();
	my $stop_ok = $primary->stop($mode, fail_ok => 1);
	my $elapsed = time() - $t0;
	note sprintf("[%s] primary %s-shutdown returned %s after %.2fs",
		$tag, $mode, $stop_ok ? 'success' : 'failure', $elapsed);

	ok($stop_ok,
		"[$tag] primary completed $mode shutdown checkpoint with disconnected replica slot")
	  or do
	{
		diag(
			"primary shutdown did not complete within ${shutdown_timeout}s -- "
			  . "tearing down with immediate mode for cleanup");
		$primary->stop('immediate', fail_ok => 1);
		return $elapsed;
	};

	ok($elapsed < $shutdown_timeout,
		sprintf("[%s] %s shutdown finished within %ds (took %.2fs)",
			$tag, $mode, $shutdown_timeout, $elapsed));
	return $elapsed;
}

# Scan the primary's log for the *bug-pattern* "Checkpoint blocked"
# warning. The warning itself can fire transiently even with the fix
# in place: once polar_compute_and_set_replica_lsn(skip_inactive=true)
# drops a stale slot, oldest_apply_lsn becomes Invalid (printed as
# "0/0"); on subsequent iterations of polar_flush_buffer_for_shutdown
# the loop-condition polar_is_checkpoint_legal() will still log
# "Checkpoint blocked. ... consistent lsn 0/X, oldest apply lsn 0/0"
# until consistent_lsn catches up to redo. Those messages are
# informational ("we are still flushing"), NOT the regression.
#
# The PRE-fix bug, in contrast, emits a warning with a NON-ZERO
# oldest_apply_lsn (the stale L_seed value pinned by the inactive
# slot): "... oldest apply lsn 0/40BE15A8" (or similar). That is the
# exact pattern the fix eliminates. We therefore search for a
# warning whose oldest_apply_lsn does NOT begin with "0/0 ".
#
# This is also why we disable logging_collector on every test
# primary: $primary->logfile is otherwise just the pre-collector
# bootstrap log and the assertion would silently never see anything,
# masking real regressions.
sub assert_no_checkpoint_blocked
{
	my ($primary, $tag) = @_;

	my $logtext = PostgreSQL::Test::Utils::slurp_file($primary->logfile);
	unlike(
		$logtext,
		qr{Checkpoint blocked\..*oldest apply lsn 0/[1-9A-Fa-f]},
		"[$tag] no bug-pattern \"Checkpoint blocked\" (non-zero oldest_apply_lsn) warning emitted during shutdown"
	);
}

# Initial seed traffic + verifying that the replica feedback has
# reached the slot. Returns the recorded oldest_apply_lsn.
sub seed_and_record_apply_lsn
{
	my ($primary, $replica, $tag) = @_;

	$primary->safe_psql($regress_db,
		q[INSERT INTO t SELECT g, repeat('x', 100)
		  FROM generate_series(1, 1000) g]);
	my $insert_lsn = $primary->lsn('insert');
	$primary->wait_for_catchup($replica->name, 'replay', $insert_lsn);

	return wait_for_apply_feedback($primary, $tag);
}

# -----------------------------------------------------------------------------
# Variant 1: single replica, fast shutdown, plus end-to-end restart and
# replica re-sync. This variant carries the precondition sanity checks
# (slot inactive, wal insert_lsn past the frozen apply_lsn) and the core
# shutdown assertion, then continues into the restart phase to validate
# "the replica re-syncs from shared storage on next startup" -- both
# parts share the same cluster setup, so running them as one subtest
# avoids one full initdb/start/shutdown cycle.
# -----------------------------------------------------------------------------
subtest 'fast shutdown with one disconnected replica, then restart and re-sync' => sub {
	my $tag = 'v1_fast_then_resync';
	my $primary = new_primary("${tag}_primary");
	my $replica = new_replica("${tag}_replica", $primary);

	$primary->start;
	$primary->polar_create_slot($replica->name);
	$primary->safe_psql($regress_db, 'CREATE EXTENSION polar_monitor;');
	$primary->safe_psql($regress_db,
		'CREATE TABLE t (id int, payload text)');
	$replica->start;

	my $oldest_apply_before_stop =
	  seed_and_record_apply_lsn($primary, $replica, $tag);
	note "[$tag] oldest_apply_lsn before replica stop: $oldest_apply_before_stop";

	$replica->stop;

	my $slot_inactive = $primary->safe_psql(
		$regress_db,
		qq[SELECT NOT active FROM pg_replication_slots
		   WHERE slot_name = '${\ $replica->name }']);
	is($slot_inactive, 't',
		"[$tag] replica slot is in_use but inactive after replica shutdown");

	produce_future_pages($primary);

	my $insert_lsn_after = $primary->safe_psql($regress_db,
		q[SELECT pg_current_wal_insert_lsn()]);
	my $gap_ok = $primary->safe_psql(
		$regress_db,
		qq[SELECT '$insert_lsn_after'::pg_lsn
		   > '$oldest_apply_before_stop'::pg_lsn]);
	is($gap_ok, 't',
		"[$tag] wal insert_lsn advanced past the frozen apply_lsn (dirty future-page gap exists)"
	);

	my $primary_count_before_shutdown = $primary->safe_psql($regress_db,
		q[SELECT count(*) FROM t]);

	shutdown_primary_ok($primary, 'fast', $tag);
	assert_no_checkpoint_blocked($primary, $tag);

	# Restart and verify the cluster is functional and the replica
	# re-syncs to the same row count it would have seen if it had been
	# online during the shutdown checkpoint.
	$primary->start;
	$replica->start;

	my $resync_lsn = $primary->lsn('insert');
	$primary->wait_for_catchup($replica->name, 'replay', $resync_lsn);

	my $primary_count_after_restart = $primary->safe_psql($regress_db,
		q[SELECT count(*) FROM t]);
	my $replica_count_after_restart = $replica->safe_psql($regress_db,
		q[SELECT count(*) FROM t]);

	is($primary_count_after_restart, $primary_count_before_shutdown,
		"[$tag] primary preserved row count across restart");
	is($replica_count_after_restart, $primary_count_after_restart,
		"[$tag] replica re-synced to primary's row count");

	# Clean shutdown for this scenario.
	$replica->stop;
	$primary->stop;
};

# -----------------------------------------------------------------------------
# Variant 2: single replica, smart shutdown
# -----------------------------------------------------------------------------
subtest 'smart shutdown with one disconnected replica' => sub {
	my $tag = 'v2_smart';
	my $primary = new_primary("${tag}_primary");
	my $replica = new_replica("${tag}_replica", $primary);

	$primary->start;
	$primary->polar_create_slot($replica->name);
	$primary->safe_psql($regress_db, 'CREATE EXTENSION polar_monitor;');
	$primary->safe_psql($regress_db,
		'CREATE TABLE t (id int, payload text)');
	$replica->start;

	seed_and_record_apply_lsn($primary, $replica, $tag);

	$replica->stop;
	produce_future_pages($primary);

	# Smart shutdown waits for clients to disconnect; we have none, so it
	# proceeds straight into the shutdown checkpoint -- exactly the path
	# the fix changes.
	shutdown_primary_ok($primary, 'smart', $tag);
	assert_no_checkpoint_blocked($primary, $tag);
};

# -----------------------------------------------------------------------------
# Variant 3: two replicas, one stopped before shutdown, one still
# streaming when primary shutdown begins (its walsender exits as part
# of shutdown). At the moment polar_flush_buffer_for_shutdown() runs
# both slots are inactive but with *different* frozen apply_lsns; the
# fix must drop both.
# -----------------------------------------------------------------------------
subtest 'fast shutdown with two replicas, mixed pre/post-shutdown stops' => sub {
	my $tag = 'v3_two';
	my $primary = new_primary("${tag}_primary");
	my $replica_a = new_replica("${tag}_replica_a", $primary);
	my $replica_b = new_replica("${tag}_replica_b", $primary);

	$primary->start;
	$primary->polar_create_slot($replica_a->name);
	$primary->polar_create_slot($replica_b->name);
	$primary->safe_psql($regress_db, 'CREATE EXTENSION polar_monitor;');
	$primary->safe_psql($regress_db,
		'CREATE TABLE t (id int, payload text)');
	$replica_a->start;
	$replica_b->start;

	# Seed traffic and wait for *both* replicas to feed their apply_lsn
	# back to the primary.
	$primary->safe_psql($regress_db,
		q[INSERT INTO t SELECT g, repeat('x', 100)
		  FROM generate_series(1, 1000) g]);
	my $seed_lsn = $primary->lsn('insert');
	$primary->wait_for_catchup($replica_a->name, 'replay', $seed_lsn);
	$primary->wait_for_catchup($replica_b->name, 'replay', $seed_lsn);

	# Both slots should now have a non-zero apply_lsn recorded.
	my $both_recorded = $primary->safe_psql(
		$regress_db,
		qq[SELECT bool_and(restart_lsn IS NOT NULL)
		   FROM pg_replication_slots
		   WHERE slot_name IN ('${\ $replica_a->name }',
		                       '${\ $replica_b->name }')]);
	is($both_recorded, 't', "[$tag] both replica slots have restart_lsn");

	# Stop replica A first. Slot A's apply_lsn freezes at the seed point.
	$replica_a->stop;

	# Drive the primary forward so replica B's apply_lsn advances past
	# A's frozen value -- now we have two slots with *different* frozen
	# apply_lsns ahead of us once B's walsender also exits.
	$primary->safe_psql($regress_db,
		q[INSERT INTO t SELECT g, repeat('w', 100)
		  FROM generate_series(2001, 3000) g]);
	my $mid_lsn = $primary->lsn('insert');
	$primary->wait_for_catchup($replica_b->name, 'replay', $mid_lsn);

	# Now produce future pages -- B is still streaming so its apply_lsn
	# may catch up some of these too, but the bgwriter throttle keeps
	# the dirty buffers in the flush list. When the primary shutdown
	# kicks in, B's walsender exits, leaving both slots inactive.
	produce_future_pages($primary);

	shutdown_primary_ok($primary, 'fast', $tag);
	assert_no_checkpoint_blocked($primary, $tag);

	# Stop the still-running replica B for a clean slate.
	$replica_b->stop('immediate', fail_ok => 1);
};

# -----------------------------------------------------------------------------
# Variant 4: replica process is alive but its walreceiver is SIGSTOPped
# (`stop_child('walreceiver')`), not the whole replica. The slot only
# becomes inactive when the primary's walsender gives up on the frozen
# receiver; once it does, the fix lets the shutdown checkpoint flush
# future pages to shared storage without producing fullpage entries.
# After the primary is down we deliberately read those future pages
# from the still-running replica -- it must NOT silently return wrong
# data; it is expected to FATAL with "Read a future page due to
# timeout" once polar_wait_old_version_page_timeout elapses.
# -----------------------------------------------------------------------------
subtest 'paused walreceiver: replica refuses to serve future pages after primary shutdown' => sub {
	my $tag = 'v4_paused';
	my $primary = new_primary("${tag}_primary");
	my $replica = new_replica("${tag}_replica", $primary);

	# Keep wal_sender_timeout below PGCTLTIMEOUT so shutdown finishes
	# promptly: with a paused walreceiver TCP send blocks, and the
	# walsender only releases its slot (active_pid = 0) after this
	# timeout fires.
	#
	# It must still exceed the worst-case time stop_child() spends
	# SIGSTOPping the walreceiver and running pstack (multi-second); if
	# it is too small, the walsender times out and clears the slot
	# before we assert the slot is still active.
	$primary->append_conf('postgresql.conf', q[wal_sender_timeout = 15s]);

	# Make the future-page detection fail fast on the replica side so
	# we don't sit in the retry loop for the full default 5s. This is
	# the GUC that governs when polar_logindex_restore_fullpage_snapshot_if_needed()
	# gives up and elogs FATAL.
	$replica->append_conf('postgresql.conf',
		q[polar_wait_old_version_page_timeout = 2000]);

	$primary->start;
	$primary->polar_create_slot($replica->name);
	$primary->safe_psql($regress_db, 'CREATE EXTENSION polar_monitor;');
	$primary->safe_psql($regress_db,
		'CREATE TABLE t (id int, payload text)');
	$replica->start;

	seed_and_record_apply_lsn($primary, $replica, $tag);

	# Pause the replica's walreceiver. The replica process keeps
	# running and the slot stays active_pid != 0 (walsender is alive)
	# until the primary's shutdown forces the walsender to give up.
	$replica->stop_child('walreceiver');

	# Slot is still ACTIVE here (walsender alive), unlike variants 1/2.
	my $slot_active_now = $primary->safe_psql(
		$regress_db,
		qq[SELECT active FROM pg_replication_slots
		   WHERE slot_name = '${\ $replica->name }']);
	is($slot_active_now, 't',
		"[$tag] slot remains active while walreceiver is only paused");

	produce_future_pages($primary);

	# Fast shutdown. With a paused walreceiver the walsender blocks on
	# send, hits wal_sender_timeout, then exits -- only THEN does the
	# slot become inactive and only then can the fix's skip_inactive
	# branch kick in. Shutdown still completes well within our budget.
	shutdown_primary_ok($primary, 'fast', $tag);
	assert_no_checkpoint_blocked($primary, $tag);

	# Primary is down; replica is still up with a frozen apply_lsn and
	# pages on shared storage whose LSN is past it. A heap scan must
	# refuse to serve those pages.
	#
	# $psql_safeguard is just an IPC::Run timeout: a defensive bound so
	# the test FAILS with "not ok" instead of HANGING if the replica
	# stops doing anything at all. The *meaningful* timeout for this
	# variant is polar_wait_old_version_page_timeout (set to 2000 ms
	# above), which is what actually causes the FATAL we assert on.
	my $psql_safeguard = 30;
	my $timed_out      = 0;
	my ($stdout, $stderr) = ('', '');
	my $ret = $replica->psql(
		$regress_db,
		'SELECT count(*) FROM t',
		stdout => \$stdout,
		stderr => \$stderr,
		timeout => $psql_safeguard,
		timed_out => \$timed_out);

	ok(!$timed_out,
		"[$tag] SELECT on replica did not hang "
		  . "(psql IPC safeguard ${psql_safeguard}s not tripped; "
		  . "real bound is polar_wait_old_version_page_timeout = 2000ms)");
	isnt($ret, 0,
		"[$tag] SELECT on replica fails after primary shutdown with paused walreceiver");
	like(
		$stderr,
		qr/Read a future page due to timeout/,
		"[$tag] replica reported \"Read a future page due to timeout\" -- no silent wrong data"
	);

	# Resume the walreceiver so the replica can be torn down cleanly.
	# The primary is gone so the walreceiver will just fail to
	# reconnect, but the replica postmaster will then exit normally.
	$replica->resume_child('walreceiver');
	$replica->stop('immediate', fail_ok => 1);
};

# -----------------------------------------------------------------------------
# Variant 5: residual race -- walsender still ACTIVE at the initial
# polar_compute_and_set_replica_lsn(true) call at the top of
# polar_flush_buffer_for_shutdown(), exits *during* the subsequent
# flush-wait loop.
#
# Expected outcome:
#   - With the in-tree fix (polar_compute_and_set_replica_lsn(true)
#     called both at the top of polar_flush_buffer_for_shutdown AND on
#     every iteration of the flush-wait loop), the iteration after
#     the SIGTERM observes active_pid == 0, drops the slot,
#     oldest_apply_lsn becomes InvalidXLogRecPtr, the future-page
#     buffers are flushed and the loop exits within PGCTLTIMEOUT --
#     this subtest PASSES.
#   - If the loop-internal call is ever removed (leaving only the
#     entry-point call), oldest_apply_lsn stays pinned at L_seed for
#     the rest of the loop, polar_buffer_can_be_flushed() refuses the
#     future-page buffers, the loop spins, pg_ctl times out and this
#     subtest FAILS -- that is the regression v5 protects against.
# -----------------------------------------------------------------------------
subtest 'walsender exits mid-checkpoint exposes residual race' => sub {
	my $tag = 'v5_race';

	# This variant pins the walsender SIGTERM to the
	# polar_shutdown_flush_loop fault (see polar_flush_buffer_for_shutdown
	# in xlog.c), which only exists in a fault-injector build. Without it
	# there is no race-free way to land the SIGTERM inside the flush-wait
	# loop, so skip the whole variant rather than fall back to a timing
	# heuristic.
	plan skip_all => 'this variant requires a --enable-fault-injector build'
	  unless ($ENV{enable_fault_injector} // '') eq 'yes';

	my $primary = new_primary("${tag}_primary");
	my $replica = new_replica("${tag}_replica", $primary);

	# Disable wal_sender_timeout so the walsender does NOT exit on its
	# own. The postmaster also does not signal walsenders to exit
	# until PM_SHUTDOWN_2, which it only reaches after the shutdown
	# checkpoint completes -- so the walsender's exit timing is
	# entirely under our control via the explicit SIGTERM below.
	$primary->append_conf('postgresql.conf', q[wal_sender_timeout = 0]);

	$primary->start;
	$primary->polar_create_slot($replica->name);
	$primary->safe_psql($regress_db, 'CREATE EXTENSION polar_monitor;');
	$primary->safe_psql($regress_db, 'CREATE EXTENSION faultinjector;');
	$primary->safe_psql($regress_db,
		'CREATE TABLE t (id int, payload text)');
	$replica->start;

	my $apply_lsn_seed =
	  seed_and_record_apply_lsn($primary, $replica, $tag);
	note "[$tag] frozen apply_lsn baseline: $apply_lsn_seed";

	# Freeze replay by SIGSTOPping the replica's startup process
	# (NOT its walreceiver -- see the section header for why). With
	# startup paused, GetXLogReplayRecPtr() never advances, so the
	# walreceiver's feedback messages keep arriving at the primary
	# with the same frozen applyPtr, pinning the slot's
	# polar_replica_apply_lsn. The walreceiver itself stays alive
	# and keeps draining the TCP receive queue, so the primary's
	# walsender's kernel send buffer never fills and its eventual
	# FATAL ereport will flush cleanly through pq_flush() instead of
	# deadlocking in secure_write's WaitEventSetWait(-1).
	$replica->stop_child('startup');

	my $slot_active = $primary->safe_psql(
		$regress_db,
		qq[SELECT active FROM pg_replication_slots
		   WHERE slot_name = '${\ $replica->name }']);
	is($slot_active, 't',
		"[$tag] slot is still active when shutdown is initiated");

	# Sanity check: with startup paused the slot's apply_lsn must
	# stay at the seed baseline. We use polar_oldest_apply_lsn()
	# (the value polar_compute_and_set_replica_lsn maintains) since
	# that is exactly what polar_flush_buffer_for_shutdown reads.
	my $apply_lsn_now = $primary->safe_psql(
		$regress_db, q[SELECT polar_oldest_apply_lsn()]);
	is($apply_lsn_now, $apply_lsn_seed,
		"[$tag] slot's apply_lsn is frozen at L_seed=$apply_lsn_seed (startup paused)"
	);

	# Capture the walsender pid via pg_stat_replication while we still
	# have a working primary connection. find_child(...) would also
	# work but pg_stat_replication is independent of how the walsender
	# dresses up its ps title.
	my $walsender_pid = $primary->safe_psql(
		$regress_db,
		qq[SELECT pid FROM pg_stat_replication
		   WHERE application_name = '${\ $replica->name }']);
	isnt($walsender_pid, '',
		"[$tag] located primary walsender pid=$walsender_pid");

	# Arm the polar_shutdown_flush_loop fault. It is reached only from
	# *inside* polar_flush_buffer_for_shutdown's flush-wait loop -- i.e.
	# strictly after the entry-point polar_compute_and_set_replica_lsn(true)
	# call has already pinned oldest_apply_lsn = L_seed against the
	# still-active slot. So when the helper below sees the fault's log
	# line it knows, that the loop is now spinning -- exactly the
	# window in which the SIGTERM must land.
	$primary->safe_psql($regress_db,
		q[SELECT inject_fault('polar_shutdown_flush_loop', 'enable', '', '', 1, 1, 0)]
	);

	produce_future_pages($primary);

	# We fork a helper process that waits, via the framework's
	# wait_for_log(), for the fault's "polar_shutdown_flush_loop reached"
	# log line -- emitted from inside the flush-wait loop -- and only
	# then SIGTERMs the walsender.
	my $marker = qr/POLAR fault: polar_shutdown_flush_loop reached/;
	my $log_offset = -s $primary->logfile;
	note
	  "[$tag] arming log-driven SIGTERM (sync on /$marker/) for walsender pid=$walsender_pid";
	my $kill_pid = fork();
	defined $kill_pid or die "[$tag] fork failed: $!";
	if ($kill_pid == 0)
	{
		# Helper process. Use POSIX::_exit so the framework's END
		# block (which runs immediate-stop on every primary in
		# @primaries_for_cleanup) does NOT fire from this child --
		# only the parent process owns the cluster lifecycle.
		eval { $primary->wait_for_log($marker, $log_offset); };
		# Even if wait_for_log timed out, fall through and try the
		# kill anyway -- a stuck walsender is exactly what we want
		# to nudge -- but log the timeout.
		diag("[$tag] wait_for_log croaked: $@") if $@;
		kill 'TERM', $walsender_pid;
		POSIX::_exit(0);
	}

	# With loop-internal recompute fix, this passes.
	# If a future change drops the loop-internal recompute, the loop
	# spins on stale oldest_apply_lsn = L_seed until PGCTLTIMEOUT and
	# this assertion fails -- that is the regression v6 guards.
	shutdown_primary_ok($primary, 'fast', $tag);
	assert_no_checkpoint_blocked($primary, $tag);

	# Reap the helper so it doesn't outlive the subtest.
	waitpid($kill_pid, 0);

	# Resume and tear down so the framework's END block doesn't
	# inherit a SIGSTOPped startup process across files.
	$replica->resume_child('startup');
	$replica->stop('immediate', fail_ok => 1);
};

done_testing();
