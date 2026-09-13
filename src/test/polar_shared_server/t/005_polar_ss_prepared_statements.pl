# Shared Server: prepared statements and the plans behind them belong to the
# session, not to the backend.
#
# A pooled backend serves one client after another and does not exit in
# between.  While prepared_queries and saved_plan_list were plain process-wide
# statics, what one client prepared was still there for the next one: it could
# read it through pg_prepared_statements, EXECUTE it, and its own PREPARE of
# that name failed with "prepared statement already exists" for a statement it
# had never created.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use PolarSharedServer;

my $node = PostgreSQL::Test::Cluster->new('ss_prep');
$node->polar_init_primary;

my $ss = PolarSharedServer->new($node);

# One dispatcher and one backend, so that every client lands on the same one.
$node->append_conf('postgresql.conf', ss_conf(backend_max_count => 1));

$node->start;

sub prepared_count
{
	my ($session) = @_;
	my $n = $session->query_safe('SELECT count(*) FROM pg_prepared_statements');
	chomp $n;
	return $n;
}

#
# 1. A client must not inherit the prepared statements of the client that used
#    the backend before it.
#
my $first = $ss->psql_session();
my $pooled_pid = backend_pid($first);

$first->query_safe('PREPARE leaked_stmt AS SELECT 1');
is(prepared_count($first), '1', 'the first client sees its own statement');
$first->quit;

my $second = $ss->psql_session();
is(backend_pid($second), $pooled_pid,
	'second client landed on the same pooled backend');

is(prepared_count($second), '0',
	'a new client starts with no prepared statements');
is( $second->query_safe(
		"SELECT count(*) FROM pg_prepared_statements WHERE name = 'leaked_stmt'"),
	'0',
	"the previous client's statement is not visible");

# EXECUTE of a name this client never prepared must fail, and fail with the
# error that says so rather than running somebody else's statement.
query_fails(
	$second,
	'EXECUTE leaked_stmt',
	"EXECUTE of the previous client's statement fails",
	qr/prepared statement "leaked_stmt" does not exist/,
	'EXECUTE reports the right error');

# And preparing that name now succeeds: no collision with a departed client.
$second->query_safe('PREPARE leaked_stmt AS SELECT 2');
is($second->query_safe('EXECUTE leaked_stmt'),
	'2', 'the new client can prepare that name itself');
$second->quit;

#
# 2. Two clients alternating on one backend each keep their own statements,
#    under the same name, over the same unqualified table name.  The backend
#    only changes client between transactions, so interleaving the two is what
#    forces the handover.
#
$node->safe_psql(
	'postgres', q{
CREATE SCHEMA s1;
CREATE TABLE s1.t(v int);
INSERT INTO s1.t VALUES (1);
CREATE SCHEMA s2;
CREATE TABLE s2.t(v int);
INSERT INTO s2.t VALUES (2);
});

my $a = $ss->psql_session();
my $b = $ss->psql_session();

is(backend_pid($a), $pooled_pid, 'session A is on the pooled backend');
is(backend_pid($b), $pooled_pid, 'session B is on the pooled backend');

$a->query_safe('SET search_path = s1');
$b->query_safe('SET search_path = s2');

$a->query_safe('PREPARE p AS SELECT v FROM t');
$b->query_safe('PREPARE p AS SELECT v FROM t');
pass('both sessions prepared the same statement name');

# Alternate several times: each EXECUTE hands the backend over, and the plan
# each session runs has to be the one planned under its own search path.
my $a_ok = 0;
my $b_ok = 0;

foreach my $i (1 .. 5)
{
	$a_ok++ if $a->query_safe('EXECUTE p') eq '1';
	$b_ok++ if $b->query_safe('EXECUTE p') eq '2';
}

is($a_ok, 5, 'session A always ran its own statement');
is($b_ok, 5, 'session B always ran its own statement');

# The view shows the session that asks, and only it.
is(prepared_count($a), '1', 'session A sees one prepared statement');
is(prepared_count($b), '1', 'session B sees one prepared statement');

# DEALLOCATE empties only the session that issued it.
$a->query_safe('DEALLOCATE p');
is(prepared_count($a), '0', "DEALLOCATE removed session A's statement");
is(prepared_count($b), '1', 'session B still has its own');
is($b->query_safe('EXECUTE p'), '2', 'and can still run it');

# DEALLOCATE ALL likewise.  (DISCARD ALL would do this too, but it also resets
# a session-private GUC, which dedicates the backend and ends the co-location
# the rest of this file needs.)
$b->query_safe('DEALLOCATE ALL');
is(prepared_count($b), '0', "DEALLOCATE ALL removed session B's statement");
is(prepared_count($a), '0', 'and session A is unaffected');

$a->quit;
$b->quit;

#
# 3. A plan held by a parked session is invalidated by DDL that happened while
#    it was parked.  A detached session has no process to read the shared
#    invalidation queue with, so this is what the per-session queue is for.
#
$node->safe_psql(
	'postgres', q{
CREATE TABLE base(i int);
INSERT INTO base VALUES (1);
CREATE VIEW v AS SELECT i FROM base;
});

my $holder = $ss->psql_session();
is(backend_pid($holder), $pooled_pid,
	'the holding session is on the pooled backend');

$holder->query_safe('PREPARE pinv AS SELECT i FROM v');
is($holder->query_safe('EXECUTE pinv'), '1', 'the plan is built and runs');

# The holder is idle from here; its backend goes back to the pool and serves
# this DDL, which must still reach the plan the holder is keeping.
$node->safe_psql('postgres',
	'CREATE OR REPLACE VIEW v AS SELECT i * 100 AS i FROM base');

is($holder->query_safe('EXECUTE pinv'), '100',
	'the parked session replanned against the new definition');

# A relation the plan depends on can also go away entirely.
$node->safe_psql('postgres', 'DROP VIEW v');

query_fails(
	$holder,
	'EXECUTE pinv',
	'EXECUTE fails once the view is gone',
	qr/relation "v" does not exist/,
	'and reports that, rather than reading a dropped relation');
$holder->quit;

#
# 4. Many clients in a row on one backend: each starts with an empty table of
#    prepared statements, prepares under a name every one of them uses, and the
#    backend keeps serving them.
#
my $sessions = 30;
my $reused = 0;
my $clean = 0;

foreach my $i (1 .. $sessions)
{
	my ($stdout, $stderr) = ('', '');

	$node->psql(
		'postgres',
		"SELECT pg_backend_pid(), count(*) FROM pg_prepared_statements",
		stdout => \$stdout,
		stderr => \$stderr);
	chomp $stdout;

	my ($pid, $count) = split(/\|/, $stdout);
	$reused++ if defined $pid && $pid eq $pooled_pid;
	$clean++ if defined $count && $count eq '0' && $stderr eq '';

	# Prepare under the same name every time: if the table were shared, the
	# second client onwards would fail here.
	$node->safe_psql('postgres',
		"PREPARE reused_name AS SELECT $i; SELECT count(*) FROM pg_prepared_statements");
}

is($reused, $sessions, "all $sessions clients were served by the same backend");
is($clean, $sessions,
	"all $sessions clients started with no prepared statements");

#
# 5. A prepared statement must be replanned after a search_path change on
#    another backend, even if that backend gives the session the path
#    generation the statement was planned under.
#
#    Generations can't be read from SQL, so prepare 100 statements under
#    consecutive generations, move the session, and bump its generation
#    into that range.  32MB of Shared Server memory: the default 1MB may not
#    hold 100 plans, and running out dedicates the backend.
#
$node->append_conf(
	'postgresql.conf', q{
polar_ss_backend_max_count = 6
polar_ss_shared_memory_size = 32MB
});
$node->restart;

$ss->create_path_tables('a', 'b');

# Both backends count from about 1, so $bumps lands mid-range.
my $statements = 100;
my $bumps = 50;

my $s = $ss->psql_session();

# Prepare under path a, one generation per statement.  The transaction pins
# the session to the first backend.
$s->query_safe('BEGIN');
my $first_pid = backend_pid($s);

run_per_path_generation($s, 'a', $statements,
	sub { "PREPARE p$_[0] AS SELECT v FROM t" });

is($s->query_safe("EXECUTE p$statements"),
	'a', 'the statements read a.t on the first backend');
$s->query_safe('COMMIT');

# Hold the first backend so the next transaction runs elsewhere.
my ($blocker, @clients) = $ss->hold_backend($first_pid);
is(backend_pid($blocker), $first_pid, 'the first backend is held');

# Set path b on the second backend and bump the generation into the range.
$s->query_safe('BEGIN');
isnt(backend_pid($s), $first_pid,
	'the session moved to a different backend');

run_per_path_generation($s, 'b', $bumps,
	sub { 'SELECT current_schemas(false)' });

# Every statement must replan for path b.
my @results = split(
	/\n/,
	$s->query_safe(
		join(";\n", map { "EXECUTE p$_" } (1 .. $statements)) . ';'));

is(scalar(@results), $statements, 'every statement returned a row');

my @stale = grep { $results[ $_ - 1 ] ne 'b' } (1 .. $statements);
is_deeply(\@stale, [],
	'no statement ran a plan built for the old search path')
  or diag('statements still reading a.t: '
	  . join(', ', map { "p$_" } @stale));

$s->query_safe('COMMIT');

$blocker->query_safe('ROLLBACK');
$_->quit foreach ($blocker, @clients);
$s->quit;

#
# 6. Nothing crashed along the way.
#
is($node->safe_psql('postgres', 'SELECT 1'), '1', 'node is still up');

$ss->assert_clean_log();

$node->stop;

done_testing();
