# Testing logical decoding on a cascading standby while its upstream standby
# is promoted, with the Polar bulk WAL read enabled.  One transaction is
# decoded before the promotion and one after, so the decoding session has to
# read across the switchpoint, where the old timeline's WAL segment stops and
# the new timeline's copy of that same segment continues.  Test that both
# transactions reach the client.
#
# Two things are needed to get the bulk read to mishandle that boundary, and
# they are what shapes the test.  The walsender has to be reading the
# switchpoint's segment already, on the old timeline, so the decoding session
# is started and pumped until it has decoded the first transaction before the
# promotion.  And the bulk read only kicks in once a full WAL page beyond the
# boundary is available, whereas the walsender is normally woken by the first
# record past the switchpoint, finds less than a page and falls back to the
# single page read, so the walsender is frozen with SIGSTOP across the
# promotion and redo is let run far ahead of it.  Getting this wrong serves
# the walsender the old timeline's segment, which is zero-filled past the
# switchpoint, and the replication connection dies with "invalid record
# length at <switchpoint>".

use strict;
use warnings FATAL => 'all';

use IPC::Run;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $timeout = $PostgreSQL::Test::Utils::timeout_default;

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
my $node_cascading = PostgreSQL::Test::Cluster->new('cascading_standby');

# Initialize primary node
$node_primary->init(allows_streaming => 1);
$node_primary->append_conf(
	'postgresql.conf', q{
wal_level = 'logical'
autovacuum = off
});
$node_primary->start;
$node_primary->safe_psql('postgres',
	q[SELECT pg_create_physical_replication_slot('primary_physical');]);
$node_primary->safe_psql('postgres', q[CREATE DATABASE testdb]);
$node_primary->safe_psql('testdb',
	q[CREATE TABLE decoding_test(x integer, y text);]);
$node_primary->backup('b1');

# Set up the standby, this is the node that gets promoted
$node_standby->init_from_backup(
	$node_primary, 'b1',
	has_streaming => 1,
	has_restoring => 1);
$node_standby->append_conf('postgresql.conf',
	q[primary_slot_name = 'primary_physical']);
$node_standby->start;
$node_primary->wait_for_replay_catchup($node_standby);
$node_standby->safe_psql('postgres',
	q[SELECT pg_create_physical_replication_slot('standby_physical');]);

# Set up the cascading standby, this is the node that runs the decoding
$node_standby->backup('b2');
$node_cascading->init_from_backup(
	$node_standby, 'b2',
	has_streaming => 1,
	has_restoring => 1);
$node_cascading->append_conf(
	'postgresql.conf',
	q[primary_slot_name = 'standby_physical'
hot_standby_feedback = on
polar_logical_repl_xlog_bulk_read_size = 128
polar_enable_read_from_wal_buffers = off]);
$node_cascading->start;
$node_standby->wait_for_replay_catchup($node_cascading, $node_primary);

# The slot is created on timeline 1, before the promotion, so that decoding
# starts below the switchpoint.  The xl_running_xacts record it waits for has
# to come from the primary, the only node not in recovery.
$node_cascading->create_logical_slot_on_standby($node_primary, 'bulk_slot',
	'testdb');

# The first transaction, decoded on timeline 1
$node_primary->safe_psql('testdb',
	q[INSERT INTO decoding_test(x,y) SELECT s, s::text FROM generate_series(1,4) s;]
);
$node_primary->wait_for_replay_catchup($node_standby);
$node_standby->wait_for_replay_catchup($node_cascading, $node_primary);

my ($stdout, $stderr) = ('', '');
my $handle = IPC::Run::start(
	[
		'pg_recvlogical', '-d',
		$node_cascading->connstr('testdb'), '-S',
		'bulk_slot', '-o',
		'include-xids=0', '-o',
		'skip-empty-xacts=1', '--no-loop',
		'--start', '-f',
		'-'
	],
	'>', \$stdout,
	'2>', \$stderr,
	IPC::Run::timeout($timeout));

ok(pump_until($handle, IPC::Run::timer($timeout), \$stdout, qr/COMMIT/),
	'first transaction decoded on timeline 1');

# Promote, with the walsender frozen so that redo gets far ahead of it
my $walsender = $node_cascading->safe_psql('postgres',
	q[SELECT active_pid FROM pg_replication_slots WHERE slot_name = 'bulk_slot']
);
like($walsender, qr/^[0-9]+$/, "have walsender pid $walsender");
kill 'STOP', $walsender;

$node_standby->promote;

# Filler WAL to push the replay LSN well past the switchpoint.  It is emitted
# in the 'postgres' database, so the slot on 'testdb' ignores it and the
# decoded output stays clean.
$node_standby->safe_psql('postgres',
	q[SELECT pg_logical_emit_message(false, 'pad', repeat('x', 8000)) FROM generate_series(1,32);]
);
$node_standby->safe_psql('testdb',
	q[INSERT INTO decoding_test(x,y) SELECT s, s::text FROM generate_series(5,7) s;]
);
$node_standby->wait_for_replay_catchup($node_cascading);

kill 'CONT', $walsender;

# Both transactions have to come out of the same decoding session
my $ok = pump_until($handle, IPC::Run::timer($timeout),
	\$stdout, qr/^.*COMMIT.*COMMIT$/s);
ok($ok, 'logical decoding crosses the promotion timeline switch');
diag("pg_recvlogical stderr:\n$stderr") unless $ok;
$handle->kill_kill();

$node_cascading->stop('fast');
$node_standby->stop('fast');
$node_primary->stop('fast');

done_testing();
