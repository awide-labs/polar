# Test for datamax feedback standby xmin
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use Test::More tests=>13;

# master
my $node_master = PostgreSQL::Test::Cluster->new('master');
$node_master->polar_init_primary();
my $primary_system_identifier = $node_master->polar_get_system_identifier;

# datamax
my $node_datamax = PostgreSQL::Test::Cluster->new('datamax');
$node_datamax->polar_init_datamax($primary_system_identifier);

# standby
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->polar_init_standby_no_recovery();
$node_standby->polar_set_root_node($node_master);
$node_standby->polar_standby_build_data;
$node_standby->polar_standby_set_recovery($node_datamax);

$node_master->append_conf('postgresql.conf', "synchronous_commit = on");
$node_master->append_conf('postgresql.conf', "synchronous_standby_names='".$node_datamax->name."'");

$node_master->start;
$node_master->polar_create_slot($node_datamax->name);

# The datamax was provisioned with the master's system identifier via
# "initdb -i", so it first starts as a writable standalone node: create the
# downstream standby's slot and the polar_monitor extension here, then stop
# and flip it into datamax mode. (PM_DATAMAX rejects DDL, so the extension
# must exist before set_recovery.)
$node_datamax->start;
$node_datamax->polar_create_slot($node_standby->name);
$node_datamax->safe_psql('postgres','CREATE EXTENSION polar_monitor;');
# Freeze so polar_get_datamax_info() stays resolvable after the datamax
# node is restarted below.
$node_datamax->safe_psql('postgres','VACUUM (FREEZE);');
$node_datamax->stop;

# set datamax recovery config
$node_datamax->polar_datamax_set_recovery($node_master);
$node_datamax->start;
$node_standby->start;

# Make the primary advance to a new LSN and wait for $follower to catch up.
# Returns 1 on success, dies on timeout. The follower is cascaded through the
# datamax, so poll the follower's replayed lsn directly instead of going via
# pg_stat_replication on the primary.
sub replay_check
{
	my ($follower) = @_;

	# Generate a tiny amount of WAL on the master and wait for it to travel the
	# two-hop datamax relay (master -> datamax -> follower). Polling for the row
	# itself (rather than pg_switch_wal() + an LSN compare) avoids forcing a full
	# WAL segment through the relay on every call and sidesteps the
	# insert-vs-replay LSN page-header race.
	my $newval = $node_master->safe_psql('postgres',
		'INSERT INTO replayed(val) SELECT coalesce(max(val),0) + 1 FROM replayed RETURNING val');

	$follower->poll_query_until('postgres',
		"SELECT EXISTS (SELECT 1 FROM replayed WHERE val = $newval)")
	  or die "timed out waiting for follower to replay value $newval";

	return 1;
}

sub get_slot_xmins
{
    my ($node, $slotname, $check_expr) = @_;

	$node->poll_query_until(
		'postgres', qq[
		SELECT $check_expr
		FROM pg_catalog.pg_replication_slots
		WHERE slot_name = '$slotname';
	]) or die "Timed out waiting for slot xmins to advance";

	my $slotinfo = $node->slot($slotname);
	return ($slotinfo->{'xmin'});
}

sub wait_xmin_update_ready
{
	my ($node, $slotname, $xmin_expected) = @_;

	$node->poll_query_until(
		'postgres', qq[
		SELECT xmin::text::int >= $xmin_expected 
		FROM pg_catalog.pg_replication_slots
		WHERE slot_name = '$slotname';
	]) or die "Timed out waiting for slot xmins ready";
}

sub insert_data
{
	my ($count) = @_;
	my $i = 0;
	for($i = 0; $i < $count; $i = $i + 1)
	{
		replay_check($node_standby);
	}
	return;
}

$node_master->safe_psql('postgres', 'CREATE TABLE replayed(val integer);');

# 1. disable hot_standby_feedback on datamax and standby
note "disable hot_standby_feedback on datamax and standby";
$node_standby->safe_psql('postgres', 'ALTER SYSTEM SET hot_standby_feedback = off;');
$node_standby->reload;
$node_datamax->safe_psql('postgres', 'ALTER SYSTEM SET hot_standby_feedback = off;');
$node_datamax->reload;
my $replay_ret = replay_check($node_standby);
ok($replay_ret == 1, "standby replay data success");
my $datamax_xmin = get_slot_xmins($node_datamax, $node_standby->name, "xmin IS NULL");
my $master_xmin = get_slot_xmins($node_master, $node_datamax->name, "xmin IS NULL");
is($datamax_xmin, '', 'datamax xmin is null when feedback is disabled both on datamax and standby');
is($master_xmin, '', 'master xmin is null when feedback is disabled both on datamax and standby');

# 2. enable hot_standby_feedback on datamax and standby
note "enable hot_standby_feedback on datamax and standby";
$node_standby->safe_psql('postgres', 'ALTER SYSTEM SET hot_standby_feedback = on;');
$node_standby->reload;
$node_datamax->safe_psql('postgres', 'ALTER SYSTEM SET hot_standby_feedback = on;');
$node_datamax->reload;
$replay_ret = 0;
$replay_ret = replay_check($node_standby);
ok($replay_ret == 1, "standby replay data success");
my $standby_ret = $node_standby->safe_psql('postgres',"select txid_current_snapshot();");
my($standby_xmin, $standby_xmax, $standby_xip) = split /:/, $standby_ret;
wait_xmin_update_ready($node_datamax, $node_standby->name, $standby_xmin);
wait_xmin_update_ready($node_master, $node_datamax->name, $standby_xmin);
$datamax_xmin = get_slot_xmins($node_datamax, $node_standby->name, "xmin IS NOT NULL");
$master_xmin = get_slot_xmins($node_master, $node_datamax->name, "xmin IS NOT NULL");
print "standby_xmin: $standby_xmin, datamax_xmin: $datamax_xmin, master_xmin: $master_xmin\n";
ok($datamax_xmin <= $standby_xmin, 'datamax record standby xmin when feedback is enabled both on datamax and standby');
ok($master_xmin >= $standby_xmin, 'datamax feedback standby xmin to master when feedback is enabled both on datamax and standby');

# 3. enable hot_standby_feedback on datamax, disable hot_standby_feedback on standby
note "enable hot_standby_feedback on datamax, disable hot_standby_feedback on standby";
$node_standby->safe_psql('postgres', 'ALTER SYSTEM SET hot_standby_feedback = off;');
$node_standby->reload;
$node_datamax->safe_psql('postgres', 'ALTER SYSTEM SET hot_standby_feedback = on;');
$node_datamax->reload;
$replay_ret = 0;
$replay_ret = replay_check($node_standby);
ok($replay_ret == 1, "standby replay data success");
$datamax_xmin = get_slot_xmins($node_datamax, $node_standby->name, "xmin IS NULL");
$master_xmin = get_slot_xmins($node_master, $node_datamax->name, "xmin IS NULL");
# because datamax feedback standby xmin to master, so it is null when feedback is disabled on standby
is($datamax_xmin, '', 'datamax xmin is null when feedback is enabled on datamax but disabled on standby');
is($master_xmin, '', 'master xmin is null when feedback is enabled on datamax but disabled on standby');

# 4. disable hot_standby_feedback on datamax, enable hot_standby_feedback on standby
note "disable hot_standby_feedback on datamax, enable hot_standby_feedback on standby";
$node_standby->safe_psql('postgres', 'ALTER SYSTEM SET hot_standby_feedback = on;');
$node_standby->reload;
$node_datamax->safe_psql('postgres', 'ALTER SYSTEM SET hot_standby_feedback = off;');
$node_datamax->reload;
$replay_ret = 0;
$replay_ret = replay_check($node_standby);
ok($replay_ret == 1, "standby replay data success");
$standby_ret = $node_standby->safe_psql('postgres',"select txid_current_snapshot();");
($standby_xmin, $standby_xmax, $standby_xip) = split /:/, $standby_ret;
wait_xmin_update_ready($node_datamax, $node_standby->name, $standby_xmin);
$datamax_xmin = get_slot_xmins($node_datamax, $node_standby->name, "xmin IS NOT NULL");
$master_xmin = get_slot_xmins($node_master, $node_datamax->name, "xmin IS NULL");
ok($datamax_xmin <= $standby_xmin, 'datamax record standby xmin when feedback is enabled on standby but disabled on datamax');
is($master_xmin, '', 'master xmin is null when feedback is enabled on standby but disabled on datamax');

# test for datamax starting streaming from last valid lsn
# loop for insert data, so that datamax can update last valid lsn 
$node_master->safe_psql('postgres', "select pg_switch_wal();");	
insert_data(10);

# restart datamax to parse xlog
$node_datamax->restart;
my $write_lsn = $node_master->lsn('write');
print "master write lsn: $write_lsn\n";
my $datamax_lsn = $node_datamax->safe_psql('postgres',
		"SELECT last_valid_received_lsn FROM polar_get_datamax_info()");
chomp($datamax_lsn);
print "datamax_lsn: $datamax_lsn\n";
ok($write_lsn le $datamax_lsn, "last_valid_received_lsn of datamax is eaqul to the write lsn of master");

$node_standby->stop;
$node_datamax->stop;
$node_master->stop;