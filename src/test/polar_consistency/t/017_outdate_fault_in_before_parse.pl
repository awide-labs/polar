# 017_outdate_fault_in_before_parse.pl
#	  A backend that starts faulting a page in *before* the startup process
#	  parses a record for it must still not lose that record.
#
# Test 016 covers the other order: the startup's buffer table lookup misses
# and publishes, and only then does a backend insert the page. Here the
# backend inserts first, so the startup's lookup lands on the placeholder id
# BufferAlloc leaves in the buffer table while it sets the descriptor up
# (bufmgr.c). A placeholder is negative, so polar_logindex_outdate_parse
# treats it as a miss and never marks a descriptor -- and the backend that
# owns the placeholder reads the slot only after its page read, so it must
# still see the publication made in between.
#
# Without the in-flight publication covering this window, the read-in replay
# is bounded by a frontier that predates the record, the buffer goes valid
# with OUTDATE clear, and the final assertion sees the stale value.
#
# IDENTIFICATION
#	  src/test/polar_consistency/t/017_outdate_fault_in_before_parse.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_fault_injector} eq 'no')
{
	plan skip_all => 'Fault injector not supported by this build';
}

my $db = 'postgres';

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;
$node_primary->append_conf('postgresql.conf', 'synchronous_commit=on');
# keep background WAL out of the way of the occurrence-counted faults
$node_primary->append_conf('postgresql.conf', 'autovacuum=off');

my $node_replica = PostgreSQL::Test::Cluster->new('replica1');
$node_replica->polar_init_replica($node_primary);

$node_primary->start;
$node_primary->polar_create_slot($node_replica->name);

$node_primary->safe_psql($db, 'create extension faultinjector;');
$node_primary->safe_psql($db,
	'create table test(id int primary key, val text);');
$node_primary->safe_psql($db, "insert into test values(1, 'old');");
$node_primary->safe_psql($db, 'checkpoint;');

# The heap page is held by relfilenode, so the fault below fires for it and
# not for the index pages the lookup walks on its way there.
my $relfilenode = $node_primary->safe_psql($db,
	"select relfilenode from pg_class where relname = 'test';");
print "test relfilenode: $relfilenode\n";

$node_replica->start;

my $lsn = $node_primary->safe_psql($db, 'select pg_current_wal_insert_lsn();');
$node_replica->poll_query_until($db,
	"select pg_last_wal_replay_lsn() >= '$lsn'::pg_lsn")
  or die 'replica did not catch up after initial load';

# Deliberately no replica read of the page: it must still be unbuffered when
# the fault-in below starts.

# Once the page is in, background replay would apply the record and mask a
# lost flag.
my $result = $node_replica->safe_psql($db,
	"select inject_fault('polar_logindex_bg_skip_replay', 'enable', '', '', 1, -1, -1);"
);
print "enable polar_logindex_bg_skip_replay: $result\n";

# Proves the arming happened on this interleaving rather than through the
# buffered path.
$result = $node_replica->safe_psql($db,
	"select inject_fault('polar_in_flight_buffer_arm', 'enable', '', '', 1, -1, -1);"
);
print "enable polar_in_flight_buffer_arm: $result\n";

# Park a backend between the buffer table insert and the descriptor setup.
$result = $node_replica->safe_psql($db,
	"select inject_fault('polar_stall_buffer_insert', 'suspend', '', '$relfilenode', 1, 1, 0);"
);
print "suspend polar_stall_buffer_insert: $result\n";

# Fire the read that faults the page in; it parks holding the placeholder.
my $reader = $node_replica->background_psql($db, on_error_stop => 0);
$reader->{stdin} .= "select val from test where id = 1;\n";
$reader->{run}->pump_nb();

$result = $node_replica->safe_psql($db,
	"select wait_until_triggered_fault('polar_stall_buffer_insert', 1);");
print "reader parked inside the fault-in: $result\n";

# Suspend the startup after the parse and before it publishes the frontier,
# so the record stays uncovered while the reader finishes.
$result = $node_replica->safe_psql($db,
	"select inject_fault('polar_stall_replay_frontier_publish', 'suspend', '', '', 1, 1, 0);"
);
print "suspend polar_stall_replay_frontier_publish: $result\n";

$node_primary->safe_psql($db, "update test set val = 'new' where id = 1;");

$result = $node_replica->safe_psql($db,
	"select wait_until_triggered_fault('polar_stall_replay_frontier_publish', 1);"
);
print "startup parsed the record and suspended: $result\n";

# The startup's lookup has now seen our placeholder and published instead of
# marking a descriptor. Let the reader finish its page read.
$result = $node_replica->safe_psql($db,
	"select inject_fault('polar_stall_buffer_insert', 'resume');");
print "resume polar_stall_buffer_insert: $result\n";
$result = $node_replica->safe_psql($db,
	"select inject_fault('polar_stall_buffer_insert', 'reset');");
print "reset polar_stall_buffer_insert: $result\n";

# Waiting for the reader's own result, rather than for the arm fault, keeps a
# missing arm out of the critical path: it then shows up as the stale value
# below instead of ten minutes inside wait_until_triggered_fault.
my $read = $reader->query_safe("select 1;");
print "reader finished its fault-in: $read\n";

$result = $node_replica->safe_psql($db,
	"select inject_fault('polar_stall_replay_frontier_publish', 'resume');");
print "resume: $result\n";
$result = $node_replica->safe_psql($db,
	"select inject_fault('polar_stall_replay_frontier_publish', 'reset');");
print "reset: $result\n";

$lsn = $node_primary->safe_psql($db, 'select pg_current_wal_insert_lsn();');
$node_replica->poll_query_until($db,
	"select pg_last_wal_replay_lsn() >= '$lsn'::pg_lsn")
  or die 'replica did not catch up after resume';

# The regression assertion: the record parsed while this backend held the
# placeholder must be applied once the frontier passes it.
$result = $node_replica->safe_psql($db, 'select val from test where id = 1;');
is($result, 'new',
	'record parsed during a concurrent fault-in is applied after catchup');

$reader->quit;

# And that it got there through the fault-in arm, not the buffered path.
$result = $node_replica->safe_psql($db,
	"select inject_fault('polar_in_flight_buffer_arm', 'status');");
print "arm fault status: $result\n";
like($result, qr/num times hit:'[1-9]/,
	'the watermark was armed by the concurrent fault-in');

$result = $node_replica->safe_psql($db,
	"select inject_fault('polar_in_flight_buffer_arm', 'reset');");
print "reset polar_in_flight_buffer_arm: $result\n";
$result = $node_replica->safe_psql($db,
	"select inject_fault('polar_logindex_bg_skip_replay', 'reset');");
print "reset polar_logindex_bg_skip_replay: $result\n";

$node_replica->stop;
$node_primary->stop;

done_testing();
