# Regression test for datamax archiving under archive_mode = always.
#
# A datamax node drives WAL archiving from its own daemon loop
# (polar_datamax_archive -> polar_datamax_ArchiverCopyLoop), which only runs
# while polar_is_datamax_mode is true. A past port reset that flag to false at
# the end of polar_datamax_handle_prealloc_walfile(); because prealloc runs
# every loop once streaming is up while the only re-setter
# (polar_datamax_handle_timeline_switch) runs only when *not* streaming,
# archiving fired for the first iteration or two after a (re)connect and then
# never again during steady-state streaming.
#
# This test pins the failure deterministically: prealloc_walfile_timeout <
# archive_timeout, so on the buggy code the flag is cleared before the first
# archive tick and stays cleared. We then complete several WAL segments *after*
# a settle window and require that at least one brand-new segment reaches the
# datamax archive directory.
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More tests => 1;

# upstream primary
my $node_master = PostgreSQL::Test::Cluster->new('master');
$node_master->polar_init_primary();
my $primary_system_identifier = $node_master->polar_get_system_identifier;
$node_master->append_conf('postgresql.conf', "polar_wal_pipeline_enable = false");
$node_master->append_conf('postgresql.conf', "polar_logindex_mem_size = 0");

# datamax
my $node_datamax = PostgreSQL::Test::Cluster->new('datamax');
$node_datamax->polar_init_datamax($primary_system_identifier);

$node_master->start;
$node_master->polar_create_slot($node_datamax->name);

# operator setup phase: bring datamax up writable to create the extension,
# then stop and flip it into datamax mode.
$node_datamax->start;
$node_datamax->safe_psql('postgres', 'CREATE EXTENSION polar_monitor;');
$node_datamax->stop;

$node_datamax->polar_datamax_set_recovery($node_master);

# Archive from the datamax itself, with archive_mode = always so the datamax
# archive scheduler (XLogArchivingAlways()) is exercised. Order the timeouts so
# prealloc (which on the buggy code cleared polar_is_datamax_mode) fires before
# the first archive tick.
my $archive_dir = $node_datamax->archive_dir;
my $copy_command =
  $PostgreSQL::Test::Utils::windows_os
  ? qq{copy "%p" "$archive_dir\\\\%f"}
  : qq{cp "%p" "$archive_dir/%f"};
$node_datamax->append_conf(
	'postgresql.conf', qq(
archive_mode = always
archive_command = '$copy_command'
polar_datamax_archive_timeout = 1000
polar_datamax_prealloc_walfile_timeout = 500
polar_datamax_prealloc_walfile_num = 2
polar_wal_pipeline_enable = false
polar_logindex_mem_size = 0
));
$node_datamax->start;
$node_datamax->wait_walstreaming_establish_timeout(20);

# List WAL-segment-named files currently in the archive directory.
sub archived_segments
{
	my ($dir) = @_;
	opendir(my $dh, $dir) or return ();
	my @segs = grep { /^[0-9A-F]{24}$/ } readdir($dh);
	closedir($dh);
	return @segs;
}

# Settle: stream long enough that, on the buggy code, prealloc has cleared
# polar_is_datamax_mode and archiving is wedged off. (archive_timeout is 1s, so
# a few seconds is well past the "first iteration or two" window.)
sleep 5;

# Snapshot what is already archived; anything completed before now (including a
# possible startup burst on the buggy code) is excluded from the assertion.
my %archived_before = map { $_ => 1 } archived_segments($archive_dir);
note("archived before steady-state burst: " . join(',', sort keys %archived_before));

# Complete several fresh WAL segments AFTER settling.
$node_master->safe_psql('postgres', 'CREATE TABLE t_archive_always(i int);');
for my $k (1 .. 6)
{
	$node_master->safe_psql('postgres',
		'INSERT INTO t_archive_always SELECT generate_series(1, 1000);');
	$node_master->safe_psql('postgres', 'SELECT pg_switch_wal();');
}
my $insert_lsn = $node_master->lsn('insert');

# Make sure the datamax has received/flushed the freshly completed segments
# before we start timing the archive poll.
$node_master->wait_for_catchup($node_datamax, 'flush',
	$insert_lsn, timeout => 60, return_failed => 1);

# A brand-new segment (completed after the settle window) must get archived.
my $got_new_segment = 0;
for (my $i = 0; $i < 60 && !$got_new_segment; $i++)
{
	foreach my $seg (archived_segments($archive_dir))
	{
		if (!$archived_before{$seg})
		{
			note("newly archived segment: $seg");
			$got_new_segment = 1;
			last;
		}
	}
	sleep 1 unless $got_new_segment;
}

ok($got_new_segment,
	'datamax keeps archiving completed WAL segments during steady-state streaming with archive_mode=always'
);

$node_datamax->stop;
$node_master->stop;
