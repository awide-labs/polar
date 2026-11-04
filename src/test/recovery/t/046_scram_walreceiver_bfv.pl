#!/usr/bin/perl
#
# 001_scram_walreceiver_bfv.pl
#
# Bug fix verification for the known walreceiver teardown heap corruption:
#
#   free(): invalid pointer
#   scram_free -> pqDropConnection -> closePGconn -> PQfinish ->
#   libpqrcv_disconnect -> WalRcvDie -> shmem_exit -> proc_exit
#
# Field facts:
#   - shared-storage replica, replication link authenticated with SCRAM
#   - fires on replica disconnect / restart / switchover -- i.e. the ordinary
#     walreceiver SIGTERM ("terminating walreceiver process due to
#     administrator command", ProcessWalRcvInterrupts path) or a broken
#     upstream socket (libpqrcv_receive path); reproducible, not
#     long-uptime-gated
#   - glibc only sees the bad free() of state->password at teardown; only
#     ASan points at the corrupting WRITE, so this test is meant to run on an
#     ASan build (see memory/asan-build-recipe).

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

$| = 1;    # autoflush so buffered TAP isn't lost if the script dies

my $db            = 'postgres';
my $repl_password = 'repropass';

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->polar_init_primary;
$primary->append_conf('postgresql.conf', "password_encryption='scram-sha-256'\n");

# the shared-storage replica; its walreceiver is the historical crash site
my $replica = PostgreSQL::Test::Cluster->new('replica');
$replica->polar_init_replica($primary);
$replica->append_conf('postgresql.conf', qq(
    restart_after_crash=true
	wal_receiver_status_interval='100ms'
	wal_retrieve_retry_interval='1s'));

$primary->start;

# resolve the real superuser (PolarDB initdb names it after the OS user) and
# give it a SCRAM password; the verifier lives in shared storage, so it is
# valid on both nodes across role flips
my $repl_user = $primary->safe_psql($db,
	"SELECT rolname FROM pg_authid WHERE rolsuper AND rolcanlogin ORDER BY 1 LIMIT 1");
chomp $repl_user;
note "superuser / replication role: $repl_user";
$primary->safe_psql($db, qq(
	SET password_encryption='scram-sha-256';
	ALTER ROLE $repl_user PASSWORD '$repl_password';));

# force SCRAM on the replication auth
rewrite_hba_to_scram($primary);
$primary->reload;

# authenticate replica's walreceiver as the SCRAM role (last conninfo line wins)
append_scram_conninfo($replica, $primary);

$primary->polar_create_slot($replica->name);
$replica->start;

note "wait walreceiver is up";
$replica->poll_query_until('postgres', qq(
    select count(pid) = 1
    from pg_stat_activity
    where backend_type = 'walreceiver'), 't');

my $walreceiver_pid = $replica->find_child('walreceiver');
note "killing walreceiver with pid $walreceiver_pid";
PostgreSQL::Test::Utils::system_or_bail('kill', '-15', $walreceiver_pid);

note "wait restart of walreceiver";
$replica->poll_query_until('postgres', qq(
    select count(pid) = 1
    from pg_stat_activity
    where backend_type = 'walreceiver' and pid != $walreceiver_pid;), 't');

my $crashed = 0;
my $crashed_at = '';
check_crash();

ok(!$crashed, 'no heap corruption across SCRAM walreceiver teardown');
diag $crashed
  ? "REPRODUCED at $crashed_at (see log tail above for the ASan report)"
  : "no corruption observed";
done_testing();

END
{
	diag "=== teardown ===";
	for my $node ($primary, $replica)
	{
		eval { $node->stop };
	}
}

# ---------- helpers ---------------------------------------------------------

sub append_scram_conninfo
{
	my ($follower, $upstream) = @_;
	$follower->append_conf('postgresql.conf',
		"primary_conninfo='host=" . $upstream->host . " port=" . $upstream->port
		  . " dbname=postgres user=$repl_user password=$repl_password"
		  . " application_name=" . $follower->name . "'");
}

sub check_crash
{
	for my $node ($primary, $replica)
	{
		my $log = eval { slurp(newest_log($node)) } // '';
		if ($log =~ /free\(\): invalid pointer|AddressSanitizer|heap-buffer-overflow|use-after-free|stack-buffer-overflow|double free/i)
		{
			$crashed  = 1;
        	$crashed_at = $node->name;
			diag "=== $crashed_at: corruption detected, log tail ===\n"
			  . tail($log, 160);
			return;
		}
	}
}

sub rewrite_hba_to_scram
{
	my ($node) = @_;
	my $hba = $node->data_dir . '/pg_hba.conf';
	open my $in, '<', $hba or BAIL_OUT("open $hba: $!");
	my @lines = <$in>;
	close $in;
	for (@lines)
	{
		next unless /^\s*(local|host)\s+replication\b/i;
		s/\btrust\b/scram-sha-256/i;
	}
	open my $out, '>', $hba or BAIL_OUT("write $hba: $!");
	print $out @lines;
	close $out;
	return;
}

# after postmaster startup, logs go to pgdata/log/*.log; pick the newest
sub newest_log
{
	my ($node) = @_;
	my $dir = $node->data_dir . '/log';
	my $newest;
	opendir(my $dh, $dir) or return $node->logfile;
	my @files = sort { (stat("$dir/$b"))[9] <=> (stat("$dir/$a"))[9] } grep { -f "$dir/$_" } readdir($dh);
	closedir $dh;
	$newest = "$dir/" . $files[0] if @files;
	return $newest // $node->logfile;
}

sub slurp
{
	my ($f) = @_;
	return '' unless defined $f && -f $f;
	return PostgreSQL::Test::Utils::slurp_file($f);
}

sub tail
{
	my ($text, $n) = @_;
	return '' unless defined $text && length $text;
	my @l = split /\n/, $text;
	return join("\n", @l[ -$n .. -1 ]);
}
