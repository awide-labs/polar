#!/usr/bin/perl
# 016_promote_then_fast_shutdown.pl
#	  Regression test for the promote / fast-shutdown race in the logindex
#	  background worker.
#
#	  Bug being tested:
#	    1. A parallel-replay standby is promoted. StartupXLOG sets
#	       bg_redo_state = POLAR_BG_ONLINE_PROMOTE and declares recovery done,
#	       so `pg_ctl promote` returns here -- while the logindex bg worker
#	       still has to finish the online-promote replay and flip the state to
#	       POLAR_BG_REDO_NOT_START.
#	    2. A fast shutdown lands in that window. The bg worker's shutdown-exit
#	       check used to fire on `replay_done` alone, which only means the last
#	       dispatch pass drained -- NOT that the promote finished. So the worker
#	       exited with the state still stuck at POLAR_BG_ONLINE_PROMOTE.
#	    3. With the state stuck in a "parallel" state and nobody left to advance
#	       it, polar_cal_cur_consistent_lsn() keeps returning the now-frozen
#	       replayed_oldest_lsn instead of polar_max_valid_lsn(). The shutdown
#	       checkpoint can never reach checkpoint.redo, polar_is_checkpoint_legal()
#	       stays false, and pg_ctl times out.
#
#	  The fix makes the bg worker keep looping until the promote actually
#	  reaches POLAR_BG_REDO_NOT_START before it exits on shutdown.
#
#	  Determinism: the window in step 1->2 is sub-millisecond in the wild, so
#	  this test forces it with the `polar_hold_online_promote` fault, which
#	  parks the worker in ONLINE_PROMOTE and holds the NOT_START transition for
#	  one pass past the shutdown request -- guaranteeing the worker's exit-check
#	  runs at least once while the state is still ONLINE_PROMOTE. The test waits
#	  (via the fault's log marker) until the worker is parked before issuing the
#	  stop, so there are no timing heuristics. Requires --enable-fault-injector.
#
# IDENTIFICATION
#		  src/test/polar_consistency/t/016_promote_then_fast_shutdown.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(time);
use POSIX ();

# The whole point of this test is the fault-driven, deterministic handshake.
if (($ENV{enable_fault_injector} // '') ne 'yes')
{
	plan skip_all => 'this test requires a --enable-fault-injector build';
}

# Cap pg_ctl's shutdown wait so a regression manifests as a bounded failure
# rather than a hang: with the fix the shutdown checkpoint completes well
# within this window; without it pg_ctl returns non-zero after this long.
my $shutdown_timeout = 30;
$ENV{PGCTLTIMEOUT} = $shutdown_timeout;

my $tag = 'promote_then_fast_shutdown';
my $marker =
  qr/POLAR fault: polar_hold_online_promote parked in ONLINE_PROMOTE/;

# Disable _enable_data_checksums below to skip the framework's pg_checksums
# sweep, which would otherwise BAIL_OUT on a node left "in production" by
# immediate-stop.

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;
$node_primary->{_enable_data_checksums} = 0;

my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->polar_init_standby($node_primary);
$node_standby->{_enable_data_checksums} = 0;

# Keep things quiet/deterministic, and send server log output
# to the file pg_ctl launched with, so $node->logfile actually contains the
# fault's parked marker we synchronize on.
for my $n ($node_primary, $node_standby)
{
	$n->append_conf('postgresql.conf', 'autovacuum = off');
	$n->append_conf('postgresql.conf', 'checkpoint_timeout = 1h');
	$n->append_conf('postgresql.conf', 'wal_keep_size = 1024MB');
	$n->append_conf('postgresql.conf', 'logging_collector = off');
}
$node_primary->append_conf('postgresql.conf',
	"synchronous_standby_names='" . $node_standby->name . "'");

$node_primary->start;
$node_primary->polar_create_slot($node_standby->name);
$node_standby->start;

# inject_fault() is provided by the faultinjector extension; create it on the
# primary so it replicates into the standby's catalog, then drive it on the
# standby (inject_fault only touches the core fault shmem, fine on a follower).
$node_primary->safe_psql('postgres', 'CREATE EXTENSION faultinjector;');

# A little traffic so the standby has WAL to replay, and wait until it has it.
$node_primary->safe_psql('postgres', 'CREATE TABLE t (id int);');
$node_primary->safe_psql('postgres',
	'INSERT INTO t SELECT generate_series(1, 1000);');
my $insert_lsn = $node_primary->lsn('insert');
$node_primary->wait_for_catchup($node_standby, 'replay', $insert_lsn);

# Arm the hold: from now on, when the standby is promoted the logindex bg
# worker parks in POLAR_BG_ONLINE_PROMOTE instead of completing the promote.
$node_standby->safe_psql('postgres',
	"SELECT inject_fault('polar_hold_online_promote', 'enable', '', '', 1, -1, -1);"
);

# Promote the standby. pg_ctl promote returns once recovery is declared done,
# i.e. while the bg worker is still (now: parked) in ONLINE_PROMOTE.
my $log_offset = -s $node_standby->logfile;
$node_standby->promote;

# Wait until the bg worker has parked in ONLINE_PROMOTE, so
# the fast shutdown below is guaranteed to land in the ONLINE_PROMOTE window.
my $parked = eval { $node_standby->wait_for_log($marker, $log_offset); 1 };
ok($parked,
	"[$tag] logindex bg worker parked in POLAR_BG_ONLINE_PROMOTE after promote"
);

# Core assertion: a fast shutdown taken in the ONLINE_PROMOTE window must still
# complete. The buggy code leaves bg_redo_state stuck and the shutdown
# checkpoint spins until PGCTLTIMEOUT; the fix lets the promote finish so the
# checkpoint completes.
my $t0 = time();
my $stop_ok = $node_standby->stop('fast', fail_ok => 1);
my $elapsed = time() - $t0;
note sprintf("[%s] fast shutdown returned %s after %.2fs",
	$tag, $stop_ok ? 'success' : 'failure', $elapsed);

ok($stop_ok,
	"[$tag] freshly-promoted node completed its fast shutdown checkpoint")
  or $node_standby->stop('immediate', fail_ok => 1);

ok($elapsed < $shutdown_timeout,
	sprintf("[%s] fast shutdown finished within %ds (took %.2fs)",
		$tag, $shutdown_timeout, $elapsed));

$node_primary->stop('fast', fail_ok => 1);

done_testing();
