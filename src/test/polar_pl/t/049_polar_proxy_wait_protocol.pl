# 049_polar_proxy_wait_protocol.pl
#
# Raw-protocol coverage for the proxy replay-wait message ('W',
# PqMsg_PolarProxyWait) negotiated with _pq_.polar_proxy_wait_v1:
#   * accepted - W before Parse or between Parse and Bind;
#   * rejected - malformed W, W after Bind, W over a suspended portal
#                or an established/aborted transaction, W without
#                negotiation, and W on the primary;
#   * recovery - protocol errors discard pipelined messages until Sync;
#   * behavior - best-effort vs strict timeouts, cancellation,
#                statement_timeout, the PolarProxyLsnWait wait event,
#                and delayed replay applied before the query runs.
#
# IDENTIFICATION
#	  src/test/polar_pl/t/049_polar_proxy_wait_protocol.pl

use strict;
use warnings;
use IO::Socket::UNIX;
use IO::Select;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan skip_all => 'Unix sockets required' if $windows_os;

sub packet
{
	my ($type, $payload) = @_;
	return $type . pack('N', length($payload) + 4) . $payload;
}

sub read_bytes
{
	my ($socket, $length) = @_;
	my $result = '';
	my $selector = IO::Select->new($socket);
	while (length($result) < $length)
	{
		die 'protocol read timed out' unless $selector->can_read(30);
		my $count = sysread($socket, my $chunk, $length - length($result));
		die 'unexpected protocol EOF' unless $count;
		$result .= $chunk;
	}
	return $result;
}

sub receive
{
	my ($socket, $terminal) = @_;
	$terminal //= 'Z';
	my @messages;
	while (1)
	{
		my $type = read_bytes($socket, 1);
		my $length = unpack('N', read_bytes($socket, 4));
		die "invalid message length $length" if $length < 4;
		my $payload = read_bytes($socket, $length - 4);
		push @messages, [$type, $payload];
		last if $type eq $terminal;
	}
	return \@messages;
}

sub exchange
{
	my ($socket, @messages) = @_;
	print {$socket} join('', @messages) or die "protocol write: $!";
	return receive($socket);
}

sub messages
{
	my ($response, $type) = @_;
	return join('', map { $_->[1] } grep { $_->[0] eq $type } @$response);
}

sub connect_proxy
{
	my ($node, $capability, $proxy) = @_;
	$capability //= '1';
	$proxy //= 1;
	my $socket = IO::Socket::UNIX->new(
		Type => SOCK_STREAM,
		Peer => $node->host . '/.s.PGSQL.' . $node->port)
	  or die "connect: $!";
	$socket->autoflush(1);
	my @options = (user => scalar getpwuid($>), database => 'postgres');
	push @options, '_polar_proxy_client_host' => '127.0.0.1',
	  '_polar_proxy_client_port' => '12345' if $proxy;
	push @options, '_pq_.polar_proxy_wait_v1' => $capability
	  if length($capability);
	my $startup = pack('N', 196608) . join("\0", @options) . "\0\0";
	print {$socket} pack('N', length($startup) + 4) . $startup;
	my $response = receive($socket,
		($capability eq '2' || (!$proxy && length($capability))) ? 'E' : 'Z');
	return ($socket, $response);
}

sub wait_packet
{
	my ($target, $timeout, $mode, $version, $flags) = @_;
	return packet('W', pack('CCnNQ>', $version // 1, $mode // 1,
		$flags // 0, $timeout // 1000, $target // 1));
}

sub parse_packet
{
	my ($sql) = @_;
	return packet('P', "\0$sql\0" . pack('n', 0));
}

sub bind_packet
{
	return packet('B', "\0\0" . pack('nnn', 0, 0, 0));
}

sub execute_packet
{
	my ($rows) = @_;
	return packet('E', "\0" . pack('N', $rows // 0));
}

my $sync = packet('S', '');
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->polar_init_primary;
my $replica = PostgreSQL::Test::Cluster->new('replica');
$replica->polar_init_replica($primary);
$primary->start;
$primary->polar_create_slot($replica->name);
$replica->start;
$primary->safe_psql('postgres', 'CREATE TABLE protocol_wait_data AS SELECT 42 AS value');
$primary->wait_for_catchup($replica);

my @select = (parse_packet('SELECT value FROM protocol_wait_data'),
	bind_packet(), execute_packet(), $sync);
my ($socket, $startup) = connect_proxy($replica);
my $backend_pid = unpack('N', messages($startup, 'K'));
my $response = exchange($socket, wait_packet(), @select);
is(messages($response, 'E'), '', 'negotiated W succeeds before Bind');
like(messages($response, 'D'), qr/42$/, 'extended query returns its row');
$response = exchange($socket, wait_packet(), wait_packet(), @select);
is(messages($response, 'E'), '', 'repeated waits before snapshot acquisition are valid');

$response = exchange($socket, parse_packet('SELECT value FROM protocol_wait_data'),
	wait_packet(), bind_packet(), execute_packet(), $sync);
is(messages($response, 'E'), '', 'W between Parse and Bind is accepted');
like(messages($response, 'D'), qr/42$/, 'query runs after W between Parse and Bind');

for my $bad (
	['version', wait_packet(1, 10, 1, 2)],
	['mode', wait_packet(1, 10, 2)],
	['flags', wait_packet(1, 10, 1, 1, 1)],
	['zero target', wait_packet(0)],
	['short payload', packet('W', '')],
	['long payload', packet('W', pack('CCnNQ>C', 1, 1, 0, 10, 1, 0))])
{
	$response = exchange($socket, $bad->[1], @select);
	like(messages($response, 'E'), qr/08P01/, "$bad->[0]: protocol error");
}

# A protocol error discards the pipelined messages until Sync; that
# recovery is generic, so verify it once instead of per variant.
$response = exchange($socket, wait_packet(), @select);
is(messages($response, 'E'), '', 'Sync recovers after a rejected W');
like(messages($response, 'D'), qr/42$/, 'query runs after recovery');

for my $prefix ([], [wait_packet()])
{
	$response = exchange($socket, @$prefix, parse_packet('SELECT 1'),
		bind_packet(), wait_packet(), execute_packet(), $sync);
	like(messages($response, 'E'), qr/requires a fresh snapshot/,
		'W after Bind is rejected, including after an earlier W');
	is(messages($response, 'D'), '', 'old portal does not execute');
}

exchange($socket, packet('Q', "BEGIN\0"));
print {$socket} parse_packet('SELECT generate_series(1, 2)')
	. bind_packet() . execute_packet(1) . packet('H', '');
receive($socket, 's');
$response = exchange($socket, wait_packet(), execute_packet(), $sync);
like(messages($response, 'E'), qr/requires a fresh snapshot/,
	'W cannot refresh a suspended portal');
exchange($socket, packet('Q', "ROLLBACK\0"));

exchange($socket, packet('Q', "BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1\0"));
$response = exchange($socket, wait_packet(), @select);
like(messages($response, 'E'), qr/requires a fresh snapshot/,
	'W cannot refresh an established transaction snapshot');
exchange($socket, packet('Q', "ROLLBACK\0"));

exchange($socket, packet('Q', "BEGIN; SELECT 1/0\0"));
$response = exchange($socket, wait_packet(), @select);
like(messages($response, 'E'), qr/25P02/, 'W is rejected in an aborted transaction');
exchange($socket, packet('Q', "ROLLBACK\0"));

exchange($socket, packet('Q', "BEGIN\0"));
for (1 .. 2)
{
	$response = exchange($socket, wait_packet(), parse_packet('SELECT 1'),
		bind_packet(), execute_packet(), packet('C', "P\0"), $sync);
	is(messages($response, 'E'), '', 'closing portal allows next W in READ COMMITTED');
}
exchange($socket, packet('Q', "COMMIT\0"));

exchange($socket, packet('Q', "BEGIN\0"));
my @closed_query = (wait_packet(), parse_packet('SELECT 1'), bind_packet(),
	execute_packet(), packet('C', "P\0"));
$response = exchange($socket, @closed_query, @closed_query, $sync);
is(messages($response, 'E'), '', 'Close permits two W queries in one pipeline');
is(scalar(grep { $_->[0] eq 'C' } @$response), 2,
	'both Close-separated pipelined queries complete');
exchange($socket, packet('Q', "COMMIT\0"));

my $future_lsn = $primary->safe_psql('postgres',
	"SELECT (pg_current_wal_lsn() - '0/0'::pg_lsn + 1073741824)::bigint");
$response = exchange($socket, wait_packet($future_lsn, 10, 0), @select);
like(messages($response, 'N'), qr/polar_proxy_lsn_wait_timeout/,
	'best-effort W reports timeout warning');
like(messages($response, 'D'), qr/42$/, 'best-effort W permits execution');

$response = exchange($socket, wait_packet($future_lsn, 10), @select);
like(messages($response, 'E'), qr/polar_proxy_lsn_wait_timeout/,
	'strict W reports replay timeout');
is(messages($response, 'D'), '', 'strict W timeout skips execution');

print {$socket} wait_packet($future_lsn, 10000) . packet('H', '');
ok($replica->poll_query_until('postgres',
	"SELECT state = 'active' AND wait_event = 'PolarProxyLsnWait'
	 AND query = 'POLAR PROXY LSN WAIT' FROM pg_stat_activity WHERE pid = $backend_pid"),
	'W reports active activity and its dedicated wait event');
$replica->safe_psql('postgres', "SELECT pg_cancel_backend($backend_pid)");
$response = exchange($socket, $sync);
like(messages($response, 'E'), qr/57014/, 'W processes cancellation');
$response = exchange($socket, wait_packet(), @select);
is(messages($response, 'E'), '', 'Sync recovers after canceled W');

exchange($socket, packet('Q', "SET statement_timeout = '50ms'\0"));
$response = exchange($socket, wait_packet($future_lsn, 0), @select);
like(messages($response, 'E'), qr/statement timeout/,
	'statement_timeout bounds an infinite W wait');

exchange($socket, packet('Q', "SET statement_timeout = 0\0"));
$replica->safe_psql('postgres', 'SELECT pg_wal_replay_pause()');
$primary->safe_psql('postgres', 'INSERT INTO protocol_wait_data VALUES (43)');
$replica->poll_query_until('postgres',
	"SELECT pg_get_wal_replay_pause_state() = 'paused'")
  or die 'replay did not pause';
my $target = $primary->safe_psql('postgres',
	"SELECT (pg_current_wal_lsn() - '0/0'::pg_lsn)::bigint");
print {$socket} wait_packet($target, 10000) . join('', @select);
ok($replica->poll_query_until('postgres',
	"SELECT wait_event = 'PolarProxyLsnWait' FROM pg_stat_activity WHERE pid = $backend_pid"),
	'W waits for delayed replay before Bind');
$replica->safe_psql('postgres', 'SELECT pg_wal_replay_resume()');
$response = receive($socket);
is(messages($response, 'E'), '', 'delayed W succeeds');
like(messages($response, 'D'), qr/43/, 'new snapshot sees the replayed insert');
close $socket;

for my $config (
	[$replica, '', 1, qr/not negotiated/],
	[$primary, '1', 1, qr/only valid on a replica/])
{
	my ($other) = connect_proxy(@$config[0 .. 2]);
	$response = exchange($other, wait_packet(), @select);
	like(messages($response, 'E'), $config->[3], 'W rejects unsupported connection');
	close $other;
}
for my $config (['2', 1], ['1', 0])
{
	my ($other, $reply) = connect_proxy($replica, @$config);
	like(messages($reply, 'E'), qr/invalid value|Proxy is disabled/,
		'invalid startup capability is rejected');
	close $other;
}

$primary->stop;
$replica->stop;
done_testing();
