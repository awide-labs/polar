# Shared Server: flow control and half-close.
#
# A client that writes faster than its backend reads fills the socket pair
# to the backend and then the client channel's buffer; from then on it must
# block in send() until the backend catches up.  The same holds for a client
# that reads slowly.  Nothing may be lost and the session must not stall:
#
#  1. a pipelined batch of a few MB behind pg_sleep();
#  2. psql COPY FROM STDIN into a table whose CHECK sleeps;
#  3. a client that asks for ~20 MB of rows and reads nothing for 3 s.
#
# A client may also stop writing without Terminate: shutdown(SHUT_WR), or
# just going away.  A half-closed client still reads, so it must get every
# answer, then EOF:
#
#  4. idle;
#  5. a query still running;
#  6. a write to the backend still pending at EOF;
#  7. mid COPY FROM STDIN, with neither CopyDone nor CopyFail.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IO::Select;
use IO::Socket::UNIX;
use Socket qw(SOCK_STREAM);
use Time::HiRes qw(time);

use FindBin;
use lib $FindBin::RealBin;

use PolarSharedServer;

my $node = PostgreSQL::Test::Cluster->new('ss_backpressure');
$node->polar_init_primary;

my $ss = PolarSharedServer->new($node);

# A stalled session keeps its backend; later cases need their own.
$node->append_conf('postgresql.conf', ss_conf(backend_max_count => 8));

$node->start;

# No byte moving either way for this long is a stall.  Some cases keep the
# backend busy for seconds on purpose.
my $stall_timeout = 30;

my $user = $node->safe_psql('postgres', 'SELECT current_user');

# One frontend message.
sub fe_message
{
	my ($type, $body) = @_;

	return $type . pack('N', length($body) + 4) . $body;
}

sub fe_query
{
	my ($sql) = @_;

	return fe_message('Q', "$sql\0");
}

# Splits complete backend messages off the front of $$buf.
sub be_messages
{
	my ($buf) = @_;
	my @msgs;

	while (length($$buf) >= 5)
	{
		my ($type, $len) = unpack('a N', $$buf);

		last if length($$buf) < $len + 1;
		push @msgs, [ $type, substr($$buf, 5, $len - 4) ];
		substr($$buf, 0, $len + 1, '');
	}
	return @msgs;
}

# Writes $payload while reading replies, as a pipelining client must:
# otherwise both sides end up blocked in send().
#
# Options:
#   want => [ $type, $n ]   stop once $n messages of $type have arrived
#   half_close => 1         shutdown(SHUT_WR) once $payload is written
#
# Without "want", reads until the server closes.  Gives up after
# $stall_timeout seconds without progress.
#
# Returns bytes "written", per-type message "count", ErrorResponse texts in
# "errors", and whether the server "closed" the connection.
sub pump
{
	my ($sock, $payload, %opt) = @_;
	my $sel = IO::Select->new($sock);
	my %res = (written => 0, count => {}, errors => '', closed => 0);
	my ($in, $shut) = ('', 0);
	my $moved = time;

	$sock->blocking(0);
	while (time - $moved < $stall_timeout)
	{
		last
		  if $opt{want}
		  && ($res{count}{ $opt{want}[0] } // 0) >= $opt{want}[1];

		if ($opt{half_close} && !$shut && $res{written} == length($payload))
		{
			shutdown($sock, 1) or die "shutdown(SHUT_WR) failed: $!";
			$shut = 1;
		}

		my $writing = $res{written} < length($payload);
		my ($r, $w) =
		  IO::Select->select($sel, $writing ? $sel : undef, undef, 0.5);

		if ($w && @$w)
		{
			my $n = syswrite($sock, $payload, 65536, $res{written});

			if (defined $n)
			{
				$res{written} += $n;
				$moved = time;
			}
		}
		if ($r && @$r)
		{
			my $n = sysread($sock, my $chunk, 65536);

			next unless defined $n;
			if ($n == 0)
			{
				$res{closed} = 1;
				last;
			}
			$in .= $chunk;
			$moved = time;
			foreach my $msg (be_messages(\$in))
			{
				my ($type, $body) = @$msg;

				$res{count}{$type}++;
				if ($type eq 'E')
				{
					my %field = map { substr($_, 0, 1) => substr($_, 1) }
					  grep { length } split /\0/, $body;

					$res{errors} .= "$field{M}\n";
				}
			}
		}
	}
	return \%res;
}

# Connects without libpq, so that the test controls every write.  Returns
# once the server is ready for a query.
sub raw_connect
{
	my ($name) = @_;
	my $sock = IO::Socket::UNIX->new(
		Type => SOCK_STREAM,
		Peer => $node->host . '/.s.PGSQL.' . $node->port)
	  or die "could not connect to the node: $!";
	my $startup =
	  pack('N', 3 << 16) . "user\0$user\0database\0postgres\0\0";
	my $res = pump($sock, pack('N', length($startup) + 4) . $startup,
		want => [ 'Z', 1 ]);

	is($res->{count}{Z}, 1, "$name: connected");
	is($res->{errors}, '', "$name: the startup raised no error");
	return $sock;
}

# Runs one query and waits for it.
sub raw_query_safe
{
	my ($sock, $name, $sql) = @_;
	my $res = pump($sock, fe_query($sql), want => [ 'Z', 1 ]);

	is($res->{count}{Z}, 1, "$name: query answered");
	is($res->{errors}, '', "$name: query succeeded");
	return;
}

#
# 1. ~4 MB pipelined behind pg_sleep(3): far more than the client buffer and
#    the socket pair hold.
#
my $sock = raw_connect('pipelined batch');

my $queries = 4000;
my $pad = 'x' x 1000;
my $batch = fe_query('SELECT pg_sleep(3)');
$batch .= fe_query("SELECT $_ /* $pad */") foreach 1 .. $queries;

my $res = pump($sock, $batch, want => [ 'Z', $queries + 1 ]);

is($res->{written}, length($batch),
	'pipelined batch: the dispatcher took all of it from the client');
is($res->{count}{Z}, $queries + 1,
	'pipelined batch: every query was answered');
is($res->{errors}, '', 'pipelined batch: no query failed');

close($sock);

#
# 2. COPY FROM STDIN into a table whose CHECK sleeps 1 ms a row.  A SQL
#    function, since loading a PL would dedicate the backend.
#
$node->safe_psql(
	'postgres', q{
	CREATE FUNCTION slow_check() RETURNS bool LANGUAGE sql
	  AS $$ SELECT pg_sleep(0.001) IS NOT NULL $$;
	CREATE TABLE copy_target (i int, pad text, CHECK (slow_check()));
});

my $rows = 3000;
my $data = join('', map { "$_\t" . ('y' x 300) . "\n" } 1 .. $rows);
my ($stdout, $stderr, $timed_out) = ('', '');

$node->psql(
	'postgres',
	"COPY copy_target FROM STDIN;\n$data\\.\n",
	stdout => \$stdout,
	stderr => \$stderr,
	timeout => 2 * $stall_timeout,
	timed_out => \$timed_out);

ok(!$timed_out, 'COPY FROM STDIN: finished');
is($stderr, '', 'COPY FROM STDIN: no error');
is($node->safe_psql('postgres', 'SELECT count(*) FROM copy_target'),
	$rows, 'COPY FROM STDIN: every row was loaded');

#
# 3. Slow reader: ~20 MB of rows, nothing read for 3 s, then all of it.
#
$sock = raw_connect('slow reader');

my $slow_rows = 20000;

syswrite($sock,
	fe_query("SELECT i, repeat('r', 1000) FROM generate_series(1, $slow_rows) i"))
  or die "could not send the query: $!";
sleep(3);
$res = pump($sock, '', want => [ 'Z', 1 ]);

is($res->{count}{D} // 0, $slow_rows, 'slow reader: every row arrived');
is($res->{count}{Z} // 0, 1, 'slow reader: ReadyForQuery followed them');
is($res->{errors}, '', 'slow reader: no error');

$res = pump($sock, fe_query('SELECT 1'), want => [ 'Z', 1 ]);
is($res->{count}{Z} // 0, 1, 'slow reader: the session still works');

close($sock);

#
# 4. Half-close on an idle session.
#
$sock = raw_connect('hc_idle');
$res = pump($sock, '', half_close => 1);

ok($res->{closed}, 'hc_idle: the server closed the connection');

close($sock);

#
# 5. Half-close with a 1 s INSERT still running.
#
$node->safe_psql('postgres', 'CREATE TABLE hc_rows (i int)');

$sock = raw_connect('hc_inflight');
$res = pump($sock, fe_query('INSERT INTO hc_rows SELECT 1 FROM pg_sleep(1)'),
	half_close => 1);

is($res->{count}{C} // 0, 1, 'hc_inflight: the INSERT was answered');
is($res->{count}{Z} // 0, 1, 'hc_inflight: ReadyForQuery followed it');
ok($res->{closed}, 'hc_inflight: then the server closed the connection');
is($node->safe_psql('postgres', 'SELECT count(*) FROM hc_rows'),
	'1', 'hc_inflight: the INSERT committed');

close($sock);

#
# 6. Half-close with a write to the backend pending.  A 2 MB query first grows
#    the client buffer (it never shrinks), so the 1 MB batch fits; the socket
#    pair holds a few hundred KB, so the write is still pending at EOF.
#
my $big_query = 'SELECT 1 /* ' . ('z' x (2 << 20)) . ' */';
my $pending = 1000;
$batch = fe_query('SELECT pg_sleep(2)');
$batch .= fe_query("SELECT $_ /* $pad */") foreach 1 .. $pending;

$sock = raw_connect('hc_pending');
raw_query_safe($sock, 'hc_pending', $big_query);

$res = pump($sock, $batch, half_close => 1);

is($res->{written}, length($batch),
	'hc_pending: the dispatcher took the whole batch');
is($res->{count}{Z} // 0, $pending + 1,
	'hc_pending: every query was answered');
ok($res->{closed}, 'hc_pending: then the server closed the connection');

close($sock);

#
# 7. Half-close mid COPY FROM STDIN: two CopyData rows, then nothing.
#
$node->safe_psql('postgres', 'CREATE TABLE hc_copy (i int)');

$sock = raw_connect('hc_copy');
$res = pump($sock, fe_query('COPY hc_copy FROM STDIN'), want => [ 'G', 1 ]);
is($res->{count}{G}, 1, 'hc_copy: the server is in COPY IN');

$res = pump($sock, fe_message('d', "1\n") . fe_message('d', "2\n"),
	half_close => 1);

ok($res->{closed}, 'hc_copy: the server closed the connection');
is($node->safe_psql('postgres', 'SELECT count(*) FROM hc_copy'),
	'0', 'hc_copy: the unfinished COPY loaded nothing');

close($sock);

#
# 8. No crashes.
#
$ss->assert_clean_log;

$node->stop;

done_testing();
