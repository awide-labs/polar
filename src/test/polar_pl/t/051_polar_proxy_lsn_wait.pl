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

# Encode a uint64 byte offset as the textual form the strict parser
# accepts (plain unsigned integer; the GUC is uint64, not pg_lsn 'X/Y'
# format).
my $LSN_NUMERIC = q((pg_current_wal_lsn() - '0/0'::pg_lsn)::text);

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

$node_primary->safe_psql('postgres',
	'CREATE TABLE t(i int); INSERT INTO t SELECT generate_series(1, 1000);');
$node_primary->wait_for_catchup($node_replica);

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
# 4. SET-time parser rejects bad values rather than silently zeroing.
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

$node_primary->stop;
$node_replica->stop;
done_testing();
