# 004_polar_px_qc_auth.pl
#	  Query coordinator authentication test cases with different combinations of
#	  existing and missing TLS certificates
#
# IDENTIFICATION
#	  src/test/authentication/t/004_polar_px_qc_auth.pl

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use File::Spec;
use FindBin;
use Cwd 'abs_path';


my $regress_db = 'postgres';

my $sql_general = q[explain analyze select * from foo;];
my $sql_entrydb = q[set polar_px_dop_per_node = 3;
					explain analyze
					select distinct x1, rank() over(order by x1)
					from (select x1 from foo order by x1) x;];


# Set up the cluster (primary + replica)

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary();
$node_primary->append_conf(
	'postgresql.conf', q[
		polar_enable_px = on
		listen_addresses = '127.0.0.1'
	]);

my $node_replica = PostgreSQL::Test::Cluster->new('replica');
$node_replica->polar_init_replica($node_primary);
$node_replica->append_conf(
	'postgresql.conf', q[
		listen_addresses = '127.0.0.1'
	]);

$node_primary->start;
$node_primary->polar_create_slot($node_replica->name);

$node_replica->start;

$node_primary->safe_psql(
	$regress_db,
	q[
		CREATE TABLE foo (x1 int, x2 int, x3 int);
		INSERT INTO foo SELECT i, i + 1, i + 2 FROM generate_series(1, 10) i;
		ANALYZE foo;
	]);

# Connection string that makes an ordinary client pose as a QC connecting to
# the replica: a well-formed pxid with the cluster's system identifier, which
# any database user can read.
my $sysid = $node_primary->safe_psql($regress_db,
	q[SELECT system_identifier FROM pg_control_system()]);
my $fake_qc_connstr = $node_replica->connstr($regress_db)
  . " pxid='2;11;$sysid;1;1;0;1;0;1;0;0'";


### TEST CASE: TLS verification is enabled but credentials are not configured
if ($ENV{with_ssl} ne 'openssl')
{
	test_query($node_primary, $regress_db, 3,
			   qr/FATAL:\s+authentication failed for query coordinator.*DETAIL:\s+TLS is not supported by this build/s,
			   $sql_general,
			   'TLS verification is enabled but credentials are not configured');
	$node_replica->connect_fails($fake_qc_connstr,
		'Non-QC client is rejected without TLS support',
		expected_stderr => qr/TLS is not supported by this build/);
}
else
{
	test_query($node_primary, $regress_db, 3,
			   qr/FATAL:\s+QE is unable to accept PX connections.*DETAIL:\s+CA certificate missing/s,
			   $sql_general,
			   'TLS verification is enabled but credentials are not configured');
	$node_replica->connect_fails($fake_qc_connstr,
		'Non-QC client is rejected when CA is not configured',
		expected_stderr => qr/QE is unable to accept PX connections/);
}


### TEST CASE: TLS verification is disabled in configuration on QE
$node_replica->safe_psql(
		$regress_db,
		qq[ALTER SYSTEM SET polar_px_enable_auth_on_qe = off]);
$node_replica->reload;
test_query($node_primary, $regress_db, 0, '', $sql_general,
		   'TLS verification is disabled in configuration on QE');
$node_replica->connect_ok($fake_qc_connstr,
	'Non-QC client is accepted when TLS verification is disabled');

$node_replica->safe_psql(
		$regress_db,
		qq[ALTER SYSTEM RESET polar_px_enable_auth_on_qe]);
$node_replica->reload;


if ($ENV{with_ssl} ne 'openssl')
{
	$node_primary->stop;
	$node_replica->stop;
	done_testing();
	exit;
}

### POSITIVE TEST SCENARIOS

### TEST CASE: Successful authentication with valid TLS certificates
setup_tls_creds($node_primary, $regress_db, '', 'qc.crt', 'qc.key');
setup_tls_creds($node_replica, $regress_db, 'qe_ca.crt', '', '');
test_query($node_primary, $regress_db, 0, '', $sql_general,
			'Successful authentication with valid TLS certificates');

### TEST CASE: Entrydb slice on coordinator (requires TLS cert, key, and CA on QC)
setup_tls_creds($node_primary, $regress_db, 'qe_ca.crt', 'qc.crt', 'qc.key');
setup_tls_creds($node_replica, $regress_db, 'qe_ca.crt', '', '');
test_query($node_primary, $regress_db, 0, '', $sql_entrydb,
			'Entrydb slice on coordinator (requires TLS cert, key, and CA on QC)');


### CLIENTS POSING AS QC

setup_tls_creds($node_replica, $regress_db, 'qe_ca.crt', '', '');

### TEST CASE: Client sends pxid without a certificate
$node_replica->connect_fails($fake_qc_connstr,
	'Non-QC client without certificate is rejected',
	expected_stderr => qr/missing PX QC certificate/);

### TEST CASE: Client sends a valid certificate but has no private key
my $qc_cert = slurp_file(
	abs_path(File::Spec->catfile($FindBin::RealBin, '..', 'tls_creds', 'qc.crt')));
$node_replica->connect_fails(
	"$fake_qc_connstr px_auth_cert='$qc_cert'",
	'Non-QC client with a stolen certificate is rejected',
	expected_stderr => qr/authentication method 21 not supported/);


### FAILURE HANDLING

### UNCONFIGURED TLS CREDENTIALS ON QC OR QE

### TEST CASE: Omitted QC cert and private key, but CA is configured on QE
setup_tls_creds($node_primary, $regress_db, '', '', '');
setup_tls_creds($node_replica, $regress_db, 'qe_ca.crt', '', '');
test_query($node_primary, $regress_db, 3,
		   qr/FATAL:\s+missing PX QC certificate/,
		   $sql_general,
		   'Omitted QC cert and private key, but CA is configured on QE');

### TEST CASE: CA is not specified for QE
setup_tls_creds($node_primary, $regress_db, '', 'qc.crt', 'qc.key');
setup_tls_creds($node_replica, $regress_db, '', '', '');
test_query($node_primary, $regress_db, 3,
		   qr/QE is unable to accept PX connections.*DETAIL:\s+CA certificate missing/s,
		   $sql_general,
		   'CA is not specified for QE');


### INVALID TLS CREDENTIALS ON QC AND QE (MISSING OR CORRUPTED)

### TEST CASE: Private key is not specified for QC
setup_tls_creds($node_primary, $regress_db, '', 'qc.crt', '');
setup_tls_creds($node_replica, $regress_db, 'qe_ca.crt', '', '');
test_query($node_primary, $regress_db, 3,
		   qr/ERROR:\s+PX TLS authentication error\s+DETAIL:\s+PX TLS credentials on QC could not be loaded/,
		   $sql_general,
		   'Private key is not specified for QC');

### TEST CASE: Certificate is not specified for QC
setup_tls_creds($node_primary, $regress_db, '', '', 'qc.key');
setup_tls_creds($node_replica, $regress_db, 'qe_ca.crt', '', '');
test_query($node_primary, $regress_db, 3,
		   qr/ERROR:\s+PX TLS authentication error\s+DETAIL:\s+PX TLS credentials on QC could not be loaded/,
		   $sql_general,
		   'Certificate is not specified for QC');

### TEST CASE: Missing (non-existent) certificate, private key, and CA files
setup_tls_creds($node_primary, $regress_db, '', 'qc_missing.crt', 'qc.key');
setup_tls_creds($node_replica, $regress_db, 'qe_ca.crt', '', '');
test_query($node_primary, $regress_db, 3,
		   qr/ERROR:\s+PX TLS authentication error\s+DETAIL:\s+PX TLS credentials on QC could not be loaded/,
		   $sql_general,
		   'Missing (non-existent) certificate file on QC');

setup_tls_creds($node_primary, $regress_db, '', 'qc.crt', 'qc_missing.key');
setup_tls_creds($node_replica, $regress_db, 'qe_ca.crt', '', '');
test_query($node_primary, $regress_db, 3,
		   qr/ERROR:\s+PX TLS authentication error\s+DETAIL:\s+PX TLS credentials on QC could not be loaded/,
		   $sql_general,
		   'Missing (non-existent) private key file on QC');

setup_tls_creds($node_primary, $regress_db, '', 'qc.crt', 'qc.key');
setup_tls_creds($node_replica, $regress_db, 'qe_ca_missing.crt', '', '');
test_query($node_primary, $regress_db, 3,
		   qr/QE is unable to accept PX connections.*DETAIL:\s+CA certificate could not be loaded on QE/s,
		   $sql_general,
		   'Missing (non-existent) CA file on QE');

### TEST CASE: Corrupted TLS cred files
setup_tls_creds($node_primary, $regress_db, '', 'qc_corrupted.crt', 'qc.key');
setup_tls_creds($node_replica, $regress_db, 'qe_ca.crt', '', '');
test_query($node_primary, $regress_db, 3,
		   qr/ERROR:\s+PX TLS authentication error.*DETAIL:\s+PX TLS credentials on QC could not be loaded/s,
		   $sql_general,
		   'Corrupted certificate file on QC');

setup_tls_creds($node_primary, $regress_db, '', 'qc.crt', 'qc_corrupted.key');
setup_tls_creds($node_replica, $regress_db, 'qe_ca.crt', '', '');
test_query($node_primary, $regress_db, 3,
		   qr/ERROR:\s+PX TLS authentication error.*DETAIL:\s+PX TLS credentials on QC could not be loaded/s,
		   $sql_general,
		   'Corrupted key file on QC');

setup_tls_creds($node_primary, $regress_db, '', 'qc.crt', 'qc.key');
setup_tls_creds($node_replica, $regress_db, 'qe_ca_corrupted.crt', '', '');
test_query($node_primary, $regress_db, 3,
		   qr/FATAL:\s+QE is unable to accept PX connections.*DETAIL:\s+CA certificate could not be loaded on QE/s,
		   $sql_general,
		   'Corrupted CA file on QE');

### TEST CASE: Mismatch between certificate and private key
setup_tls_creds($node_primary, $regress_db, '', 'qc.crt', 'qc_wrong.key');
setup_tls_creds($node_replica, $regress_db, 'qe_ca.crt', '', '');
test_query($node_primary, $regress_db, 3,
		   qr/PX TLS authentication error.*DETAIL:\s+PX TLS credentials on QC could not be loaded/s,
		   $sql_general,
		   'Mismatch between certificate and private key');

setup_tls_creds($node_primary, $regress_db, '', 'qc_wrong.crt', 'qc.key');
setup_tls_creds($node_replica, $regress_db, 'qe_ca.crt', '', '');
test_query($node_primary, $regress_db, 3,
		   qr/PX TLS authentication error.*DETAIL:\s+PX TLS credentials on QC could not be loaded/s,
		   $sql_general,
		   'Mismatch between certificate and private key');

### TEST CASE: Certificate validation fails due to incorrect Common Name (CN)
setup_tls_creds($node_primary, $regress_db, '', 'qc_wrong_CN.crt', 'qc_wrong_CN.key');
setup_tls_creds($node_replica, $regress_db, 'qe_ca.crt', '', '');
test_query($node_primary, $regress_db, 3,
		   qr/FATAL:\s+invalid PX QC certificate passed.*DETAIL:\s+invalid TLS certificate's common name:\s+"db_user"/s,
		   $sql_general,
		   'Incorrect Common Name (CN)');


### CORE TLS AUTHENTICATION

### TEST CASE: Mismatch between certificate and CA
setup_tls_creds($node_primary, $regress_db, '', 'qc_wrong.crt', 'qc_wrong.key');
setup_tls_creds($node_replica, $regress_db, 'qe_ca.crt', '', '');

test_query($node_primary, $regress_db, 3,
		   qr/invalid PX QC certificate passed.*DETAIL:\s+failed to validate PX QC certificate: certificate signature failure/s,
		   $sql_general,
		   'Mismatch between certificate and CA');


$node_primary->stop;
$node_replica->stop;

done_testing();




sub setup_tls_creds {
	my ($node, $db_name, $ca_fname, $cert_fname, $key_fname) = @_;
	my $tls_dir = abs_path(File::Spec->catdir($FindBin::RealBin, '..', 'tls_creds'));
	my $ca_path = $ca_fname ne ''
					? File::Spec->catfile($tls_dir, $ca_fname)
					: '';
	my $cert_path = $cert_fname ne ''
					? File::Spec->catfile($tls_dir, $cert_fname)
					: '';
	my $key_path = $key_fname ne ''
					? File::Spec->catfile($tls_dir, $key_fname)
					: '';

	$node->safe_psql(
		$db_name,
		qq[
			ALTER SYSTEM SET polar_px_ca_file = '$ca_path';
			ALTER SYSTEM SET polar_px_peer_cert_file = '$cert_path';
			ALTER SYSTEM SET polar_px_peer_key_file = '$key_path';
		]);
		$node->reload;
}


sub test_query {
	my ($node, $db_name, $expected_res, $expected_err, $test_sql, $test_name) = @_;
	my ($res, $out, $err) = $node->psql($db_name, $test_sql);

	if ($expected_res eq 0)
	{
		ok($res == '0' && $out =~ qr/PolarDB PX Optimizer/, $test_name);
	}
	else
	{
		like(
			$err,
			$expected_err,
			$test_name);
	}
}

