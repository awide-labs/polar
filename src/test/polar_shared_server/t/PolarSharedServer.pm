# PolarSharedServer.pm
#	  Helpers shared by the Shared Server TAP tests.
package PolarSharedServer;

use strict;
use warnings;

use Exporter 'import';
use PostgreSQL::Test::Utils;
use Test::More;

our @EXPORT = qw(
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

1;
