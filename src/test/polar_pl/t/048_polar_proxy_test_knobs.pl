# 048_polar_proxy_test_knobs.pl
#
# Coverage for the three test knobs that ride alongside the proxy LSN
# wait:
#
#   * polar_query_delay_us
#       Microsecond sleep before each SELECT on the primary; skips
#       replicas, extended-protocol queries, and non-SELECT statements.
#
#   * polar_replay_min_lag_size
#       Forces the replica to hold replay behind WAL receive by at
#       least N bytes. Reloadable.
#
#   * polar_startup_replay_delay_size
#       Internal unit was switched from MB to bytes; this test asserts
#       that unit-suffixed values ('100MB') continue to parse and that
#       the raw-bytes form matches.
#
# IDENTIFICATION
#	  src/test/polar_pl/t/048_polar_proxy_test_knobs.pl

use strict;
use warnings;
use Time::HiRes qw(gettimeofday tv_interval);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# ---------------------------------------------------------------------
# Cluster setup
# ---------------------------------------------------------------------
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;

my $node_replica = PostgreSQL::Test::Cluster->new('replica');
$node_replica->polar_init_replica($node_primary);

$node_primary->start;
$node_primary->polar_create_slot($node_replica->name);
$node_replica->start;

$node_primary->safe_psql('postgres', 'CREATE TABLE t(i int);');
$node_primary->wait_for_catchup($node_replica);

# ---------------------------------------------------------------------
# 1. polar_query_delay_us - primary SELECT delayed by the configured
#    microsecond sleep. Use a small but measurable value so the test
#    stays stable on slow CI machines.
# ---------------------------------------------------------------------
my $delay_us = 500_000;    # 500ms; well above scheduler jitter under -j load
my $tolerance_s = 0.400;   # split point: delay-applied >= 400ms; no-delay < 400ms
                           # CI's worst observed jitter on an empty SELECT was
                           # ~95ms, so 400ms gives ~300ms of headroom each way.

sub timed_select
{
	my ($node, $sql) = @_;
	my $t0 = [gettimeofday];
	$node->safe_psql('postgres', $sql);
	return tv_interval($t0);
}

my $with_delay = timed_select($node_primary,
	"SET polar_query_delay_us = $delay_us; SELECT 1;");
cmp_ok($with_delay, '>=', $tolerance_s,
	"polar_query_delay_us slows primary SELECT (>= ${tolerance_s}s, got $with_delay)");

my $no_delay = timed_select($node_primary,
	"SET polar_query_delay_us = 0; SELECT 1;");
cmp_ok($no_delay, '<', $tolerance_s,
	"polar_query_delay_us = 0 removes the delay (< ${tolerance_s}s, got $no_delay)");

# Replica: same GUC value must NOT delay (RecoveryInProgress() gates).
my $replica_delay = timed_select($node_replica,
	"SET polar_query_delay_us = $delay_us; SELECT 1;");
cmp_ok($replica_delay, '<', $tolerance_s,
	"polar_query_delay_us is a no-op on replicas (< ${tolerance_s}s, got $replica_delay)");

# Non-SELECT on primary: must NOT delay (gate skips non-SelectStmt).
my $non_select = timed_select($node_primary,
	"SET polar_query_delay_us = $delay_us; INSERT INTO t VALUES (1);");
cmp_ok($non_select, '<', $tolerance_s,
	"polar_query_delay_us skips non-SELECT (< ${tolerance_s}s, got $non_select)");

# ---------------------------------------------------------------------
# 2. polar_startup_replay_delay_size - unit acceptance.
#    The GUC is PGC_SIGHUP with GUC_UNIT_BYTE. Both '100MB' and the
#    equivalent raw-bytes value must parse, and SHOW must report the
#    same human-readable string.
# ---------------------------------------------------------------------
$node_primary->safe_psql('postgres',
	"ALTER SYSTEM SET polar_startup_replay_delay_size = '100MB';");
$node_primary->safe_psql('postgres', 'SELECT pg_reload_conf();');
my $show_mb = $node_primary->safe_psql('postgres',
	'SHOW polar_startup_replay_delay_size;');
chomp $show_mb;
is($show_mb, '100MB', "polar_startup_replay_delay_size accepts '100MB'");

$node_primary->safe_psql('postgres',
	'ALTER SYSTEM SET polar_startup_replay_delay_size = 104857600;');
$node_primary->safe_psql('postgres', 'SELECT pg_reload_conf();');
my $show_bytes = $node_primary->safe_psql('postgres',
	'SHOW polar_startup_replay_delay_size;');
chomp $show_bytes;
is($show_bytes, $show_mb,
	"polar_startup_replay_delay_size: raw bytes (104857600) == '100MB'");

# Clean up: drop the override so it doesn't poison later test phases.
$node_primary->safe_psql('postgres',
	'ALTER SYSTEM RESET polar_startup_replay_delay_size;');
$node_primary->safe_psql('postgres', 'SELECT pg_reload_conf();');

# ---------------------------------------------------------------------
# 3. polar_replay_min_lag_size - forces a replay-vs-receive gap.
#    Set on the replica, generate enough WAL on the primary, then
#    observe that the receive LSN runs ahead of the replay LSN by at
#    least roughly the configured floor.
# ---------------------------------------------------------------------
my $min_lag_bytes = 8 * 1024 * 1024;    # 8 MiB - big enough to dominate noise
$node_replica->safe_psql('postgres',
	"ALTER SYSTEM SET polar_replay_min_lag_size = $min_lag_bytes;");
$node_replica->safe_psql('postgres', 'SELECT pg_reload_conf();');

# Generate ~64 MiB of WAL so the gap is easily satisfiable.
$node_primary->safe_psql('postgres',
	'INSERT INTO t SELECT generate_series(1, 2000000);');

# Poll for the lag floor to take effect (up to 30s).
my $observed_gap = 0;
for (1 .. 30)
{
	my $row = $node_replica->safe_psql(
		'postgres',
		q(SELECT (pg_last_wal_receive_lsn() - pg_last_wal_replay_lsn())::text;)
	);
	chomp $row;
	$observed_gap = $row + 0;
	last if $observed_gap >= $min_lag_bytes / 2;    # half floor = clearly active
	sleep 1;
}
cmp_ok($observed_gap, '>=', $min_lag_bytes / 2,
	"polar_replay_min_lag_size enforces receive-vs-replay gap (>= "
	  . ($min_lag_bytes / 2)
	  . " bytes, observed $observed_gap)");

# Lift the floor so the replica can fully catch up before we tear down.
$node_replica->safe_psql('postgres',
	'ALTER SYSTEM SET polar_replay_min_lag_size = 0;');
$node_replica->safe_psql('postgres', 'SELECT pg_reload_conf();');
$node_primary->wait_for_catchup($node_replica);

$node_primary->stop;
$node_replica->stop;
done_testing();
