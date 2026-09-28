package PolarDatamaxUtils;

# Helpers shared by the polar_datamax TAP tests: locating a node's wal
# directory, listing it, comparing the wal two directories hold, and getting
# wal for a datamax node from a backup set.
#
# A test loads this module with:
#
#   use FindBin;
#   use lib $FindBin::RealBin;
#   use PolarDatamaxUtils;

use strict;
use warnings;

use Exporter 'import';
use PostgreSQL::Test::Utils;

our @EXPORT = qw(
  polar_waldir
  polar_get_walfile
  polar_lsn_to_int
  polar_wal_segment_size
  polar_walfile_compare
  get_wal_from_backup
);

# wal directory of a node: pg_wal in its polar data directory, or
# polar_datamax/pg_wal for a datamax node
sub polar_waldir
{
	my ($node, $is_datamax) = @_;
	my $polar_datadir = $node->polar_get_datadir;
	return $is_datamax
	  ? "$polar_datadir/polar_datamax/pg_wal"
	  : "$polar_datadir/pg_wal";
}

# print and return the current wal files of a node
sub polar_get_walfile
{
	my ($node, $is_datamax) = @_;
	my $polar_waldir = polar_waldir($node, $is_datamax);
	my $name = $node->name;
	my $walfile = readpipe("ls $polar_waldir");
	my @wal_array = split('\n', $walfile);
	print "current walfile of node $name:\n";
	foreach my $i (@wal_array)
	{
		print "$i\n";
	}
	return @wal_array;
}

# convert an lsn of the form "X/Y" to a byte position
sub polar_lsn_to_int
{
	my ($lsn) = @_;
	# X and Y are the high and low 32 bits of the position, in hex
	my ($hi, $lo) = split('/', $lsn);
	return (hex($hi) << 32) | hex($lo);
}

# wal segment size of a running node, in bytes
sub polar_wal_segment_size
{
	my ($node) = @_;
	return $node->safe_psql('postgres',
		"select setting from pg_settings where name = 'wal_segment_size'");
}

# Judge whether $waldir2 holds the same wal as $waldir1 up to $end_lsn: every
# wal file in $waldir1 that holds wal below $end_lsn must also be in $waldir2,
# with the same wal.  A node whose wal is compared must have flushed $end_lsn.
#
# Whole files cannot be compared: a node can append wal to its live segment
# at any time (e.g. a checkpoint or the bgwriter's running-xacts record), so
# one copy may hold wal the other has not received yet, and a recycled
# segment keeps stale bytes past the write point.  Compare only the part of
# each segment that lies below the end of its timeline's wal instead:
# $end_lsn for the current timeline, the switch point in the timeline history
# for an older one.  Those bytes are written once and streamed verbatim.
# Segments past the end of their timeline (e.g. preallocated or recycled ones)
# hold no wal and are skipped.  History files are compared whole.
#
# The current timeline is the one of the newest history file in $waldir1, or
# timeline 1 if there is none.  A file that disappears from $waldir1 during
# the comparison was removed by its node, and is skipped.
sub polar_walfile_compare
{
	my ($waldir1, $waldir2, $end_lsn, $segsize) = @_;
	print "compare wal in $waldir1 with $waldir2 up to $end_lsn\n";

	opendir(my $dh, $waldir1) or die "could not open $waldir1: $!";
	my @files = sort readdir($dh);
	closedir($dh);

	# A timeline history file is named by the timeline id in 8 hex digits,
	# then ".history", e.g. 00000002.history.  Fixed-width hex names sort
	# by timeline id.
	my @histories = grep { /^[0-9A-F]{8}\.history$/ } @files;
	my $cur_tli = @histories ? hex(substr($histories[-1], 0, 8)) : 1;

	# end of wal of the current timeline and of each of its ancestors
	my %tli_end = ($cur_tli => polar_lsn_to_int($end_lsn));
	if (@histories)
	{
		foreach my $line (split(/\n/, slurp_file("$waldir1/$histories[-1]")))
		{
			# A history line is "<parent tli>\t<switch point>\t<reason>",
			# e.g. "1\t0/2000108\tno recovery target specified".  Capture
			# the parent timeline id (decimal) and the lsn where its wal
			# ends (hex "X/Y").  Comment and blank lines do not match.
			$tli_end{$1} = polar_lsn_to_int($2)
			  if ($line =~ /^\s*(\d+)\s+([0-9A-F]+\/[0-9A-F]+)/i);
		}
	}

	my $ret = 1;
	my $ncompared = 0;
	foreach my $i (@files)
	{
		my $len;
		# A wal segment is named by 24 hex digits, e.g.
		# 000000020000000000000003: 8 for the timeline id, 8 for the high
		# 32 bits of the segment's start lsn, and 8 for the segment's number
		# within those 4 GB of wal.
		if ($i =~ /^([0-9A-F]{8})([0-9A-F]{8})([0-9A-F]{8})$/)
		{
			my $tli = hex($1);
			my $segstart = (hex($2) << 32) + hex($3) * $segsize;
			next
			  unless (defined($tli_end{$tli}) && $segstart < $tli_end{$tli});
			$len = $tli_end{$tli} - $segstart;
			$len = $segsize if ($len > $segsize);
		}
		# a timeline history file, see above
		elsif ($i =~ /^[0-9A-F]{8}\.history$/)
		{
			$len = -s "$waldir1/$i";
		}
		# anything else (archive_status, xlogtemp.*, *.partial) is not wal
		# streamed to $waldir2
		else
		{
			next;
		}

		my $res;
		if (!-f "$waldir2/$i")
		{
			$res = "missing in $waldir2";
		}
		else
		{
			# cmp -n LEN exits 0 iff the first LEN bytes are identical
			$res = readpipe("cmp -n $len $waldir1/$i $waldir2/$i 2>&1");
			$res = "ok" if ($? == 0);
		}
		chomp($res);
		if ($res ne "ok" && !-f "$waldir1/$i")
		{
			print "compare walfile $i: removed from $waldir1 meanwhile\n";
			next;
		}
		print "compare walfile $i, first $len bytes: $res\n";
		$ret = 0 if ($res ne "ok");
		$ncompared++;
	}
	return $ncompared > 0 ? $ret : 0;
}

# get wal for a datamax node from a backup set with polar_tools
sub get_wal_from_backup
{
	my ($self, $backup, $datamax_pfs, $backup_pfs) = @_;
	my $pgdata = $self->polar_get_datadir;
	my $name = $self->name;
	print "### node \"$name\" get wal from backup_set \"$backup\" \n";
	my $ret = system(
		"polar_tools datamax-get-wal -D $pgdata -M $datamax_pfs -b $backup -m $backup_pfs"
	);
	return $ret;
}

1;
