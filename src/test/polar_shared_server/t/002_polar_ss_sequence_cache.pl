# Shared Server: the sequence cache belongs to the session, not to the backend.
#
# A pooled backend serves one client after another and does not exit in
# between.  While seqhashtab and last_used_seq were plain process-wide
# statics, the SeqTableData entry a departing client left behind was still
# there for the next one:
#
#   * currval()/lastval() returned a value the new session never obtained, and
#
#   * lock_and_open_sequence() compares that entry's lxid against a
#     session-scoped one, so the two could match by accident, LockRelationOid()
#     was skipped, and relation_open(relid, NoLock) ran with no lock held --
#     an assertion failure in a cassert build, a silent unlocked read in a
#     production one.
#
# The pool is pinned to a single backend so that every client is guaranteed to
# land on the same one; the tests assert that co-location rather than assuming
# it, because they would pass vacuously if the clients were spread out.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use PolarSharedServer;

my $node = PostgreSQL::Test::Cluster->new('ss_seq');
$node->polar_init_primary;

my $ss = PolarSharedServer->new($node);

$node->append_conf('postgresql.conf', ss_conf(backend_max_count => 1));

$node->start;

$node->safe_psql('postgres', 'CREATE SEQUENCE sq');

my $not_defined = qr/currval of sequence "sq" is not yet defined in this session/;
my $no_lastval  = qr/lastval is not yet defined in this session/;

#
# 1. A client must not inherit the sequence state of the client that used the
#    backend before it.  This is the case that took the backend down.
#
my $first = $ss->psql_session();
my $pooled_pid = backend_pid($first);

is($first->query_safe("SELECT nextval('sq')"),
	'1', 'first client gets nextval 1');
$first->quit;

my $second = $ss->psql_session();
is(backend_pid($second), $pooled_pid,
	'second client landed on the same pooled backend');

query_fails(
	$second,
	"SELECT currval('sq')",
	'currval() fails in a session that never called nextval()',
	$not_defined,
	'currval() reports the right error');

query_fails(
	$second,
	'SELECT lastval()',
	'lastval() fails in a session that never called nextval()',
	$no_lastval,
	'lastval() reports the right error');

is($node->safe_psql('postgres', 'SELECT 1'), '1',
	'backend survived currval() on a sequence a previous client used');
$second->quit;

#
# 2. Two clients alternating on one backend each keep their own values.  The
#    backend only changes client between transactions, so interleaving the two
#    is what forces the handover.
#
my $a = $ss->psql_session();
my $b = $ss->psql_session();

is(backend_pid($a), $pooled_pid, 'session A is on the pooled backend');
is(backend_pid($b), $pooled_pid, 'session B is on the pooled backend');

my $val_a = $a->query_safe("SELECT nextval('sq')");
my $val_b = $b->query_safe("SELECT nextval('sq')");
chomp($val_a, $val_b);
isnt($val_a, $val_b, 'the two sessions drew different values');

is($a->query_safe("SELECT currval('sq')"),
	$val_a, 'session A keeps its own currval after B used the sequence');
is($b->query_safe("SELECT currval('sq')"),
	$val_b, 'session B keeps its own currval');
is($a->query_safe('SELECT lastval()'),
	$val_a, 'session A keeps its own lastval');
is($b->query_safe('SELECT lastval()'),
	$val_b, 'session B keeps its own lastval');

#
# 3. DISCARD empties only the session that issued it.
#
$a->query_safe('DISCARD SEQUENCES');

query_fails(
	$a,
	"SELECT currval('sq')",
	'DISCARD SEQUENCES cleared session A',
	$not_defined,
	'session A reports the right error');

is($b->query_safe("SELECT currval('sq')"),
	$val_b, 'DISCARD SEQUENCES in session A left session B untouched');

# setval() belongs to the session that called it, too.
$b->query_safe("SELECT setval('sq', 1000)");
is($b->query_safe("SELECT currval('sq')"),
	'1000', 'setval() sets currval in the calling session');

query_fails($a, "SELECT currval('sq')",
	"session A did not pick up session B's setval()");

$a->quit;
$b->quit;

#
# 4. Many clients in a row on one backend: each starts with no sequence state,
#    and the backend keeps serving them.
#
my $sessions = 30;
my $reused = 0;
my $clean = 0;

for my $i (1 .. $sessions)
{
	my ($stdout, $stderr) = ('', '');

	$node->psql(
		'postgres',
		"SELECT pg_backend_pid()",
		stdout => \$stdout,
		stderr => \$stderr);
	chomp $stdout;
	$reused++ if $stdout eq $pooled_pid;

	$node->safe_psql('postgres', "SELECT nextval('sq')");

	($stdout, $stderr) = ('', '');
	$node->psql(
		'postgres',
		"SELECT currval('sq')",
		stdout => \$stdout,
		stderr => \$stderr);
	$clean++ if $stderr =~ $not_defined;
}

is($reused, $sessions, "all $sessions clients were served by the same backend");
is($clean, $sessions,
	"all $sessions clients started without the previous client's currval");

#
# 5. Nothing crashed along the way.
#
is($node->safe_psql('postgres', 'SELECT 1'), '1', 'node is still up');

$ss->assert_clean_log();

$node->stop;

done_testing();
