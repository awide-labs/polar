# Shared Server: a library preloaded at backend start takes the backend out of
# the pool, just as the same library would if it were loaded on demand.
#
# Shared Server dedicates a backend -- takes it out of the pool, so it serves
# one client and exits with it -- whenever it loads a library that has a
# _PG_init(), because such a module keeps state in the process that the next
# client would inherit.  What is checked here:
#
#   1. session_preload_libraries dedicates.
#   2. local_preload_libraries dedicates.
#   3. shared_preload_libraries does not, including when it names the same
#      file as session_preload_libraries.
#
# Each case runs two clients and looks at three things: which backend each one
# got, how many escalations were logged, and -- through the control in section
# 0 -- that the pool does reuse a backend when nothing asks it not to.
#
# plpgsql is the module throughout.  It is always built, and it has a
# _PG_init(), which is the only property the rule looks at.

use strict;
use warnings;
use File::Path qw(make_path);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use PolarSharedServer;

my $node = PostgreSQL::Test::Cluster->new('ss_preload');
$node->polar_init_primary;

my $ss = PolarSharedServer->new($node);

$node->append_conf('postgresql.conf', ss_conf(backend_max_count => 8));

# local_preload_libraries only looks in $libdir/plugins/, which no build
# creates.  A symlink is enough, and keeps the inode the same as $libdir's
# copy, so the file is not loaded twice into one process.
my $pkglibdir = $node->config_data('--pkglibdir');
my $plugin_ok = eval {
	make_path("$pkglibdir/plugins");
	symlink("$pkglibdir/plpgsql.so", "$pkglibdir/plugins/plpgsql.so")
	  || $!{EEXIST};
};

$node->start;

my $loaded_re =
  qr/polar shared server set dedicated from internal_load_library '[^']*plpgsql\.so'/;

#
# 0. Control: with nothing preloaded, consecutive clients do share one
#    backend.  Every case below says "a different backend", which would be
#    trivially true if the pool never reused one.
#
my $control = $ss->psql_session();
my $pooled_pid = backend_pid($control);
$control->quit;

my $control2 = $ss->psql_session();
is(backend_pid($control2), $pooled_pid,
	'control: a client that preloads nothing reuses the pooled backend');
$control2->quit;

# Each database below gets the setting of its own, rather than the cluster
# getting it in postgresql.conf, so that the control pool above stays a pool.
# The dispatcher keys its pools on (database, user, startup GUCs), so clients
# of these databases never share a backend with the control clients anyway.
#
# Runs two clients against $dbname and requires each to get a backend of its
# own, with one escalation logged per backend.
sub preload_dedicates
{
	my ($name, $dbname) = @_;

	my $before = $ss->log_count($loaded_re);

	my $first = $ss->psql_session(dbname => $dbname);
	my $first_pid = backend_pid($first);
	is($ss->wait_log_count($loaded_re, $before + 1),
		$before + 1, "$name: the first client's backend escalated");

	my $second = $ss->psql_session(dbname => $dbname);
	my $second_pid = backend_pid($second);
	isnt($second_pid, $first_pid,
		"$name: the second client landed on a different backend");
	is($ss->wait_log_count($loaded_re, $before + 2),
		$before + 2, "$name: and its backend escalated too");

	$first->quit;
	$second->quit;
	return;
}

#
# 1. session_preload_libraries.  Loaded by
#    process_session_preload_libraries() from InitPostgres(), with the
#    backend's own private session still current.
#
$node->safe_psql('postgres', 'CREATE DATABASE ss_session_preload');
$node->safe_psql('postgres',
	q{ALTER DATABASE ss_session_preload SET session_preload_libraries = 'plpgsql'}
);
preload_dedicates('session_preload_libraries', 'ss_session_preload');

#
# 2. local_preload_libraries.  Loaded from the same function, one call later,
#    and restricted to $libdir/plugins/.  Any user can set it, which is the
#    reason it must not be left out.
#
SKIP:
{
	skip "could not create $pkglibdir/plugins", 3 unless $plugin_ok;

	$node->safe_psql('postgres', 'CREATE DATABASE ss_local_preload');
	$node->safe_psql('postgres',
		q{ALTER DATABASE ss_local_preload SET local_preload_libraries = 'plpgsql'}
	);
	preload_dedicates('local_preload_libraries', 'ss_local_preload');
}

#
# 3. shared_preload_libraries must not dedicate, and must not start doing so
#    by way of the other two: once the postmaster has loaded the file, a
#    backend that preloads the same file finds it on file_list and never
#    reaches _PG_init() at all.
#
$node->append_conf('postgresql.conf', q{shared_preload_libraries = 'plpgsql'});
$node->restart;

my $any_re = qr/polar shared server set dedicated from internal_load_library/;
my $before_shared = $ss->log_count($any_re);

my $shared = $ss->psql_session();
my $shared_pid = backend_pid($shared);

# Proves the library really is there: without it this would fail, and on a
# backend that had loaded it itself this would have dedicated (the first
# plpgsql function in a backend is what loads plpgsql.so).
$shared->query_safe('DO $$ BEGIN NULL; END $$');
$ss->log_barrier($shared);
$shared->quit;

my $shared2 = $ss->psql_session();
is(backend_pid($shared2), $shared_pid,
	'shared_preload_libraries: consecutive clients still share one backend');
$shared2->quit;

is($ss->log_count($any_re), $before_shared,
	'shared_preload_libraries: no escalation logged');

# The same file named in session_preload_libraries is now a no-op load, so it
# must not dedicate either.
my $again = $ss->psql_session(dbname => 'ss_session_preload');
my $again_pid = backend_pid($again);
$ss->log_barrier($again);
$again->quit;

my $again2 = $ss->psql_session(dbname => 'ss_session_preload');
is(backend_pid($again2), $again_pid,
	'a session-preloaded library already loaded by the postmaster does not dedicate'
);
$again2->quit;

is($ss->log_count($any_re), $before_shared,
	'and logs no escalation either');

#
# 4. Nothing crashed along the way.
#
is($node->safe_psql('postgres', 'SELECT 1'), '1', 'node is still up');
$ss->assert_clean_log();

$node->stop;

done_testing();
