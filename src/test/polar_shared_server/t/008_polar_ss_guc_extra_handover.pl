# Shared Server: a session that has changed a GUC with an "extra" survives
# being handed to another backend.
#
# What is checked here:
#
#   1. the handover really happens, so that nothing below passes vacuously;
#   2. each of the three settings that have an extra survives it;
#   3. the value that arrives is the one that was set;
#   4. the settings that have no extra, which never reached the broken call,
#      still work;
#   5. the same on 'fifo', the default policy, with two clients taking turns
#      on one backend -- where the switch is between sessions rather than
#      between processes, and reaches the same code;
#   6. nothing in the log was killed by a signal.
#
# One thing section 3 does not assert.  TimeZone's extra is a "pg_tz *" into
# the process's own timezone cache, so copying its eight bytes to another
# backend copies an address that means nothing there. It will be fixed in
# the next commits.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use PolarSharedServer;

# A crash takes psql's connection with it, and with restart_after_crash off
# the instance does not come back, so every probe below is wrapped and a
# short timeout keeps a broken build failing rather than hanging.
my $probe_timeout = 30;

my $node = PostgreSQL::Test::Cluster->new('ss_guc_extra');
$node->polar_init_primary;

my $ss = PolarSharedServer->new($node);

# 'disposable' hands the session to another backend after every statement,
# which forces the handover with one client and no timing games.
$node->append_conf('postgresql.conf',
	ss_conf(
		backend_max_count => 8,
		extra => q{polar_ss_session_schedule_policy = 'disposable'}));

$node->start;

# Opens a session, runs $set, and then asks two more questions on the same
# session.  It is those later statements that pay for the SET: the handover
# happens between them, and the restore of the setting is the first thing the
# arriving backend does.
#
# Returns a hash reference with whatever the session lived long enough to
# answer, so that a dead session fails an assertion here rather than aborting
# the file.
sub set_then_move
{
	my ($set, $show) = @_;
	my %r;

	$ss->ensure_up();
	eval {
		my $session = $ss->psql_session(timeout => $probe_timeout);

		$r{before} = backend_pid($session);
		$session->query_safe($set);
		$r{after} = backend_pid($session);
		$r{shown} = $session->query_safe("SHOW $show");
		chomp $r{shown};
		$session->quit;
		1;
	} or do { $r{error} = $@ };

	return \%r;
}

#
# 1. Control: the session really is moved between statements.  Every case
#    below would pass on its own if it were not.
#
my $control = set_then_move("SELECT 'control'", 'search_path');
isnt($control->{after}, $control->{before},
	'control: consecutive statements of one session run on different backends');

#
# 2 and 3.  The three settings that have an extra.  Before the fix each of
#    these killed the backend on the statement after the SET.
#
my @with_extra = (
	[ 'TimeZone', "SET TimeZone = 'Europe/Moscow'", 'TimeZone', 'Europe/Moscow', 1 ],
	[ 'DateStyle', "SET DateStyle = 'ISO, DMY'", 'DateStyle', 'ISO, DMY', 0 ],
	[
		'client_encoding', "SET client_encoding = 'LATIN1'",
		'client_encoding', 'LATIN1', 0
	]);

foreach my $case (@with_extra)
{
	my ($name, $set, $show, $want, $extra_is_a_pointer) = @$case;
	my $r = set_then_move($set, $show);

	ok(defined $r->{shown}, "$name: the session survived the handover")
	  or diag("session did not answer: $r->{error}");

  SKIP:
	{
		skip "$name: the session did not survive", 2
		  unless defined $r->{shown};

		isnt($r->{after}, $r->{before},
			"$name: and the statement after the SET ran on another backend");

		if ($extra_is_a_pointer)
		{
		  TODO:
			{
				local $TODO =
				  'the extra is a process-local pointer, see the header';
				is($r->{shown}, $want, "$name: the value survived the move");
			}
		}
		else
		{
			is($r->{shown}, $want, "$name: the value survived the move");
		}
	}
}

#
# 4. Control: the settings whose extra pointer is NULL.  They took the other
#    branch of polar_session_guc_copy_extra() and never reached the broken
#    call, so they worked throughout -- which is what made the three above
#    look like a problem with those three settings rather than with the copy.
#
my @without_extra = (
	[ 'search_path', "SET search_path = 'public'", 'search_path', 'public' ],
	[ 'work_mem', "SET work_mem = '5MB'", 'work_mem', '5MB' ],
	[
		'application_name', "SET application_name = 'ss_guc_extra'",
		'application_name', 'ss_guc_extra'
	]);

foreach my $case (@without_extra)
{
	my ($name, $set, $show, $want) = @$case;
	my $r = set_then_move($set, $show);

	is($r->{shown}, $want, "$name: a setting with no extra still moves intact");
}

#
# 5. The default policy.  'fifo' moves a session only when someone else wants
#    the backend, so this pins the pool to one and has two clients take turns
#    on it.  polar_switch_to_session() runs on every turn -- the session
#    changes even though the process does not -- so the restore, and the call
#    that used to be NULL, run just as they do above.
#
$ss->ensure_up();
$node->stop;
$node->append_conf(
	'postgresql.conf', q{
polar_ss_backend_max_count = 1
polar_ss_session_schedule_policy = 'fifo'
});
$node->start;

my $fifo = eval {
	my $a = $ss->psql_session(timeout => $probe_timeout);
	my $b = $ss->psql_session(timeout => $probe_timeout);

	my %r;
	$r{a_pid} = backend_pid($a);
	$a->query_safe("SET TimeZone = 'Europe/Moscow'");

	# b takes the backend from a, and a takes it back.
	$r{b_pid} = backend_pid($b);
	$r{a_again} = backend_pid($a);
	$r{alive} = $a->query_safe("SELECT 'alive'");
	chomp $r{alive};

	$a->quit;
	$b->quit;
	\%r;
};
my $fifo_error = $@;

ok(defined $fifo && $fifo->{alive} eq 'alive',
	'fifo: a session that set TimeZone survives giving the backend up and taking it back'
) or diag("session did not answer: $fifo_error");

SKIP:
{
	skip 'fifo: the session did not survive', 1 unless defined $fifo;

	is($fifo->{b_pid}, $fifo->{a_pid},
		'fifo: both clients were served by the one backend the pool allows');
}

#
# 6. Nothing crashed along the way.  The log is read before the node is put
#    back, so a crash cannot be tidied away by ensure_up().
#
$ss->assert_clean_log();

$ss->ensure_up();
is($node->safe_psql('postgres', 'SELECT 1'), '1', 'node is still up');

$node->stop;

done_testing();
