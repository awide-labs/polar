# "postgres -C <guc>" against a shared-storage node.
use strict;
use warnings;
use POSIX qw(:sys_wait_h);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');

# Sets up a primary over localfs shared storage (initdb + polar-initdb.sh),
# including pg_control on the share.  The server is not started: every -C run
# below exercises a standalone postmaster startup, exactly like a user asking
# for a GUC value while the primary is up.
$node->polar_init_primary;

# Runs "postgres -D datadir -C guc" without starting the server; returns
# (exit_status, stdout, stderr), mapping signal deaths to 128+signo.
sub run_show_guc
{
	my ($guc) = @_;
	my ($stdout, $stderr) = ('', '');

	IPC::Run::run [ 'postgres', '-D', $node->data_dir, '-C', $guc ],
	  '>', \$stdout, '2>', \$stderr;

	return (
		WIFEXITED($?) ? WEXITSTATUS($?) : 128 + WTERMSIG($?),
		$stdout, $stderr);
}

# Ordinary GUC: the value is printed before shared storage would be touched,
# so the VFS plugin must not even be loaded, let alone mounted.
{
	my ($rc, $out, $err) = run_show_guc('max_connections');
	is($rc, 0, '-C max_connections exits successfully')
	  or diag("stderr:\n$err");
	like($out, qr/^\d+\s*$/, '-C max_connections prints a value')
	  or diag("stderr:\n$err");
	unlike($err, qr/polar_vfs loaded/,
		'-C max_connections does not load the VFS plugin');
	unlike($err, qr/Database will be in /,
		'-C max_connections does not mount shared storage');
}

# Runtime-computed GUCs: the value is only valid after ReadControlFile() has
# read pg_control from shared storage, so a printed value means the postmaster
# mounted it instead of skipping the mount.  Log messages below FATAL are
# silenced for these, including the ones about mounting shared storage.
for my $guc ('shared_memory_size', 'shared_memory_size_in_huge_pages')
{
	my ($rc, $out, $err) = run_show_guc($guc);
	is($rc, 0, "-C $guc exits successfully") or diag("stderr:\n$err");
	like($out, qr/^\d+\s*$/, "-C $guc prints a value");
	is($err, '', "-C $guc prints no log messages");
}

done_testing();
