# Shared Server: a client that needs backend-private state gets its own backend.
#
# A pooled backend serves one client after another and does not exit in
# between, so any per-client state that lives in the process rather than in
# PolarSessionContext would be inherited by the next client.  For the kinds of
# state that cannot reasonably be moved into the session, Shared Server takes
# the backend out of the pool instead: it sets
# MyProc->polar_is_backend_dedicated, and the dispatcher then never hands that
# backend to anybody else.
#
# This covers every such escalation that a TAP test on Linux can reach.  Four
# of them were missing from the port and are what this test was written for;
# the rest already worked and are pinned here because they are exactly as easy
# to lose -- the four that went missing did so by being edited out of files
# nobody re-read.
#
#   tablecmds.c              CREATE TEMP TABLE, unless ON COMMIT DROP
#   async.c                  LISTEN
#   portalcmds.c             DECLARE ... WITH HOLD
#   lock.c                   a session-level advisory lock
#   dfmgr.c                  loading a library that has _PG_init()
#   guc.c                    a custom (placeholder) variable
#   guc.c                    a GUC_SESSION_DEDICATED variable
#   guc.c                    turning polar_enable_shared_server off
#   guc.c                    a GUC named in polar_ss_dedicated_guc_names
#   guc.c                    a function from polar_ss_dedicated_extension_names
#   guc.c                    a db/user pair in polar_ss_dedicated_dbuser_names
#   polar_session_context.c  polar_ss_session_schedule_policy = dedicated
#
# The remaining ones in the tree are out of reach here, for reasons that are
# not going to change:
#
#   pg_locale.c              pg_perm_setlocale(), inside #ifdef WIN32
#   guc.c "unknown extenal variable"
#                            a non-placeholder custom GUC is defined from an
#                            extension's _PG_init(), and internal_load_library()
#                            has already dedicated the backend by then, so the
#                            POLAR_SS_NOT_DEDICATED() guard keeps this one quiet
#   guc.c "session makeDefault"
#                            an internal invariant; it carries Assert(false)
#   polar_session_context.c  four sites that fire on shared-memory exhaustion
#                            or on a memory context of the wrong type surviving
#                            a transaction.  That is what check_ss_oom is for,
#                            and a separate defect makes that suite write about
#                            175 MB of log per second.
#
# The cases that must *not* escalate -- ON COMMIT DROP temp tables,
# transaction-scoped advisory locks, VACUUM, cursors without WITH HOLD -- are
# checked as well, because dedicating on those would quietly disable pooling
# rather than break anything visible.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use PolarSharedServer;

my $node = PostgreSQL::Test::Cluster->new('ss_dedicated');
$node->polar_init_primary;

my $ss = PolarSharedServer->new($node);

$node->append_conf('postgresql.conf',
	ss_conf(
		backend_max_count => 8,
		extra => q{shared_preload_libraries = 'polar_monitor'}));

$node->start;

# Only section 12 needs polar_monitor.  Everything else reads the backend pid
# with the built-in pg_backend_pid(), which lives in the server binary and
# loads nothing.  That is deliberate: internal_load_library() dedicates a
# pooled backend as soon as it loads a library with a _PG_init() (section 7),
# so a co-location check that had to call into an extension would stop proving
# anything the day that extension gained one.  pg_backend_pid() here returns
# exactly what polar_session_backend_pid() would -- both are
# polar_get_session_id(MyProcPid) -- so nothing is lost by not depending on it.
#
# Section 12 has no such choice: it has to call a function out of a real
# library.  polar_monitor is named in shared_preload_libraries above, so the
# postmaster loads it once and every backend inherits it on file_list -- the
# _PG_init() branch of internal_load_library() is never reached in a backend,
# whether or not polar_monitor ever gains one, and section 12 measures the rule
# it means to measure.  Section 7 uses plpgsql, which is not preloaded, so the
# _PG_init() rule itself is still checked.
$node->safe_psql('postgres', 'CREATE EXTENSION polar_monitor');

#
# 0. Control: with none of these actions, consecutive clients do share one
#    backend.  Every case below says "a different backend", which would be
#    trivially true if the pool never reused one.
#
my $control = $ss->psql_session();
my $pooled_pid = backend_pid($control);
$control->quit;

my $control2 = $ss->psql_session();
is(backend_pid($control2), $pooled_pid,
	'control: a client that needs nothing private reuses the pooled backend');
$control2->quit;

# Runs $action in a fresh client and checks that its backend left the pool.
# Returns the pid the pool serves from afterwards, so the next case can assert
# co-location against it.
#
#   $log_re  the escalation the action must log
#   $check   optional sub run on the next client, to show the state is gone
sub dedicates
{
	my ($name, $action, $log_re, $check) = @_;

	my $before = $ss->log_count($log_re);

	my $doer = $ss->psql_session();
	my $doer_pid = backend_pid($doer);
	is($doer_pid, $pooled_pid, "$name: the client started on the pooled backend");

	$doer->query_safe($action);
	is($ss->wait_log_count($log_re, $before + 1), $before + 1,
		"$name: escalation logged exactly once");
	$doer->quit;

	my $next = $ss->psql_session();
	my $next_pid = backend_pid($next);
	isnt($next_pid, $doer_pid,
		"$name: the next client landed on a different backend");

	$check->($next, $name) if defined $check;
	$next->quit;

	return $next_pid;
}

# Builds a $check that runs $probe on the next client and requires it to fail
# with $probe_re.
sub must_fail
{
	my ($probe, $probe_re) = @_;

	return sub {
		my ($next, $name) = @_;

		query_fails(
			$next, $probe,
			"$name: the next client cannot reach the state",
			$probe_re, "$name: and fails for the right reason");
	};
}

#
# 1. CREATE TEMP TABLE.  The temporary schema is named pg_temp_<MyBackendId>,
#    after the process, so it cannot follow the session anywhere.
#
my $temp_re =
  qr/polar shared server set dedicated from non ONCOMMIT_DROP temp table/;

$pooled_pid = dedicates(
	'CREATE TEMP TABLE',
	'CREATE TEMP TABLE atmp(i int)',
	$temp_re,
	must_fail('SELECT * FROM pg_temp.atmp', qr/does not exist/));

#
# 2. The same table, created through search_path, without the word TEMP.
#    RangeVarGetAndCheckCreationNamespace() is what makes this a temp table, so
#    an escalation tested before that call -- which is where PolarDB 11 puts
#    it -- never fires here.
#
$pooled_pid = dedicates(
	'CREATE TABLE under search_path = pg_temp',
	'SET search_path = pg_temp, public; CREATE TABLE probe2(i int)',
	$temp_re,
	must_fail(
		'SET search_path = pg_temp, public; SELECT * FROM probe2',
		qr/relation "probe2" does not exist/));

#
# 3. ON COMMIT DROP is exempt, and has to stay exempt.  Such a table is gone
#    when the transaction ends, and a backend only changes client between
#    transactions, so there is nothing left to leak.
#
my $before_oncommit = $ss->log_count($temp_re);

my $oncommit = $ss->psql_session();
is(backend_pid($oncommit), $pooled_pid,
	'ON COMMIT DROP: the client started on the pooled backend');
$oncommit->query_safe(
	'BEGIN; CREATE TEMP TABLE dropme(i int) ON COMMIT DROP; COMMIT');
$ss->log_barrier($oncommit);
is($ss->log_count($temp_re), $before_oncommit,
	'ON COMMIT DROP temp table did not escalate');
$oncommit->quit;

my $after_oncommit = $ss->psql_session();
is(backend_pid($after_oncommit), $pooled_pid,
	'ON COMMIT DROP left the backend in the pool');
$after_oncommit->quit;

#
# 4. LISTEN.  The channel list is process state that outlives the transaction.
#
my $listen_re = qr/polar shared server set dedicated from Async_Listen/;
my $before_listen = $ss->log_count($listen_re);

my $listen = $ss->psql_session();
my $listen_pid = backend_pid($listen);
is($listen_pid, $pooled_pid, 'LISTEN: the client started on the pooled backend');

$listen->query_safe('LISTEN leaked_channel');
is($ss->wait_log_count($listen_re, $before_listen + 1), $before_listen + 1,
	'LISTEN: escalation logged exactly once');

# Still once after a second LISTEN: POLAR_SS_NOT_DEDICATED() keeps the flag,
# and the log line, from repeating.
$listen->query_safe('LISTEN another_channel');
$ss->log_barrier($listen);
is($ss->log_count($listen_re), $before_listen + 1,
	'LISTEN: the second LISTEN did not log again');
$listen->quit;

my $after_listen = $ss->psql_session();
$pooled_pid = backend_pid($after_listen);
isnt($pooled_pid, $listen_pid,
	'LISTEN: the next client landed on a different backend');
is($after_listen->query_safe('SELECT count(*) FROM pg_listening_channels()'),
	'0', 'LISTEN: the next client is subscribed to nothing');
$after_listen->quit;

#
# 5. A session-level advisory lock.  It is held until the client releases it,
#    so it is still held at handover.
#
my $adv_re =
  qr/polar shared server set dedicated from LockAcquireExtended lockmode/;
my $before_lock = $ss->log_count($adv_re);

my $lock = $ss->psql_session();
my $lock_pid = backend_pid($lock);
is($lock_pid, $pooled_pid,
	'session advisory lock: the client started on the pooled backend');

$lock->query_safe('SELECT pg_advisory_lock(4242)');
is($ss->wait_log_count($adv_re, $before_lock + 1), $before_lock + 1,
	'session advisory lock: escalation logged exactly once');
$lock->quit;

my $after_lock = $ss->psql_session();
$pooled_pid = backend_pid($after_lock);
isnt($pooled_pid, $lock_pid,
	'session advisory lock: the next client landed on a different backend');
is( $after_lock->query_safe(
		"SELECT count(*) FROM pg_locks WHERE locktype = 'advisory'"),
	'0',
	'session advisory lock: the next client holds no advisory lock');

# A transaction-scoped advisory lock goes away with its transaction, so it is
# gone before the handover and must not cost the client its pooled backend.
my $before_xact = $ss->log_count($adv_re);
$after_lock->query_safe('BEGIN; SELECT pg_advisory_xact_lock(4243); COMMIT');
$ss->log_barrier($after_lock);
is($ss->log_count($adv_re), $before_xact, 'xact advisory lock did not escalate');
$after_lock->quit;

my $after_xact = $ss->psql_session();
is(backend_pid($after_xact), $pooled_pid,
	'xact advisory lock left the backend in the pool');
$after_xact->quit;

# VACUUM takes session-level locks on the table and on its TOAST table, but
# releases them before the command ends, so it must not escalate either.
my $before_vacuum = $ss->log_count($adv_re);

my $vacuum = $ss->psql_session();
is(backend_pid($vacuum), $pooled_pid,
	'VACUUM: the client started on the pooled backend');
$vacuum->query_safe('CREATE TABLE vac_probe(i int, t text)');
$vacuum->query_safe('VACUUM vac_probe');
$ss->log_barrier($vacuum);
is($ss->log_count($adv_re), $before_vacuum, 'VACUUM did not escalate');
$vacuum->quit;

my $after_vacuum = $ss->psql_session();
is(backend_pid($after_vacuum), $pooled_pid,
	'VACUUM left the backend in the pool');
$after_vacuum->quit;

#
# 6. DECLARE ... WITH HOLD.  The cursor outlives the transaction that made it.
#
my $hold_re = qr/polar shared server set dedicated from PerformCursorOpen/;
my $before_hold = $ss->log_count($hold_re);

my $hold = $ss->psql_session();
my $hold_pid = backend_pid($hold);
is($hold_pid, $pooled_pid,
	'WITH HOLD cursor: the client started on the pooled backend');

$hold->query_safe('BEGIN; DECLARE c CURSOR WITH HOLD FOR SELECT 1; COMMIT');
is($ss->wait_log_count($hold_re, $before_hold + 1), $before_hold + 1,
	'WITH HOLD cursor: escalation logged exactly once');
$hold->quit;

my $after_hold = $ss->psql_session();
$pooled_pid = backend_pid($after_hold);
isnt($pooled_pid, $hold_pid,
	'WITH HOLD cursor: the next client landed on a different backend');
is($after_hold->query_safe('SELECT count(*) FROM pg_cursors'),
	'0', 'WITH HOLD cursor: the next client has no cursors');
$after_hold->quit;

# A cursor without WITH HOLD dies with its transaction and must not escalate.
my $before_nohold = $ss->log_count($hold_re);

my $nohold = $ss->psql_session();
is(backend_pid($nohold), $pooled_pid,
	'plain cursor: the client started on the pooled backend');
$nohold->query_safe('BEGIN; DECLARE c_nohold CURSOR FOR SELECT 1; COMMIT');
$ss->log_barrier($nohold);
is($ss->log_count($hold_re), $before_nohold, 'plain cursor did not escalate');
$nohold->quit;

my $after_nohold = $ss->psql_session();
is(backend_pid($after_nohold), $pooled_pid,
	'plain cursor left the backend in the pool');
$after_nohold->quit;

#
# The four above are the ones this test was written for.  The rest already
# worked; they are pinned so that they cannot be lost the same way.
#

#
# 7. Loading a library that has a _PG_init().  Whatever that function sets up
#    is per-process C state the extension believes belongs to one client.  The
#    first plpgsql function in a backend loads plpgsql.so, which has one.
#
#    DO does not run plpgsql code directly, it
#    calls the language's inline handler, plpgsql_inline_handler, and that is
#    an ordinary C function declared LANGUAGE c AS 'MODULE_PATHNAME' in
#    plpgsql--1.0.sql, so its probin is $libdir/plpgsql.  Resolving its address
#    goes fmgr_info_C_lang() -> load_external_function() ->
#    internal_load_library(), which dlopen()s the file, finds _PG_init() with
#    dlsym(), and escalates before calling it (dfmgr.c:364-371).
#
$pooled_pid = dedicates(
	'library with _PG_init()',
	'DO $$ BEGIN NULL; END $$',
	qr/polar shared server set dedicated from internal_load_library/);

#
# 8. A custom (placeholder) variable.  It is appended to the process-wide
#    guc_variables array, where the next client would find it.
#
$pooled_pid = dedicates(
	'custom variable',
	'SET ss_probe.answer = 42',
	qr/polar shared server set dedicated from custom variable 'ss_probe\.answer'/,
	must_fail(
		'SHOW ss_probe.answer',
		qr/unrecognized configuration parameter "ss_probe\.answer"/));

#
# 9. A GUC_SESSION_DEDICATED variable.  Four carry that flag; temp_tablespaces
#    is the one that is USERSET and harmless to set.
#
$pooled_pid = dedicates(
	'session private guc',
	"SET temp_tablespaces = ''",
	qr/polar shared server set dedicated from session private guc 'temp_tablespaces'/);

#
# 10. Turning polar_enable_shared_server off inside a session.  The client has
#     just asked not to be pooled.
#
$pooled_pid = dedicates(
	'polar_enable_shared_server = off',
	'SET polar_enable_shared_server = off',
	qr/polar shared server set dedicated from disable 'polar_enable_shared_server'/);

#
# The last three are driven by SIGHUP settings that change what the whole
# instance does, so they run at the end, each one after the checks that would
# be disturbed by it.
#

#
# 11. A GUC named in polar_ss_dedicated_guc_names.  Such a GUC is deliberately
#     not carried across a handover, so setting it costs the pool the backend.
#
$ss->reload_setting('polar_ss_dedicated_guc_names', 'statement_timeout');

$pooled_pid = dedicates(
	'guc in polar_ss_dedicated_guc_names',
	"SET statement_timeout = '5s'",
	qr/polar shared server set dedicated from dedicated guc 'statement_timeout'/);

#
# 12. A function from a library named in polar_ss_dedicated_extension_names.
#     This is the one case that needs a real extension, and it needs one that
#     does not dedicate on load: load_external_function() runs before the list
#     is consulted, so a library that escalated on load would mask the rule
#     being checked here.  Preloading polar_monitor is what rules that out.
#
my $ext_re =
  qr/polar shared server set dedicated from dedicated extention '\$libdir\/polar_monitor'/;
$ss->reload_setting('polar_ss_dedicated_extension_names',
	q{"$libdir/polar_monitor"});

# The check sits behind fmgr's per-process cache of C function addresses, so it
# runs only for the first client to resolve a given function in a given backend
# -- a miss, never a hit.  This backend has resolved no polar_monitor function
# yet, because nothing before this point calls one, so the call below is a miss
# and the rule fires.  Read the pid after the call rather than before it, for
# the same reason: it must be the first thing this client does.
my $before_ext = $ss->log_count($ext_re);
my $ext_a = $ss->psql_session();

$ext_a->query_safe('SELECT polar_node_type()');
is($ss->wait_log_count($ext_re, $before_ext + 1), $before_ext + 1,
	'dedicated extension: escalation logged on first call');

my $ext_a_pid = backend_pid($ext_a);
$ext_a->quit;

my $ext_b = $ss->psql_session();
isnt(backend_pid($ext_b), $ext_a_pid,
	'dedicated extension: the next client landed on a different backend');
$ext_b->quit;

#
# 13. polar_ss_session_schedule_policy = dedicated, which dedicates every
#     session as it is switched in.  This is the configuration the
#     check_ss_dedicated regression run uses.
#
my $policy_re =
  qr/polar shared server set dedicated from polar_ss_session_schedule_policy/;
$ss->reload_setting('polar_ss_session_schedule_policy', 'dedicated');

my $before_policy = $ss->log_count($policy_re);
my $pol_a = $ss->psql_session();
my $pol_a_pid = backend_pid($pol_a);
is($ss->wait_log_count($policy_re, $before_policy + 1), $before_policy + 1,
	'schedule policy dedicated: escalation logged');
$pol_a->quit;

my $pol_b = $ss->psql_session();
isnt(backend_pid($pol_b), $pol_a_pid,
	'schedule policy dedicated: the next client landed on a different backend');
$pol_b->quit;

#
# 14. A db/user pair in polar_ss_dedicated_dbuser_names.  This one is decided
#     by the dispatcher rather than by the backend, so it makes the session
#     private instead of setting the backend flag, and no later case could see
#     any of the others fire.
#
my $dbuser_re =
  qr{polar shared server set dedicated from dedicated dbuser 'postgres/\*'};
$ss->reload_setting('polar_ss_dedicated_dbuser_names', q{"postgres/*"});

# Counted after the reload: reload_setting() connects to read the new value
# back, and that connection already matches.
my $before_dbuser = $ss->log_count($dbuser_re);

my $dbu_a = $ss->psql_session();
my $dbu_a_pid = backend_pid($dbu_a);
is($ss->wait_log_count($dbuser_re, $before_dbuser + 1), $before_dbuser + 1,
	'dedicated dbuser: the dispatcher logged the match');
$dbu_a->quit;

my $dbu_b = $ss->psql_session();
isnt(backend_pid($dbu_b), $dbu_a_pid,
	'dedicated dbuser: the next client landed on a different backend');
$dbu_b->quit;

#
# 15. Nothing crashed along the way.
#
is($node->safe_psql('postgres', 'SELECT 1'), '1', 'node is still up');
$ss->assert_clean_log();

$node->stop;

done_testing();
