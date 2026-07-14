# Runs the test_polar_datamax() body on a real shared-storage node -- the only
# way it executes at all.  `make check` spins up a plain local-FS cluster, so
# the body no-ops via its `if (!polar_enable_shared_storage_mode)` guard.
# This test closes the gap and makes the body a CI gate.
#
# The body flips the live backend to POLAR_STANDALONE_DATAMAX, exercises the
# datamax control structure / meta file / WAL removal & archive logic, then
# writes a WAL segment built from the checked-in 16 MB fixture
# (src/test/modules/test_polar_datamax/000000010000000000000001) and walks it
# with polar_datamax_parse_xlog().  It restores POLAR_PRIMARY at the end, and
# the mutated state is per-backend, so a fresh `SELECT 1` and `stop` work after.
use strict;
use warnings;
use Cwd qw(abs_path);
use File::Basename qw(dirname);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Absolute path of the reference WAL image shipped with the test module.  The
# suite runs with cwd = its source dir and __FILE__ = t/010_...; resolve the
# module dir relative to this script so it works both in- and out-of-tree.
my $fixture = abs_path(
	dirname(__FILE__) . '/../../modules/test_polar_datamax/000000010000000000000001');
ok(defined $fixture && -f $fixture, 'reference WAL fixture exists')
  or BAIL_OUT('missing WAL fixture 000000010000000000000001');

my $node = PostgreSQL::Test::Cluster->new('primary');

# 16 MB segments: the fixture and the parse-xlog constants assume it, and it
# keeps the run relatively fast.  polar_init_primary() does the shared-storage
# setup (init_polar_node + polar-initdb.sh ... localfs).
$node->polar_init_primary(extra => ['--wal-segsize=16']);
$node->append_conf('postgresql.conf', <<'EOF');
archive_mode = on
archive_command = '/bin/true'
# The body flips node type to POLAR_STANDALONE_DATAMAX in a live primary
# backend; the background writer then samples the smaller datamax valid-LSN and
# PANICs on its "consistent lsn moved backwards" tripwire.  Disabling the flush
# list quiesces that sampling (harmless on a single node with no RO replicas).
polar_enable_flushlist = off
EOF
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION test_polar_datamax;');

my ($out, $err);
$node->psql(
	'postgres',
	"SELECT test_polar_datamax('$fixture');",
	stdout => \$out,
	stderr => \$err);

is($out, '', 'test_polar_datamax body returned one empty row');
is($node->safe_psql('postgres', 'SELECT 1'), '1', 'node still up after run');

$node->stop;

# Polar test nodes run with logging_collector=on (Cluster.pm enables it for
# polar nodes), so the server's runtime output lands in <datadir>/log/*.log
# rather than the file pg_ctl -l writes.  Read both -- and after stop, so the
# collector has flushed every message.
my $log = slurp_file($node->logfile);
my $logdir = $node->data_dir . '/log';
$log .= slurp_file($_) foreach (sort glob("$logdir/*.log"));

unlike($log, qr/TRAP:|PANIC:|FATAL:/, 'no crash in server log');
like(
	$log,
	qr{last received valid record of primary is: 0/1FFFBB8},
	'parse-xlog case 2 (intact) reached received_lsn');
like(
	$log,
	qr{last received valid record of primary is: 0/1FFF400},
	'parse-xlog case 3 (zeroed tail) reached last_valid_lsn');

done_testing();
