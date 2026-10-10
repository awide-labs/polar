use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('master');
$node->init;
$node->append_conf('postgresql.conf', "wal_level = logical");
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = '\$libdir/pg_squeeze'");
$node->append_conf('postgresql.conf',
	"output_plugin_libraries = 'pg_squeeze'");
$node->start;

my $host = $node->host;
my $port = $node->port;

my $ret = system("./test/concurrency --host=${host} --port=${port}");
ok($ret == 0, "Concurrency test passed");

$node->stop;
done_testing();
