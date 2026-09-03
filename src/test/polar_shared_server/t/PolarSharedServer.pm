# PolarSharedServer.pm
#	  Helpers shared by the Shared Server TAP tests.
package PolarSharedServer;

use strict;
use warnings;

use Exporter 'import';
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

our @EXPORT = qw(
  backend_pid
  query_fails
  ss_conf
);

sub new
{
	my ($class, $node) = @_;

	return bless { node => $node }, $class;
}

# The settings every file in this suite runs with.
#
# pool_min_size 1 keeps one backend alive to be reused, which is what makes
# the co-location assertions meaningful, and the two timeouts at 0 stop it
# from being retired out from under them.
#
# backend_max_count is the one value that is not the same everywhere, so it
# has to be passed.  1 pins every client to a single backend.  Anything a test
# does that dedicates a backend needs more than 1: the dedicated backend
# leaves the pool, and the next client needs a new one to arrive.
#
# restart_after_crash is off so that a backend that dies takes the instance
# with it.  Otherwise the rest of a file would go on passing against a server
# that had quietly restarted underneath it.
#
# "extra" is appended as one further line, for the few tests that need one.
sub ss_conf
{
	my (%opt) = @_;

	my $conf = <<"SS_CONF";

polar_enable_shm_aset = true
polar_enable_shared_server = true
polar_ss_dispatcher_count = 1
polar_ss_backend_max_count = $opt{backend_max_count}
polar_ss_backend_pool_min_size = 1
polar_ss_backend_idle_timeout = 0
polar_ss_backend_keepalive_timeout = 0
restart_after_crash = off
SS_CONF

	$conf .= "$opt{extra}\n" if exists $opt{extra};

	return $conf;
}

# Opens a psql session against this node.
#
# Every client in this suite wants the same two things: the postgres database,
# and on_error_stop off, so that a statement meant to fail is an assertion
# rather than the end of the file.  Anything else -- another database, a
# different role, a timeout -- is passed straight through to background_psql().
sub psql_session
{
	my ($self, %opt) = @_;
	my $dbname = 'postgres';

	$dbname = delete $opt{dbname} if exists $opt{dbname};

	return $self->{node}->background_psql($dbname, on_error_stop => 0, %opt);
}

# Returns the pid of the backend currently serving this session.
#
# The built-in pg_backend_pid() is polar_get_session_id(MyProcPid), the same
# value polar_monitor's polar_session_backend_pid() returns, and reading it
# loads no library -- loading one with a _PG_init() would dedicate the pooled
# backend and destroy the co-location every file in this suite depends on.
sub backend_pid
{
	my ($session) = @_;
	my $pid = $session->query_safe('SELECT pg_backend_pid()');

	chomp $pid;
	return $pid;
}

# Runs a statement that is meant to fail, and checks that it did.
#
# A background psql accumulates stderr across statements, so the buffer has to
# be emptied before the statement and again after it, or the next test to look
# at it reads an error left by some earlier one.  That bookkeeping is why this
# is a helper: getting it wrong leaves a check that passes without looking at
# anything.
#
# $pattern and $pattern_name are optional; without them only the failure
# itself is asserted.
sub query_fails
{
	my ($session, $sql, $name, $pattern, $pattern_name) = @_;

	$session->{stderr} = '';
	my (undef, $failed) = $session->query($sql);

	ok($failed, $name);
	like($session->{stderr}, $pattern, $pattern_name) if defined $pattern;
	$session->{stderr} = '';

	return;
}

# How many times $pattern appears in everything this node has logged.
#
# polar_init_primary runs a logging collector, so most of what a backend
# writes -- the dedication messages this suite reads, among them -- lands in
# the collector's files under the data directory rather than in
# $node->logfile, where the postmaster's own output goes.  Both are read: a
# caller should not have to know which half its message came out of.
sub log_count
{
	my ($self, $pattern) = @_;
	my $node = $self->{node};
	my $n = 0;

	foreach my $file ($node->logfile, sort glob($node->data_dir . '/log/*.log'))
	{
		next unless -f $file;
		my $text = PostgreSQL::Test::Utils::slurp_file($file);
		$n += () = $text =~ /$pattern/g;
	}
	return $n;
}

# The check every file in this suite ends with: nothing in the log says a
# backend died.  Further [$pattern, $name] pairs are checked the same way, for
# the per-file resource limits a test can exhaust.
sub assert_clean_log
{
	my ($self, @extra) = @_;

	is($self->log_count(qr/FailedAssertion/),
		0, 'no assertion failure in the log');
	is($self->log_count(qr/was terminated by signal/),
		0, 'no backend was killed by a signal');

	foreach my $check (@extra)
	{
		my ($pattern, $name) = @$check;
		is($self->log_count($pattern), 0, $name);
	}
	return;
}

# How long the wait_* helpers poll before giving up and letting the caller's
# assertion fail: a hundred tries a tenth of a second apart, ten seconds.
my $poll_tries = 100;
my $poll_delay = 100_000;

# Serial number for the markers log_barrier() writes.  One test file is one
# process, so it only has to be unique within that.
my $barrier_seq = 0;

# The collector writes what the backend sent down a pipe, so a line can be a
# moment late.  Wait for it rather than sampling once.
sub wait_log_count
{
	my ($self, $pattern, $want) = @_;
	my $n = 0;

	foreach my $i (1 .. $poll_tries)
	{
		$n = $self->log_count($pattern);
		return $n if $n >= $want;
		usleep($poll_delay);
	}
	return $n;
}

# Ordering barrier for the "must not escalate" checks.  The given session logs
# a marker of its own; once that marker has arrived, anything the same backend
# logged earlier has arrived too, so a count taken afterwards cannot be short
# by a line that is merely late.  A failing statement is used because it logs
# from a pooled backend without dedicating it, which LISTEN or a plpgsql DO
# block would not.
sub log_barrier
{
	my ($self, $session) = @_;
	my $marker = 'ss_log_barrier_' . ++$barrier_seq;

	$session->{stderr} = '';
	$session->query("SELECT $marker");
	$session->{stderr} = '';
	$self->wait_log_count(qr/\Q$marker\E/, 1);
	return;
}

# Applies a SIGHUP setting and waits until a new session reports it, so that
# whatever runs next cannot run against the old value.  $value is given as
# SHOW prints it and quoted here for the file, which suits the string and enum
# settings this is used for; one with units would print differently.
sub reload_setting
{
	my ($self, $name, $value) = @_;
	my $node = $self->{node};

	(my $quoted = $value) =~ s/'/''/g;
	$node->append_conf('postgresql.conf', "$name = '$quoted'\n");
	$node->reload;

	is($self->wait_for("SHOW $name", $value),
		$value, "reload: $name = '$value'");
	return;
}

# Polls $sql until it answers $want, and returns whatever it last answered so
# that the caller can assert on it.  Disconnect cleanup is asynchronous -- the
# dispatcher may still be fetching a backend to run it on -- so a test that
# sampled once would be racing it.
sub wait_for
{
	my ($self, $sql, $want) = @_;
	my $got = '';

	foreach my $i (1 .. $poll_tries)
	{
		$got = $self->{node}->safe_psql('postgres', $sql);
		return $got if $got eq $want;
		usleep($poll_delay);
	}
	return $got;
}

1;
