# 030_polar_new_slot_during_checkpoint.pl
#
# A physical slot created while a checkpoint is already running must keep
# its WAL.  PolarDB reserves such a slot at the redo point recorded in the
# *on-storage* control file (ReplicationSlotReserveWal), which can be several
# segments behind the redo point the running checkpoint has just chosen, so
# the checkpoint re-reads the slot minimum right before removing old WAL
# ("recalculate the current minimum LSN to avoid invalidating and cleaning
# the WAL segment used by the slot newly created", CreateCheckPoint).
#
# Every scenario below runs on a node with no other slot, reserves a slot at
# the redo point of the last completed (restart)point while the next one is
# in flight, and checks that the slot keeps its WAL.  They share one primary
# and one standby, each dropping its slot when done.
#
# 1. The slot is created before the checkpoint saves slots.  The node had no
#    slot when the checkpoint started, so the first reading of the slot
#    minimum is InvalidXLogRecPtr and only the re-read sees the new slot;
#    combining the two with a plain Min() picked the invalid one, and the
#    slot was invalidated with its WAL recycled.
#
# 2. The reservation spans the control file update.  The backend reads the
#    old redo point under ReplicationSlotAllocationLock, the checkpoint
#    writes the new one, and the checkpoint's re-read must wait for the
#    reservation to finish instead of missing the slot.  Run for a
#    checkpoint on the primary and for a restartpoint on a standby.
#
# 3. The slot is advanced in memory before the checkpoint removes WAL, and
#    the node crashes before any subsequent checkpoint can save the
#    advance.  The re-read comes after CheckPointGuts() has saved the
#    slots, so the checkpoint must save them again before bounding WAL
#    removal by it.  Otherwise the slot comes back at the old redo point
#    with its WAL removed: crash recovery does not need that WAL, but a
#    client starting from the slot, such as pg_receivewal with an empty
#    directory, fails.
#
# IDENTIFICATION
#	  src/test/polar_pl/t/030_polar_new_slot_during_checkpoint.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

# Cluster::init() appends PG_TEST_INITDB_EXTRA_OPTS after ->extra, so the
# global --wal-segsize=16 that CI sets would override the --wal-segsize=1
# below. Re-append our value so it wins.
local $ENV{PG_TEST_INITDB_EXTRA_OPTS} = $ENV{PG_TEST_INITDB_EXTRA_OPTS};
if (defined $ENV{PG_TEST_INITDB_EXTRA_OPTS}
	&& $ENV{PG_TEST_INITDB_EXTRA_OPTS} =~ m/wal-segsize=/)
{
	$ENV{PG_TEST_INITDB_EXTRA_OPTS} .= " --wal-segsize=1";
}

my $primary = PostgreSQL::Test::Cluster->new('primary');
# 1MB WAL segments: the test has to put the running checkpoint a couple of
# segments ahead of the control file, and PolarDB defaults to 1GB ones.
$primary->init(allows_streaming => 1, extra => ['--wal-segsize=1']);
# Leave max_wal_size/min_wal_size alone: init() sets max_wal_size high enough
# that nothing here triggers a checkpoint of its own, and both defaults stay
# valid whatever segment size we end up with.
$primary->append_conf(
	'postgresql.conf', qq(
	log_checkpoints = on
	logging_collector = off
	checkpoint_timeout = 1h
	wal_keep_size = 0
	max_slot_wal_keep_size = -1
));
$primary->start;
$primary->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$primary->safe_psql('postgres', 'CREATE TABLE t (a int)');

# Advance the primary through three WAL segments, so that a checkpoint
# ignoring the slot really does recycle the segment it points at.
sub advance_wal
{
	for (1 .. 3)
	{
		$primary->safe_psql('postgres',
			'INSERT INTO t SELECT generate_series(1, 1000)');
		$primary->safe_psql('postgres', 'SELECT pg_switch_wal()');
	}
}

# Checkpoint the primary, so that its control file holds the redo point a
# slot created during the next checkpoint reserves at (the anchor), then
# advance through three WAL segments past it.  Returns the anchor.
sub prepare_primary_wal_gap
{
	$primary->safe_psql('postgres', 'CHECKPOINT');
	my $anchor = $primary->safe_psql('postgres',
		'SELECT redo_lsn FROM pg_control_checkpoint()');
	advance_wal();
	return $anchor;
}

sub slot_restart_lsn
{
	my ($node, $slot) = @_;
	return $node->safe_psql('postgres',
		"SELECT restart_lsn FROM pg_replication_slots WHERE slot_name = '$slot'"
	);
}

# pg_walfile_name() is refused during recovery; all nodes stay on timeline 1,
# so ask the primary.
sub walfile_name
{
	my ($lsn) = @_;
	return $primary->safe_psql('postgres', "SELECT pg_walfile_name('$lsn')");
}

sub create_reserved_physical_slot
{
	my ($node, $slot) = @_;
	$node->safe_psql('postgres',
		"SELECT pg_create_physical_replication_slot('$slot', true)");
}

sub drop_slot
{
	my ($node, $slot) = @_;
	$node->safe_psql('postgres', "SELECT pg_drop_replication_slot('$slot')");
}

sub detach_point
{
	my ($node, $point) = @_;
	$node->safe_psql('postgres', "SELECT injection_points_detach('$point')");
}

# Detach a point attached with the 'wait' action and wake up whoever is
# already waiting on it.
sub release_wait_point
{
	my ($node, $point) = @_;
	detach_point($node, $point);
	$node->safe_psql('postgres', "SELECT injection_points_wakeup('$point')");
}

# Start a CHECKPOINT on $node in the background.  Returns the session and the
# log offset to wait for its completion from.
sub start_checkpoint
{
	my ($node) = @_;
	my $offset = -s $node->logfile;
	my $ckpt = $node->background_psql('postgres');
	$ckpt->query_until(
		qr/starting_checkpoint/,
		q(\echo starting_checkpoint
CHECKPOINT;
));
	return ($ckpt, $offset);
}

sub finish_checkpoint
{
	my ($node, $ckpt, $offset, $what) = @_;
	$node->wait_for_log(qr/(checkpoint|restartpoint) complete:/, $offset);
	ok($ckpt->quit, "$what: CHECKPOINT command succeeded");
}

# Start a checkpoint on the primary and park it at polar_delay_checkpoint_guts,
# right after its first reading of the slot minimum.  The park point logs on
# arrival; waiting for that line is what makes a slot created next provably
# created after the reading, with no timing assumption.
sub park_after_first_reading
{
	$primary->safe_psql('postgres',
		"SELECT injection_points_attach('polar_delay_checkpoint_guts', 'notice')"
	);
	my ($ckpt, $offset) = start_checkpoint($primary);
	$offset =
	  $primary->wait_for_log(qr/inject delay before checkpoint guts/,
		$offset);
	return ($ckpt, $offset);
}

# A slot created during a (restart)point must have reserved at the redo
# point of the last completed one, the anchor.  This only shows where the
# reservation happened, not whether the control file has advanced since.
# Returns that position.
sub check_reserved_at_anchor
{
	my ($node, $slot, $anchor, $what) = @_;
	my $lsn = slot_restart_lsn($node, $slot);
	is($lsn, $anchor,
		"$what: slot reserved at the redo point of the previous one");
	return $lsn;
}

# The slot must sit at least two segments behind the redo point the
# (restart)point removes WAL up to, or a pass proves nothing.
sub check_behind_redo
{
	my ($node, $lsn, $what) = @_;
	my $behind = $node->safe_psql(
		'postgres',
		"SELECT pg_wal_lsn_diff(redo_lsn, '$lsn')
		   >= 2 * pg_size_bytes(current_setting('wal_segment_size'))
		 FROM pg_control_checkpoint()");
	is($behind, 't',
		"$what: control file redo is at least two segments past the slot");
}

sub check_slot_kept
{
	my ($node, $slot, $seg, $what) = @_;
	is( $node->safe_psql(
			'postgres',
			"SELECT wal_status FROM pg_replication_slots WHERE slot_name = '$slot'"
		),
		'reserved',
		"$what: slot is not invalidated");
	ok(-f $node->data_dir . "/pg_wal/$seg",
		"$what: reserved WAL segment $seg is still present");
}

# Scenario 2, for $node's (restart)point parked at $point, which must sit
# past CheckPointGuts() and before the control file update.
sub check_reservation_window
{
	my ($node, $point, $what, $slot) = @_;

	my $anchor = $node->safe_psql('postgres',
		'SELECT redo_lsn FROM pg_control_checkpoint()');

	$node->safe_psql('postgres',
		"SELECT injection_points_attach('$point', 'wait')");
	my ($ckpt, $offset) = start_checkpoint($node);
	$node->wait_for_event('checkpointer', $point);

	# Reserve WAL for a new slot and park the backend while it holds
	# ReplicationSlotAllocationLock and the old redo point.
	$node->safe_psql('postgres',
		"SELECT injection_points_attach('polar_delay_slot_reserve_wal', 'notice')"
	);
	my $create = $node->background_psql('postgres');
	$create->query_until(
		qr/creating_slot/,
		qq(\\echo creating_slot
SELECT pg_create_physical_replication_slot('$slot', true);
));
	$offset =
	  $node->wait_for_log(qr/inject delay for set minimum slot required lsn/,
		$offset);

	# Let the checkpointer update the control file.  It must then wait for
	# the reservation before reading the slot minimum again.
	release_wait_point($node, $point);
	$node->wait_for_event('checkpointer', 'ReplicationSlotAllocation');
	pass("$what: checkpointer waits for the reservation");

	my $lsn = check_reserved_at_anchor($node, $slot, $anchor, $what);
	check_behind_redo($node, $lsn, $what);
	my $seg = walfile_name($lsn);

	# Finish the reservation, then the (restart)point.
	detach_point($node, 'polar_delay_slot_reserve_wal');
	ok($create->quit, "$what: slot creation succeeded");
	finish_checkpoint($node, $ckpt, $offset, $what);

	check_slot_kept($node, $slot, $seg, $what);
	drop_slot($node, $slot);
}

#
# Scenario 1: slot created after the first reading, on a node with no slot.
#
{
	my $what = 'slot created before slots are saved';
	my $slot = 'created_before_save';
	my $anchor = prepare_primary_wal_gap();

	my ($ckpt, $offset) = park_after_first_reading();
	create_reserved_physical_slot($primary, $slot);
	my $lsn = check_reserved_at_anchor($primary, $slot, $anchor, $what);
	my $seg = walfile_name($lsn);

	detach_point($primary, 'polar_delay_checkpoint_guts');
	finish_checkpoint($primary, $ckpt, $offset, $what);

	check_behind_redo($primary, $lsn, $what);
	check_slot_kept($primary, $slot, $seg, $what);
	drop_slot($primary, $slot);
}

#
# Scenario 2 on the primary: checkpoint.
#
prepare_primary_wal_gap();
check_reservation_window(
	$primary, 'polar-checkpoint-before-record',
	'checkpoint', 'reserved_across_checkpoint');

#
# Scenario 2 on a streaming standby: restartpoint.
#
$primary->backup('backup');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
$standby->start;

# Anchor the standby's control file on a restartpoint, then give it a newer
# checkpoint record to restart from, several segments further on.
$primary->safe_psql('postgres', 'CHECKPOINT');
$primary->wait_for_replay_catchup($standby);
$standby->safe_psql('postgres', 'CHECKPOINT');
advance_wal();
$primary->safe_psql('postgres', 'CHECKPOINT');
$primary->wait_for_replay_catchup($standby);
check_reservation_window(
	$standby, 'create-restart-point',
	'restartpoint', 'reserved_across_restartpoint');

$standby->stop;

#
# Scenario 3: slot advanced in memory before WAL removal, then a crash.
# Runs last, after the standby is gone, as it restarts the primary.
#
{
	my $what = 'slot advanced before WAL removal';
	my $slot = 'advanced_before_removal';
	my $anchor = prepare_primary_wal_gap();

	# Besides parking after the first reading, pause the checkpoint at
	# checkpoint-before-old-wal-removal: after it has saved the slots and
	# updated the control file, but before its second reading of the slot
	# minimum.  An advance made there is in memory only when that reading
	# sees it, which is what exposes the bug.
	$primary->safe_psql('postgres',
		"SELECT injection_points_attach('checkpoint-before-old-wal-removal', 'wait')"
	);
	my ($ckpt, $offset) = park_after_first_reading();

	# Creating the slot saves it to disk at the anchor.
	create_reserved_physical_slot($primary, $slot);
	my $saved_lsn = check_reserved_at_anchor($primary, $slot, $anchor, $what);

	# Let the checkpoint save slots, which skips the clean new one, update
	# the control file, and stop before its second reading.
	detach_point($primary, 'polar_delay_checkpoint_guts');
	$primary->wait_for_event('checkpointer',
		'checkpoint-before-old-wal-removal');
	check_behind_redo($primary, $saved_lsn, $what);

	# Advance the slot in memory only: this marks it dirty but does not save
	# it.
	$primary->safe_psql('postgres',
		"SELECT pg_replication_slot_advance('$slot', pg_current_wal_lsn())");
	my $advanced_lsn = slot_restart_lsn($primary, $slot);
	cmp_ok($advanced_lsn, 'ne', $saved_lsn, "$what: slot advanced in memory");

	release_wait_point($primary, 'checkpoint-before-old-wal-removal');
	finish_checkpoint($primary, $ckpt, $offset, $what);

	# Crash before any later checkpoint runs.
	$primary->stop('immediate');
	$primary->start;

	my $restart_lsn = slot_restart_lsn($primary, $slot);
	note(   "$what: restart_lsn after the crash: $restart_lsn (saved at "
		  . "creation: $saved_lsn, advanced to: $advanced_lsn)");
	check_slot_kept(
		$primary, $slot,
		walfile_name($restart_lsn),
		"$what, after a crash");

	# pg_receivewal with an empty directory asks the server for the slot's
	# restart_lsn (READ_REPLICATION_SLOT) and streams from the start of that
	# segment.  Stop it at a position already flushed so it exits on its own
	# when the WAL is there.
	$primary->safe_psql('postgres', 'SELECT pg_switch_wal()');
	my $endpos =
	  $primary->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
	$primary->safe_psql('postgres', 'INSERT INTO t VALUES (1)');
	my $recv_dir = PostgreSQL::Test::Utils::tempdir;
	$primary->command_ok(
		[
			'pg_receivewal', '-D', $recv_dir, '--slot',
			$slot, '-n', '--no-sync', '--verbose',
			'--endpos', $endpos
		],
		"$what: pg_receivewal streams from the slot after a crash");
	drop_slot($primary, $slot);
}

$primary->stop;

done_testing();
