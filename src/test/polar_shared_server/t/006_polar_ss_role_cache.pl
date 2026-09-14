# Shared Server: the role membership cache in acl.c and the superuser cache in
# superuser.c belong to the session, not to the backend process.
#
# Both are consulted by every privilege check.  Once they live in the session,
# the process-wide syscache callback that clears them reaches only whichever
# session the reading backend happens to be serving, and a parked session has
# no process at all: the per-session invalidation queue is what has to reach
# the rest.
#
# So each section parks one client and changes the catalog behind its back.
# What makes that a test rather than a coincidence is the second client: it has
# to be on the same backend, and it has to run a statement between the change
# and the question, so that the backend drains the shared invalidation queue
# for a session that is not the parked one.  change_catalog() asserts that
# co-location every time, because without it the section would pass on the
# process-wide path and prove nothing.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use PolarSharedServer;

my $node = PostgreSQL::Test::Cluster->new('ss_acl');
$node->polar_init_primary;

my $ss = PolarSharedServer->new($node);

# max_count has to be more than 1 here for a reason of this file's own: the
# pooled clients connect as "pooled" while every safe_psql connects as the OS
# superuser, and the dispatcher keys its pools on database and role, so the
# two need a backend each.
$node->append_conf('postgresql.conf', ss_conf(backend_max_count => 16));

$node->start;

my $orig_dba = $node->safe_psql('postgres',
	q{SELECT pg_get_userbyid(datdba) FROM pg_database WHERE datname = 'postgres'}
);

# The pooled clients connect as "pooled".  It starts out a superuser for
# section 1 and spends the rest of the file as an ordinary role, because
# has_privs_of_role() and is_member_of_role() answer yes to everything for a
# superuser without looking at any membership at all.
#
# "grp" is what sections 1 to 4 ask about, granted and revoked as they go.
# "r1" and "g1" are for section 6, which needs a role "pooled" can SET ROLE to.
$node->safe_psql(
	'postgres', q{
CREATE ROLE pooled LOGIN SUPERUSER;
CREATE ROLE grp;
CREATE ROLE g1;
CREATE ROLE r1;
GRANT r1 TO pooled;
GRANT g1 TO r1;
});

sub pooled_psql
{
	return $ss->psql_session(extra_params => [ '-U', 'pooled' ]);
}

# MEMBER reaches is_member_of_role() and so the ROLERECURSE_MEMBERS cache;
# USAGE reaches has_privs_of_role() and so the ROLERECURSE_PRIVS one.  Both ask
# superuser_arg() before they look at any membership.
sub has_role
{
	my ($session, $role, $priv) = @_;
	my $v = $session->query_safe("SELECT pg_has_role('$role', '$priv')");
	chomp $v;
	return $v;
}

# Change the catalog from a third client and leave the parked one none the
# wiser except through its own invalidation queue.  See the note at the top of
# the file for why the busy client is checked and then used.
sub change_catalog
{
	my ($parked, $busy, $sql) = @_;

	is(backend_pid($busy), backend_pid($parked),
		"one backend still serves both clients, before: $sql");

	$node->safe_psql('postgres', $sql);
	$busy->query_safe('SELECT 1');
	return;
}

my $pa = pooled_psql();
my $pb = pooled_psql();

#
# 1. A parked session sees its role stop being a superuser.
#
#    has_privs_of_role() and is_member_of_role() both ask superuser_arg()
#    before they look at any membership, so a stale answer there is a privilege
#    check that says yes to everything.  This section runs first because it is
#    the only one that needs the cached answer to have been true: the very
#    first question A asks is asked while "pooled" really is a superuser, and
#    the answer it keeps is the one the rest of the section has to dislodge.
#
is(has_role($pa, 'grp', 'member'),
	't', 'a superuser is a member of every role');

change_catalog($pa, $pb, 'ALTER ROLE pooled NOSUPERUSER');

is(has_role($pa, 'grp', 'member'), 'f', 'a parked session sees NOSUPERUSER');

change_catalog($pa, $pb, 'ALTER ROLE pooled SUPERUSER');

is(has_role($pa, 'grp', 'member'), 't', 'and sees the privilege come back');

change_catalog($pa, $pb, 'ALTER ROLE pooled NOSUPERUSER');

is(has_role($pa, 'grp', 'member'), 'f', 'and go away again');

#
# 2. A parked session sees a role membership granted and revoked while it
#    waited.  pg_auth_members is what roles_is_member_of() walks.
#
is(has_role($pa, 'grp', 'member'), 'f', 'A is not a member of grp yet');
is(has_role($pa, 'grp', 'usage'), 'f', 'and has none of its privileges');

change_catalog($pa, $pb, 'GRANT grp TO pooled');

is(has_role($pa, 'grp', 'member'), 't', 'a parked session sees the GRANT');
is(has_role($pa, 'grp', 'usage'),
	't', 'and the privileges that came with it');

change_catalog($pa, $pb, 'REVOKE grp FROM pooled');

is(has_role($pa, 'grp', 'member'), 'f', 'and sees the REVOKE');
is(has_role($pa, 'grp', 'usage'), 'f', 'and loses the privileges again');

#
# 3. A parked session sees rolinherit change.  This is the pg_authid half of
#    the cache, and it tells the two halves apart: dropping inheritance takes
#    the privileges away and leaves the membership alone.
#
change_catalog($pa, $pb, 'GRANT grp TO pooled');
is(has_role($pa, 'grp', 'usage'), 't', 'A inherits grp again');

change_catalog($pa, $pb, 'ALTER ROLE pooled NOINHERIT');

is(has_role($pa, 'grp', 'usage'), 'f', 'a parked session sees NOINHERIT');
is(has_role($pa, 'grp', 'member'),
	't', 'which leaves the membership itself alone');

change_catalog($pa, $pb,
	'ALTER ROLE pooled INHERIT; REVOKE grp FROM pooled');
is(has_role($pa, 'grp', 'member'), 'f', 'and the state is back where it was');

#
# 4. A parked session sees the database change owner.  The owner is an implicit
#    member of pg_database_owner, so pg_database is the third catalog the cache
#    depends on.
#
is(has_role($pa, 'pg_database_owner', 'member'),
	'f', 'A does not own the database');

change_catalog($pa, $pb, 'ALTER DATABASE postgres OWNER TO pooled');

is(has_role($pa, 'pg_database_owner', 'member'),
	't', 'a parked session sees the database change owner');

change_catalog($pa, $pb, "ALTER DATABASE postgres OWNER TO $orig_dba");

is(has_role($pa, 'pg_database_owner', 'member'),
	'f', 'and sees it change back');

$pb->quit;
$pa->quit;

#
# 5. A session that filled the caches while pooled and then dedicated the
#    backend keeps working.
#
#    SET ROLE is one of the statements that dedicate, and dedication is what
#    decides where a recomputed list is allocated: the session's own memory
#    context while pooled, TopMemoryContext once dedicated.  So this walks a
#    list allocated under one into a recompute that frees it from under the
#    other, in both directions.
#
my $pc = pooled_psql();
is(has_role($pc, 'grp', 'member'), 'f', 'C fills its caches while pooled');
is(has_role($pc, 'grp', 'usage'), 'f', 'both of them');

$pc->query_safe('SET ROLE r1');

is(has_role($pc, 'g1', 'member'), 't', 'r1 is a member of g1');
is(has_role($pc, 'g1', 'usage'), 't', 'and has its privileges');

$pc->query_safe('RESET ROLE');

is(has_role($pc, 'grp', 'member'), 'f', 'and pooled is still not in grp');
$pc->quit;

#
# 6. Many clients in a row, each running a privilege check that fills both
#    caches on one pooled backend.  initialize_acl() registers three syscache
#    callbacks and superuser_arg() a fourth; the per-session callback list is
#    process-wide and holds 64 entries, so registering them per session rather
#    than per process would kill the backend part-way through.
#
# Section 5 dedicated a backend and took it out of the pool, so ask which one
# the pool settles on now rather than reusing the earlier answer.
my $pooled_pid = $ss->pooled_backend_pid(extra_params => [ '-U', 'pooled' ]);

my $sessions = 40;
my $reused = 0;

foreach my $i (1 .. $sessions)
{
	my ($stdout, $stderr) = ('', '');

	$node->psql(
		'postgres',
		"SELECT pg_has_role('grp', 'member'), pg_has_role('grp', 'usage'); "
		  . 'SELECT pg_backend_pid()',
		stdout => \$stdout,
		stderr => \$stderr,
		extra_params => [ '-U', 'pooled' ]);

	my @lines = split(/\n/, $stdout);
	$reused++ if @lines && $lines[-1] eq $pooled_pid;
}

is($reused, $sessions, "all $sessions clients were served by the same backend");
is($node->safe_psql('postgres', 'SELECT 1'),
	'1', 'the pooled backend survived them');

#
# 7. Nothing crashed or ran out of anything along the way.
#
$ss->assert_clean_log(
	[
		qr/out of syscache_callback_list slots/,
		'the syscache callback list did not overflow'
	]);

$node->stop;

done_testing();
