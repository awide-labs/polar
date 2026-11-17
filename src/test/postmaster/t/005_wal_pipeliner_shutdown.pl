# Verify that the WAL pipeliner's expected exit is not treated as a crash.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

for my $mode ('smart', 'fast')
{
	my $node = PostgreSQL::Test::Cluster->new("pipeliner_$mode");
	$node->init;
	$node->append_conf(
		'postgresql.conf', qq[
polar_wal_pipeline_enable = on
logging_collector = off
]);
	$node->start;

	is($node->safe_psql('postgres',
			q[SELECT count(*) FROM pg_stat_activity
			  WHERE backend_type = 'polar wal pipeliner']),
		'1', "$mode shutdown has a running WAL pipeliner");
	# pg_ctl only waits for the PID file to disappear, so its success does
	# not establish that the postmaster exited normally.  Check the log too.
	$node->stop($mode);
	unlike(
		slurp_file($node->logfile),
		qr/WAL pipeliner process .* exited|terminating any other active server processes|abnormal database system shutdown/,
		"$mode shutdown did not treat the pipeliner exit as a crash");

}

done_testing();
