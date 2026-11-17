# 051_polar_proxy_lsn_wait.pl
#
# Coverage for the LSN-based session-consistency wait introduced for
# the connection-proxy path: the replica is told a target WAL position
# via polar_xact_split_wait_lsn, plus a timeout and a mode, and
# GetSnapshotData() blocks until replay reaches that position or the
# deadline fires.
#
# This test exercises that surface end-to-end on a real primary+replica
# pair:
#   * happy path  - target already replayed, query returns immediately;
#   * one-shot    - SHOW reports empty after the wait is consumed;
#   * best_effort - unreachable target with a short deadline raises a
#                   WARNING that carries the stable detail token, and
#                   the query still returns stale data;
#   * strict      - same setup raises an ERROR with the detail token;
#   * interruption- the full loop exposes PolarProxyLsnWait and cancel or
#                   fatal-exit cleanup records an interrupted terminal state;
#   * statistics  - cumulative counters, non-overlapping histograms,
#                   and the required accounting identity remain exact;
#   * parser      - garbage / negative / trailing-junk values are
#                   rejected at SET time (the strict parser, not the
#                   silent-zero default).
#
# IDENTIFICATION
#	  src/test/polar_pl/t/051_polar_proxy_lsn_wait.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes ();

my $stopped_waiter_pid;
END
{
	kill 'CONT', $stopped_waiter_pid if defined $stopped_waiter_pid;
}

# Encode a uint64 byte offset as the textual form the strict parser
# accepts (plain unsigned integer; the GUC is uint64, not pg_lsn 'X/Y'
# format).
my $LSN_NUMERIC = q((pg_current_wal_lsn() - '0/0'::pg_lsn)::text);

sub get_wait_stats
{
	my ($node) = @_;
	my %stats;
	my $rows = $node->safe_psql('postgres',
		'SELECT metric, value FROM polar_stat_proxy_lsn_wait() ORDER BY metric');

	for my $row (split(/\n/, $rows))
	{
		my ($metric, $value) = split(/\|/, $row, 2);

		$stats{$metric} = 0 + $value;
	}

	return \%stats;
}

# Start one strict-mode background psql blocked in the replay-wait loop
# on an unreachable target, and confirm its dedicated wait event.
sub start_blocked_waiter
{
	my ($node, $label, $target_lsn) = @_;
	my $waiter = $node->background_psql('postgres', on_error_stop => 0);
	my $pid = $waiter->query_safe('SELECT pg_backend_pid()');
	chomp $pid;

	$waiter->query_until(
		qr//, qq(
			SET polar_proxy_wait_timeout_ms = 5000;
			SET polar_consistency_mode = 'strict';
			SET polar_xact_split_wait_lsn = '$target_lsn';
			SELECT count(*) FROM t;
		));

	ok(
		$node->poll_query_until('postgres',
			"SELECT wait_event = 'PolarProxyLsnWait'
			 FROM pg_stat_activity WHERE pid = $pid;",
			't'),
		"$label: dedicated wait event is visible");

	return ($waiter, $pid);
}

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

$node_primary->safe_psql(
	'postgres', q(
		CREATE EXTENSION polar_monitor;
		CREATE TABLE t(i int);
		INSERT INTO t SELECT generate_series(1, 1000);
	));
$node_primary->wait_for_catchup($node_replica);

my $stats_before = get_wait_stats($node_replica);

# ---------------------------------------------------------------------
# 1. Happy path - target already in replay history returns at once.
# ---------------------------------------------------------------------
my $past_lsn =
  $node_primary->safe_psql('postgres', "SELECT $LSN_NUMERIC;");
chomp $past_lsn;

$node_primary->safe_psql('postgres', 'INSERT INTO t VALUES (9999);');
$node_primary->wait_for_catchup($node_replica);

my $rows = $node_replica->safe_psql(
	'postgres', qq(
		SET polar_xact_split_wait_lsn = '$past_lsn';
		SELECT count(*) FROM t;
	));
chomp $rows;
is($rows, '1001', 'happy path: query returns full row count');

# One-shot semantics: in-memory target is empty after the wait is
# consumed. Probed via a sentinel-wrapped current_setting so that the
# empty-string answer is preserved through safe_psql's output trim.
my $show = $node_replica->safe_psql(
	'postgres', qq(
		SET polar_xact_split_wait_lsn = '$past_lsn';
		SELECT count(*) FROM t;
		SELECT '<<' || current_setting('polar_xact_split_wait_lsn') || '>>';
	));
like($show, qr/<<>>/, 'one-shot: target is empty after consume');

# ---------------------------------------------------------------------
# 2. best_effort timeout - unreachable target, short deadline.
#    Expect a WARNING carrying the stable detail token and a row count
#    (stale data is acceptable in this mode).
# ---------------------------------------------------------------------
my $future_lsn = $node_primary->safe_psql('postgres',
	"SELECT ($LSN_NUMERIC\::bigint + 1073741824)::text;");
chomp $future_lsn;

my ($stdout, $stderr) = ('', '');
$node_replica->psql(
	'postgres',
	qq(
		SET polar_proxy_wait_timeout_ms = 10;
		SET polar_consistency_mode = 'best_effort';
		SET polar_xact_split_wait_lsn = '$future_lsn';
		SELECT count(*) FROM t;
	),
	stdout => \$stdout,
	stderr => \$stderr);

like($stderr, qr/WARNING/, 'best_effort: WARNING emitted on timeout');
like($stderr, qr/polar_proxy_lsn_wait_timeout/,
	'best_effort: stable detail token present');
like($stdout, qr/^\d+/m, 'best_effort: query returns rows despite timeout');

# ---------------------------------------------------------------------
# 3. strict timeout - same unreachable target, mode=strict.
#    Expect an ERROR with the stable detail token, query aborted.
# ---------------------------------------------------------------------
($stdout, $stderr) = ('', '');
my $rc = $node_replica->psql(
	'postgres',
	qq(
		SET polar_proxy_wait_timeout_ms = 10;
		SET polar_consistency_mode = 'strict';
		SET polar_xact_split_wait_lsn = '$future_lsn';
		SELECT count(*) FROM t;
	),
	stdout => \$stdout,
	stderr => \$stderr,
	on_error_die => 0);

isnt($rc, 0, 'strict: psql exits non-zero on timeout');
like($stderr, qr/ERROR/, 'strict: ERROR emitted on timeout');
like($stderr, qr/polar_proxy_lsn_wait_timeout/,
	'strict: stable detail token present');

# ---------------------------------------------------------------------
# 4. Finite waits clamp the final sleep to the remaining deadline.
# ---------------------------------------------------------------------
my $clamp_stats_before = get_wait_stats($node_replica);

($stdout, $stderr) = ('', '');
$node_replica->psql(
	'postgres',
	qq(
		SET polar_proxy_wait_timeout_ms = 15;
		SET polar_proxy_wait_max_delay_us = 10000;
		SET polar_consistency_mode = 'best_effort';
		SET polar_xact_split_wait_lsn = '$future_lsn';
		SELECT count(*) FROM t;
	),
	stdout => \$stdout,
	stderr => \$stderr);

like($stderr, qr/polar_proxy_lsn_wait_timeout/,
	'sleep clamp: finite wait reaches timeout');

my $clamp_stats_after = get_wait_stats($node_replica);
my $clamped_requested_sleep_us =
  $clamp_stats_after->{requested_sleep_us_total}
  - $clamp_stats_before->{requested_sleep_us_total};

cmp_ok($clamped_requested_sleep_us, '>', 0,
	'sleep clamp: requested sleep is recorded');
cmp_ok($clamped_requested_sleep_us, '<=', 15000,
	'sleep clamp: requested sleep does not exceed the deadline');

# ---------------------------------------------------------------------
# 5. Interruption - expose one wait event for the full loop, then cancel.
# ---------------------------------------------------------------------
my ($waiter, $waiter_pid) =
  start_blocked_waiter($node_replica, 'interrupt', $future_lsn);

is(
	$node_replica->safe_psql(
		'postgres', "SELECT pg_cancel_backend($waiter_pid);"),
	't',
	'interrupt: waiting backend is canceled');

ok(
	pump_until(
		$waiter->{run}, $waiter->{timeout},
		\$waiter->{stderr}, qr/canceling statement due to user request/),
	'interrupt: cancellation reached the wait loop');
$waiter->quit();

my ($terminated_waiter, $terminated_waiter_pid) =
  start_blocked_waiter($node_replica, 'fatal interrupt', $future_lsn);

is(
	$node_replica->safe_psql(
		'postgres', "SELECT pg_terminate_backend($terminated_waiter_pid);"),
	't',
	'fatal interrupt: waiting backend is terminated');

ok(
	pump_until(
		$terminated_waiter->{run}, $terminated_waiter->{timeout},
		\$terminated_waiter->{stderr},
		qr/terminating connection due to administrator command/),
	'fatal interrupt: termination reached the wait loop');
$terminated_waiter->reconnect_and_clear();
$terminated_waiter->quit();

# ---------------------------------------------------------------------
# 6. SET-time parser rejects bad values rather than silently zeroing.
# ---------------------------------------------------------------------
for my $bad ('garbage', '-1', '12345abc')
{
	my $err = '';
	$node_replica->psql(
		'postgres',
		"SET polar_xact_split_wait_lsn = '$bad';",
		stderr => \$err,
		on_error_die => 0);
	like($err, qr/ERROR/, "parser rejects '$bad'");
}

# ---------------------------------------------------------------------
# 7. Cumulative observability and accounting.
# ---------------------------------------------------------------------
my $stats_after = get_wait_stats($node_replica);

is(
	$stats_after->{calls_total} - $stats_before->{calls_total},
	7,
	'stats: all replay-wait calls counted');
is(
	$stats_after->{ready_immediately_total}
	  - $stats_before->{ready_immediately_total},
	2,
	'stats: immediate calls counted');
is(
	$stats_after->{waited_total} - $stats_before->{waited_total},
	5,
	'stats: sleep-loop entries counted');
is(
	$stats_after->{completed_after_wait_total}
	  - $stats_before->{completed_after_wait_total},
	0,
	'stats: no delayed call completed in this scenario');
is(
	$stats_after->{best_effort_timeout_total}
	  - $stats_before->{best_effort_timeout_total},
	2,
	'stats: best-effort timeout counted');
is(
	$stats_after->{strict_timeout_total}
	  - $stats_before->{strict_timeout_total},
	1,
	'stats: strict timeout counted before ERROR');
is(
	$stats_after->{interrupted_total} - $stats_before->{interrupted_total},
	2,
	'stats: interrupted wait counted');
is(
	$stats_after->{waiters_current} - $stats_before->{waiters_current},
	0,
	'stats: interruption decrements current waiters');

cmp_ok(
	$stats_after->{sleep_loops_total} - $stats_before->{sleep_loops_total},
	'>',
	0,
	'stats: sleep loops accumulated');
cmp_ok(
	$stats_after->{requested_sleep_us_total}
	  - $stats_before->{requested_sleep_us_total},
	'>',
	0,
	'stats: requested sleep accumulated');
cmp_ok(
	$stats_after->{actual_wait_us_total}
	  - $stats_before->{actual_wait_us_total},
	'>',
	0,
	'stats: monotonic actual wait accumulated');
cmp_ok(
	$stats_after->{initial_gap_bytes_total}
	  - $stats_before->{initial_gap_bytes_total},
	'>',
	0,
	'stats: initial replay gaps accumulated');

my $wait_histogram_delta = 0;
for my $metric (
	qw(wait_us_le_100 wait_us_100_1ms wait_us_1_2ms wait_us_2_5ms
	wait_us_5_10ms wait_us_10_50ms wait_us_50_100ms wait_us_100ms_1s
	wait_us_gt_1s))
{
	$wait_histogram_delta +=
	  $stats_after->{$metric} - $stats_before->{$metric};
}
is($wait_histogram_delta, 5, 'stats: wait histogram is non-overlapping');

my $gap_histogram_delta = 0;
for my $metric (
	qw(gap_bytes_le_4kb gap_bytes_4_64kb gap_bytes_64kb_1mb
	gap_bytes_1_8mb gap_bytes_gt_8mb))
{
	$gap_histogram_delta +=
	  $stats_after->{$metric} - $stats_before->{$metric};
}
is($gap_histogram_delta, 7, 'stats: gap histogram is non-overlapping');

is(
	$stats_after->{calls_total},
	$stats_after->{ready_immediately_total}
	  + $stats_after->{completed_after_wait_total}
	  + $stats_after->{strict_timeout_total}
	  + $stats_after->{best_effort_timeout_total}
	  + $stats_after->{interrupted_total}
	  + $stats_after->{waiters_current},
	'stats: required call accounting identity holds');

my $delayed_stats_before = get_wait_stats($node_replica);
$node_replica->safe_psql('postgres', 'SELECT pg_wal_replay_pause()');
$node_primary->safe_psql('postgres', 'INSERT INTO t VALUES (10000)');
$node_replica->poll_query_until('postgres',
	"SELECT pg_get_wal_replay_pause_state() = 'paused'")
  or die 'replay did not pause';
my $delayed_lsn =
  $node_primary->safe_psql('postgres', "SELECT $LSN_NUMERIC");
my $delayed_waiter = $node_replica->background_psql('postgres');
my $delayed_pid = $delayed_waiter->query_safe('SELECT pg_backend_pid()');
$delayed_waiter->query_until(qr//, qq(
	SET polar_proxy_wait_timeout_ms = 10000;
	SET polar_consistency_mode = 'strict';
	SET polar_xact_split_wait_lsn = '$delayed_lsn';
	SELECT 'delayed_rows=' || count(*) FROM t;
));
ok($node_replica->poll_query_until('postgres',
	"SELECT wait_event = 'PolarProxyLsnWait' FROM pg_stat_activity WHERE pid = $delayed_pid"),
	'delayed success: backend enters wait loop');
$node_replica->safe_psql('postgres', 'SELECT pg_wal_replay_resume()');
ok(pump_until($delayed_waiter->{run}, $delayed_waiter->{timeout},
	\$delayed_waiter->{stdout}, qr/delayed_rows=1002/),
	'delayed success: query sees replayed insert');
$delayed_waiter->quit();
my $delayed_stats_after = get_wait_stats($node_replica);
is($delayed_stats_after->{completed_after_wait_total}
	- $delayed_stats_before->{completed_after_wait_total}, 1,
	'delayed success: completion counted');

SKIP:
{
	skip 'SIGSTOP is unavailable on Windows', 6 if $windows_os;
	$node_replica->safe_psql('postgres', 'SELECT pg_wal_replay_pause()');
	$node_primary->safe_psql('postgres', 'INSERT INTO t VALUES (10001)');
	$node_replica->poll_query_until('postgres',
		"SELECT pg_get_wal_replay_pause_state() = 'paused'")
	  or die 'replay did not pause';
	my $late_lsn =
	  $node_primary->safe_psql('postgres', "SELECT $LSN_NUMERIC");
	my $late_stats_before = get_wait_stats($node_replica);
	my $late_waiter =
	  $node_replica->background_psql('postgres', on_error_stop => 0);
	my $late_pid = $late_waiter->query_safe('SELECT pg_backend_pid()');
	$late_waiter->query_until(qr//, qq(
		SET polar_proxy_wait_timeout_ms = 1000;
		SET polar_proxy_wait_max_delay_us = 10000;
		SET polar_consistency_mode = 'strict';
		SET polar_xact_split_wait_lsn = '$late_lsn';
		SELECT 'late_rows=' || count(*) FROM t;
	));
	ok($node_replica->poll_query_until('postgres',
		"SELECT wait_event = 'PolarProxyLsnWait' FROM pg_stat_activity WHERE pid = $late_pid"),
		'late replay: backend enters wait loop');
	# Let the loop settle into its long polling sleeps before stopping
	# the backend, so the stop lands inside a sleep and the resumed
	# iteration observes the reached target instead of re-checking
	# the deadline.
	Time::HiRes::sleep(0.1);
	is(kill('STOP', $late_pid), 1, 'late replay: stop waiting backend');
	$stopped_waiter_pid = $late_pid;
	Time::HiRes::sleep(1.1);
	$node_replica->safe_psql('postgres', 'SELECT pg_wal_replay_resume()');
	$node_primary->wait_for_catchup($node_replica);
	is(kill('CONT', $late_pid), 1, 'late replay: resume after target and deadline');
	undef $stopped_waiter_pid;
	ok(pump_until($late_waiter->{run}, $late_waiter->{timeout},
		\$late_waiter->{stdout}, qr/late_rows=1003/),
		'late replay: reached target takes precedence over the deadline');
	unlike($late_waiter->{stderr}, qr/polar_proxy_lsn_wait_timeout/,
		'late replay: no timeout once the target was reached');
	my $late_stats_after = get_wait_stats($node_replica);
	is($late_stats_after->{completed_after_wait_total}
	  - $late_stats_before->{completed_after_wait_total}, 1,
		'late replay: late success counted as completed');
	$late_waiter->quit();
}

# ---------------------------------------------------------------------
# 8. Large timeout must not overflow the int32 deadline.
#    polar_proxy_wait_timeout_ms is converted to usec via *1000.  Before
#    the fix this multiplication was done in int32, so any value above
#    INT_MAX/1000 (2147483 ms) wrapped negative and the strict-mode wait
#    errored out on its very first iteration instead of actually waiting.
#    Use an unreachable target so the wait can only terminate via (a) the
#    overflow-driven "LSN wait timeout" ERROR, or (b) the statement_timeout
#    safety net planted below.  A correct int64 deadline must reach (b),
#    never (a).
# ---------------------------------------------------------------------
my $big_timeout = 2147484;    # INT_MAX/1000 + 1: used to overflow int32

($stdout, $stderr) = ('', '');
my $rc_big = $node_replica->psql(
	'postgres',
	qq(
		SET statement_timeout = '2s';
		SET polar_proxy_wait_timeout_ms = $big_timeout;
		SET polar_consistency_mode = 'strict';
		SET polar_xact_split_wait_lsn = '$future_lsn';
		SELECT count(*) FROM t;
	),
	stdout => \$stdout,
	stderr => \$stderr,
	on_error_die => 0);

isnt($rc_big, 0, 'large timeout: query aborts (unreachable target)');
like($stderr, qr/statement timeout/,
	'large timeout: deadline waited for the real timeout, not an overflow ERROR');
unlike($stderr, qr/polar_proxy_lsn_wait_timeout/,
	'large timeout: no spurious LSN-wait-timeout ERROR from int32 overflow');

$node_primary->stop;
$node_replica->stop;
done_testing();
