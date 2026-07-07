# 019_dbase_drop_cascading.pl
#	  Verify that DROP DATABASE replays through a cascading shared-storage
#	  standby without deadlocking the standby's startup process.
#
# IDENTIFICATION
#	  src/test/polar_consistency/t/019_dbase_drop_cascading.pl
#
# DESCRIPTION
#
# Topology:  primary  -->  standby1  -->  replica2
#              (shared-storage)  (streaming)  (shared-storage)
#
# dbase_redo() on the standby calls polar_wait_ddl_lock_on_standby() with the
# XLOG_DBASE_DROP record's own EndRecPtr before deleting the shared-storage
# database directory. The walsender to the replica is normally capped at the
# standby's replayPtr, which is stuck one record short of that barrier while
# the startup process is blocked inside dbase_redo -- so on the buggy build
# the replica can never acknowledge the barrier and the standby's replay
# stalls forever: a self-referential wait, i.e. a hard deadlock.
#
# The fix publishes the awaited barrier in WalSndCtl->polar_wait_ddl_lsn and
# lifts the walsender's send bound to Max(replayPtr, barrier)
# (polar_max_sendable_lsn), letting exactly that one record through. This
# test reproduces the scenario: on the fixed build standby1 replays past the
# drop and replica2 catches up; on the buggy build, wait_for_catchup below
# hangs until the poll times out.

use strict;
use warnings;
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

# Set up primary -> standby1 -> replica2.
$node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;

$node_standby1 = PostgreSQL::Test::Cluster->new('standby1');
$node_standby1->polar_init_standby($node_primary);

# Report replay position upstream promptly so the cascade catchup converges in
# milliseconds rather than the 10s default interval.
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

# Bring the whole cascade to a known-caught-up state.
$node_primary->safe_psql($db, 'CREATE DATABASE victim_db');
$node_primary->wait_for_catchup($node_standby1, 'replay',
	$node_primary->lsn('insert'));
$node_standby1->wait_for_catchup($node_replica2, 'replay',
	$node_standby1->lsn('replay'));

# DROP DATABASE -> XLOG_DBASE_DROP -> dbase_redo on standby1. With the
# unreachable-barrier bug, standby1's startup parks in polar_wait_ddl_lock_on_
# standby() and never replays past it, so this wait_for_catchup hangs.
$node_primary->safe_psql($db, 'DROP DATABASE victim_db');
my $drop_lsn = $node_primary->lsn('flush');

$node_primary->wait_for_catchup($node_standby1, 'replay', $drop_lsn);
$node_standby1->wait_for_catchup($node_replica2, 'replay',
	$node_standby1->lsn('replay'));

pass('cascade replayed DROP DATABASE without deadlocking standby1');

done_testing();
