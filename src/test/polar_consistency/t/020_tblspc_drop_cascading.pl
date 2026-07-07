# 020_tblspc_drop_cascading.pl
#	  Verify that DROP TABLESPACE replays through a cascading shared-storage
#	  standby without deadlocking the standby's startup process.
#
# IDENTIFICATION
#	  src/test/polar_consistency/t/020_tblspc_drop_cascading.pl
#
# DESCRIPTION
#
# Topology:  primary  -->  standby1  -->  replica2
#              (shared-storage)  (streaming)  (shared-storage)
#
# Mirror of 019 for the XLOG_TBLSPC_DROP path. tblspc_redo() calls
# polar_wait_ddl_lock_on_standby() with the record's own EndRecPtr before
# removing the tablespace directories from shared storage. While the startup
# process is parked inside tblspc_redo, that barrier lies one record past
# replayPtr, where the cascading walsender is normally capped -- without the
# published-barrier lift of the send bound (polar_max_sendable_lsn) the
# replica can never acknowledge it and the cascade deadlocks. On the buggy
# build the wait_for_catchup below hangs; on the fixed build it passes at
# once.

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

# Bring the whole cascade to a known-caught-up state. An empty tablespace is
# required so that DROP emits XLOG_TBLSPC_DROP (tblspc_redo) rather than
# removing individual relation files.
my $tsdir = PostgreSQL::Test::Utils::tempdir;
$node_primary->safe_psql($db,
	"CREATE TABLESPACE victim_ts LOCATION '$tsdir'");
$node_primary->wait_for_catchup($node_standby1, 'replay',
	$node_primary->lsn('insert'));
$node_standby1->wait_for_catchup($node_replica2, 'replay',
	$node_standby1->lsn('replay'));

# DROP TABLESPACE -> XLOG_TBLSPC_DROP -> tblspc_redo on standby1. With the
# unreachable-barrier bug, standby1's startup parks in
# polar_wait_ddl_lock_on_standby() and this wait_for_catchup hangs.
$node_primary->safe_psql($db, 'DROP TABLESPACE victim_ts');
my $drop_lsn = $node_primary->lsn('flush');

$node_primary->wait_for_catchup($node_standby1, 'replay', $drop_lsn);
$node_standby1->wait_for_catchup($node_replica2, 'replay',
	$node_standby1->lsn('replay'));

pass('cascade replayed DROP TABLESPACE without deadlocking standby1');

done_testing();
