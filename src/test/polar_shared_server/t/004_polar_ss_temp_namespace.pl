# Shared Server: the temporary schema and the search path belong to the
# session, not to the backend.
#
# CREATE TEMP TABLE takes the backend out of the pool, so that path cannot
# hand a temporary *relation* to another client.  Nothing takes it
# out of the pool for the other ways into a temporary schema -- a temporary
# function or type, or a table that is ON COMMIT DROP -- and those are what
# most of this file uses, because they are what still shares a backend.
#
# The pool is arranged so that clients land on one backend and the tests
# assert that co-location rather than assuming it: they would pass vacuously
# if the clients were spread out.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

use FindBin;
use lib $FindBin::RealBin;

use PolarSharedServer;

my $node = PostgreSQL::Test::Cluster->new('ss_temp');
$node->polar_init_primary;

my $ss = PolarSharedServer->new($node);

$node->append_conf('postgresql.conf', ss_conf(backend_max_count => 16));

$node->start;

sub temp_schema
{
	my ($session) = @_;
	my $oid = $session->query_safe('SELECT pg_my_temp_schema()');
	chomp $oid;
	return $oid;
}

# How many temporary objects of this name exist, in anybody's temp schema.
sub temp_function_count
{
	my ($name) = @_;
	return sprintf(
		q{SELECT count(*) FROM pg_proc p JOIN pg_namespace n }
		  . q{ON n.oid = p.pronamespace }
		  . q{WHERE n.nspname LIKE 'pg\_temp%%' AND p.proname = '%s'},
		$name);
}

sub temp_relation_count
{
	my ($name) = @_;
	return sprintf(
		q{SELECT count(*) FROM pg_class c JOIN pg_namespace n }
		  . q{ON n.oid = c.relnamespace }
		  . q{WHERE n.nspname LIKE 'pg\_temp%%' AND c.relname = '%s'},
		$name);
}

# Creates a temporary function whose body is a private string, on a session
# that stays pooled: CREATE FUNCTION pg_temp.f() sets the session's temporary
# schema up without dedicating the backend, which CREATE TEMP TABLE would.
sub make_temp_function
{
	my ($session, $name, $body) = @_;
	$session->query_safe(
		"CREATE FUNCTION pg_temp.$name() RETURNS text AS "
		  . "\$\$ SELECT '$body'::text \$\$ LANGUAGE sql");
	return;
}

# The first client on a fresh pool is not necessarily the one the pool settles
# on, so open one and drop it before asking.
my $warmup = $ss->psql_session();
$warmup->query_safe('SELECT 1');
$warmup->quit;

my $pooled_pid = $ss->pooled_backend_pid();

#
# 1. Owning a temporary schema and being in the pool are mutually exclusive.
#
#    CREATE FUNCTION pg_temp.f() is the case that matters here.  CREATE TEMP
#    TABLE is taken out of the pool earlier, by DefineRelation();
#    nothing covers a temporary function, so before this rule existed such a
#    client stayed pooled, was parked with objects in its schema, and lost them
#    to the next client that created a temporary table on that backend.
#
my $a = $ss->psql_session();
is(backend_pid($a), $pooled_pid, 'session A starts on the pooled backend');

make_temp_function($a, 'secret', 'session-A-private-data');
my $a_schema = temp_schema($a);
isnt($a_schema, '0', 'session A now owns a temporary schema');

my $b = $ss->psql_session();
isnt(backend_pid($b), backend_pid($a),
	'a client owning a temporary schema left the pool');

# B sets a temporary schema up on the backend A was pooled on.  Under the old
# naming that is A's schema, and this is the statement that used to empty it.
$b->query_safe('BEGIN');
$b->query_safe('CREATE TEMP TABLE oc(i int) ON COMMIT DROP');
$b->query_safe('COMMIT');

is( $a->query_safe('SELECT pg_temp.secret()'),
	'session-A-private-data',
	'session A still has its own temporary function');
is(temp_schema($a), $a_schema, 'session A still owns the same schema');

# B owns nothing between transactions, so it gave the schema back and is still
# poolable -- this is what keeps the rule above from disabling pooling outright.
is(temp_schema($b), '0',
	'a client with an empty temporary schema released it at commit');

my $c = $ss->psql_session();
is(backend_pid($c), backend_pid($b),
	'and stayed in the pool: the next client landed on it');

# C is a fresh client on a backend that has served a temporary schema.  It must
# have none of its own, and pg_temp must resolve to nothing for it.
is(temp_schema($c), '0',
	'a fresh client did not inherit a temporary schema');

query_fails(
	$c,
	'SELECT pg_temp.secret()',
	"a fresh client cannot reach another client's schema",
	qr/schema "pg_temp" does not exist/,
	'pg_temp resolves to nothing in a session that has no temporary schema');

query_fails(
	$c,
	'SELECT secret()',
	"another client's temporary schema is not on this one's search path",
	qr/function secret\(\) does not exist/,
	'the unqualified name reports the right error');
$c->quit;

$b->quit;
$a->quit;

#
# 2. Two clients that each create a temporary table get separate schemas, and
#    neither can see into the other.  Creating one dedicates the backend, so
#    these two are on different backends by construction; what is checked here
#    is that the state each of them ends up with is its own.
#
my $ta = $ss->psql_session();
$ta->query_safe('CREATE TEMP TABLE atmp(v text)');
$ta->query_safe("INSERT INTO atmp VALUES ('a-rows')");
my $ta_schema = temp_schema($ta);

my $tb = $ss->psql_session();
isnt(backend_pid($tb), backend_pid($ta),
	'a client that made a temporary table kept its backend to itself');

$tb->query_safe('SET search_path = pg_temp, public');
$tb->query_safe('CREATE TABLE probe2(v text)');
my $tb_schema = temp_schema($tb);

isnt($tb_schema, $ta_schema,
	'the two clients own different temporary schemas');
is( $tb->query_safe(
		q{SELECT relnamespace = pg_my_temp_schema() FROM pg_class WHERE relname = 'probe2'}
	),
	't',
	'an unqualified CREATE under search_path = pg_temp lands in the own schema'
);

query_fails($tb, 'SELECT * FROM atmp',
	"the second client cannot see the first client's temp table");

# Large values follow the same path: each temporary table's toast table has to
# go into that session's own toast schema.
is( $ta->query_safe(
		q{SELECT n.nspname FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace}
		  . q{ WHERE c.oid = (SELECT reltoastrelid FROM pg_class WHERE oid = 'atmp'::regclass)}
	),
	'pg_toast_temp_' . $ta->query_safe(
		q{SELECT substring(nspname from 'pg_temp_(.*)') FROM pg_namespace WHERE oid = pg_my_temp_schema()}
	),
	'the first client toasts into its own temp toast schema');
isnt(
	$tb->query_safe(
		q{SELECT (SELECT relnamespace FROM pg_class}
		  . q{ WHERE oid = (SELECT reltoastrelid FROM pg_class WHERE oid = 'probe2'::regclass))::text}
	),
	$ta->query_safe(
		q{SELECT (SELECT relnamespace FROM pg_class}
		  . q{ WHERE oid = (SELECT reltoastrelid FROM pg_class WHERE oid = 'atmp'::regclass))::text}
	),
	'the two clients toast into different temp toast schemas');

$tb->quit;
$ta->quit;

is($ss->wait_for(temp_relation_count('atmp'), '0'),
	'0', 'the first client\'s temp table is gone after it disconnected');
is($ss->wait_for(temp_relation_count('probe2'), '0'),
	'0', 'the second client\'s temp table is gone after it disconnected');

#
# 3. Temporary objects are removed when their session goes away, whichever way
#    it goes away.  Both cleanup paths are exercised: the Terminate handler in
#    the backend, and the dispatcher fetching a backend for a client that
#    vanished without sending one.
#
# graceful quit
$pooled_pid = $ss->pooled_backend_pid();
my $g = $ss->psql_session();
is(backend_pid($g), $pooled_pid, 'the graceful-quit client is pooled');
make_temp_function($g, 'f_quit', 'quit');
$g->quit;
is($ss->wait_for(temp_function_count('f_quit'), '0'),
	'0', 'a graceful disconnect removes the temporary objects');

# idle at disconnect: let the session be parked with no backend first
my $i = $ss->psql_session();
make_temp_function($i, 'f_idle', 'idle');
usleep(500_000);
$i->quit;
is($ss->wait_for(temp_function_count('f_idle'), '0'),
	'0', 'disconnecting from idle removes the temporary objects');

# inside an open transaction
my $x = $ss->psql_session();
make_temp_function($x, 'f_xact', 'xact');
$x->query_safe('BEGIN');
$x->query_safe('SELECT 1');
$x->quit;
is($ss->wait_for(temp_function_count('f_xact'), '0'),
	'0', 'disconnecting inside a transaction removes the temporary objects');

# the client is killed outright, so no Terminate message is ever sent
my $k = $ss->psql_session();
make_temp_function($k, 'f_kill', 'kill');
is($node->safe_psql('postgres', temp_function_count('f_kill')),
	'1', 'the killed client did create its temporary function');
$k->{run}->kill_kill;
is($ss->wait_for(temp_function_count('f_kill'), '0'),
	'0', 'a client killed outright still has its temporary objects removed');

#
# 4. The search path resolves per session, not per backend.
#
$node->safe_psql('postgres',
	'CREATE SCHEMA sa; CREATE TABLE sa.t(i int); '
	  . 'CREATE SCHEMA sb; CREATE TABLE sb.t(i int)');

my $pa = $ss->psql_session();
my $pb = $ss->psql_session();
is(backend_pid($pa), backend_pid($pb),
	'the two search path sessions share one backend');

$pa->query_safe('SET search_path = sa');
$pb->query_safe('SET search_path = sb');

# regclass output omits the schema when the schema is on the search path, so
# ask the catalog which schema the name actually resolved in.
my $resolve = q{SELECT coalesce((SELECT n.nspname FROM pg_class c }
  . q{JOIN pg_namespace n ON n.oid = c.relnamespace }
  . q{WHERE c.oid = to_regclass('t')), 'none')};

foreach my $round (1 .. 3)
{
	is($pa->query_safe($resolve),
		'sa', "round $round: session A resolves t in its own schema");
	is($pb->query_safe($resolve),
		'sb', "round $round: session B resolves t in its own schema");
}

#
# 5. A session parked on a shared backend still hears about catalog changes
#    that alter what its search path means.  Now that the cached path lives in
#    the session, the process-wide invalidation callback can only reach
#    whichever session is loaded; the per-session queue is what reaches the
#    rest.
#
$pa->query_safe('SET search_path = news');
is($pa->query_safe($resolve), 'none', 'the schema does not exist yet');

# Keep session B busy on the backend so that A is the parked one.
$pb->query_safe('SELECT 1');
$node->safe_psql('postgres', 'CREATE SCHEMA news; CREATE TABLE news.t(i int)');
$pb->query_safe('SELECT 1');

is($pa->query_safe($resolve),
	'news', 'a parked session sees a schema created while it waited');

$pb->query_safe('SELECT 1');
$node->safe_psql('postgres', 'ALTER SCHEMA news RENAME TO olds');
$pb->query_safe('SELECT 1');

is($pa->query_safe($resolve), 'none', 'and sees the schema go away again');

$pb->quit;
$pa->quit;

#
# 6. Many clients in a row, each setting a temporary schema up on the same
#    pooled backend.  ON COMMIT DROP keeps the backend in the pool, so all of
#    them land on it.  RemoveTempRelationsCallback() is registered with
#    before_shmem_exit(), whose list holds twenty entries, so registering it
#    per session rather than per process would kill the backend on the
#    twenty-first client.
#
my $sessions = 30;
my $reused = 0;

$pooled_pid = $ss->pooled_backend_pid();

foreach my $i (1 .. $sessions)
{
	my ($stdout, $stderr) = ('', '');

	$node->psql(
		'postgres',
		'BEGIN; CREATE TEMP TABLE oc(i int) ON COMMIT DROP; COMMIT; '
		  . 'SELECT pg_backend_pid()',
		stdout => \$stdout,
		stderr => \$stderr);
	chomp $stdout;
	$reused++ if $stdout eq $pooled_pid;
}

is($reused, $sessions, "all $sessions clients were served by the same backend");
is($node->safe_psql('postgres', 'SELECT 1'),
	'1', 'the pooled backend survived them');

#
# 7. A plpgsql plan cached in a backend must not be reused by a session that
#    comes from another backend with a different search path but the same
#    path generation.
#
#    Generations can't be read from SQL, so plan 100 functions under
#    consecutive generations and bring a session whose generation is in
#    that range to them.  Two backends, so the session can only land on the
#    one with the plans.  plpgsql is preloaded: a pooled backend that loads
#    it itself becomes dedicated.
#
$node->append_conf(
	'postgresql.conf', q{
shared_preload_libraries = 'plpgsql'
polar_ss_backend_max_count = 2
});
$node->restart;

# Both backends count from about 1, so $bumps lands mid-range.
my $functions = 100;
my $bumps = 50;

$ss->create_path_tables('a', 'b');

# Called qualified: path "a" alone does not include public.
$node->safe_psql(
	'postgres',
	join(
		'',
		map {
			"CREATE FUNCTION public.f$_() RETURNS text LANGUAGE plpgsql AS "
			  . '$$ DECLARE r text; BEGIN SELECT v INTO r FROM t; RETURN r; END $$;'
			  . "\n"
		} (1 .. $functions)));

my $planner = $ss->psql_session();
my $s = $ss->psql_session();

# Plan under path b, one generation per function.  The open transaction
# pins the planner to its backend and keeps S off it.
$planner->query_safe('BEGIN');
my $plan_pid = backend_pid($planner);

my @planned = run_per_path_generation($planner, 'b', $functions,
	sub { "SELECT public.f$_[0]()" });

is(scalar(grep { $_ eq 'b' } @planned),
	$functions, 'the plans read b.t');

# S sets path a on the other backend and bumps its generation into the
# range.
$s->query_safe('BEGIN');
my $other_pid = backend_pid($s);
isnt($other_pid, $plan_pid, 'S is on the other backend');

run_per_path_generation($s, 'a', $bumps,
	sub { 'SELECT current_schemas(false)' });

$s->query_safe('COMMIT');
$planner->query_safe('COMMIT');

# Hold the other backend so S's next transaction lands on the plans.
my ($holder, @clients) = $ss->hold_backend($other_pid);
is(backend_pid($holder), $other_pid, 'the other backend is held');

# Every function must replan for path a.
$s->query_safe('BEGIN');
is(backend_pid($s), $plan_pid, 'S moved to the backend with the plans');

my @results = split(
	/\n/,
	$s->query_safe(
		join(";\n", map { "SELECT public.f$_()" } (1 .. $functions)) . ';'
	));

is(scalar(@results), $functions, 'every function returned a row');

my @stale = grep { $results[ $_ - 1 ] ne 'a' } (1 .. $functions);
is_deeply(\@stale, [],
	"no function ran a plan built for another session's search path")
  or diag('functions still reading b.t: '
	  . join(', ', map { "f$_" } @stale));

$s->query_safe('COMMIT');

$holder->query_safe('ROLLBACK');
$_->quit foreach ($holder, @clients);
$s->quit;
$planner->quit;

#
# 8. Nothing crashed or ran out of anything along the way.
#
$ss->assert_clean_log(
	[
		qr/out of before_shmem_exit slots/,
		'the process exit callback list did not overflow'
	]);

$node->stop;

done_testing();
