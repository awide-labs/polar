# 012_wake_up_startup_after_each_xlog_meta.pl
#	  Test for wake up startup after each xlog meta.
#
# Copyright (c) 2024, Alibaba Group Holding Limited
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# IDENTIFICATION
#	  src/test/polar_consistency/t/012_wake_up_startup_after_each_xlog_meta.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(time usleep);
srand();
$ENV{'PG_TEST_NOCLEAN'} = 1;

my $loop_count = 1000;
my $wait_xlog_meta_timeout = 12;
my $db = 'postgres';

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;

my $node_replica = PostgreSQL::Test::Cluster->new('replica1');
$node_replica->polar_init_replica($node_primary);

$node_primary->append_conf('postgresql.conf',
	"synchronous_standby_names='" . $node_replica->name . "'");
$node_primary->append_conf('postgresql.conf', "synchronous_commit=on");
$node_primary->append_conf('postgresql.conf', "checkpoint_timeout=60000");

$node_primary->start;
$node_primary->polar_create_slot($node_replica->name);

$node_replica->append_conf('postgresql.conf',
	"polar_wait_xlog_meta_timeout=${wait_xlog_meta_timeout}s");
$node_replica->start;

$node_primary->safe_psql($db, "create extension polar_monitor;");
$node_primary->wait_for_catchup($node_replica);

my $replica_psql = $node_replica->background_psql($db);
$replica_psql->set_query_timer_restart();

sub run_ddl
{
	my ($node) = @_;
	my $sql_loop = int(rand(5)) + 1;
	my $ddl_sql = "
		DO \$\$
		DECLARE
		counter integer := 1;
		BEGIN
		RAISE LOG 'beging tmp test';
		FOR counter IN 1..$sql_loop LOOP
			EXECUTE 'CREATE TEMP TABLE temp_table_' || counter || ' (id serial, name text) ON COMMIT DELETE ROWS';
		END LOOP;
		RAISE LOG 'end tmp test';
		END;
		\$\$;
	";

	my ($stdout, $stderr) = ('', '');
	my $h = IPC::Run::start(
		[
			'psql', '-XAtq', '-v', 'ON_ERROR_STOP=1',
			'-d', $node->connstr($db), '-c', $ddl_sql
		],
		'>', \$stdout, '2>', \$stderr,
		IPC::Run::timeout($PostgreSQL::Test::Utils::timeout_default));

	# The DDL waits until the replica replays its lock records. Measure for
	# how long the replica startup process sleeps although the walreceiver has
	# already received WAL it has not replayed: that is a missed wakeup.
	# Measuring the DDL time instead would also count slow WAL writes on a
	# loaded disk, which can take tens of seconds.
	#
	# The received LSN is the primary's flush LSN. A flush that stops in the
	# middle of a record writes out whole WAL pages and ends on a page
	# boundary; until the rest of that record is flushed, the replica has
	# nothing to replay. Any other flush LSN ends a record, so all WAL up to
	# it can be replayed.
	my $max_sleep = 0;
	my ($sleep_lsn, $sleep_start);
	while ($h->pumpable)
	{
		my $lsn = $replica_psql->query_safe(
			"SELECT pg_last_wal_replay_lsn() FROM pg_stat_activity
			  WHERE backend_type = 'startup'
				AND wait_event = 'RecoveryWalStream'
				AND pg_last_wal_receive_lsn() > pg_last_wal_replay_lsn()
				AND (pg_last_wal_receive_lsn() - '0/0')
					% current_setting('wal_block_size')::int <> 0");
		my $now = time();

		if ($lsn eq '')
		{
			$sleep_lsn = undef;
		}
		elsif (!defined $sleep_lsn || $lsn ne $sleep_lsn)
		{
			($sleep_lsn, $sleep_start) = ($lsn, $now);
		}
		elsif ($now - $sleep_start > $max_sleep)
		{
			$max_sleep = $now - $sleep_start;
		}

		$h->pump_nb;
		usleep(20_000);
	}
	$h->finish or die "psql failed: $stderr";

	return $max_sleep;
}

for (my $i = 0; $i < $loop_count; $i++)
{
	my $max_sleep = run_ddl($node_primary);
	ok( $max_sleep < $wait_xlog_meta_timeout / 4,
		sprintf("startup slept %.1fs with unreplayed WAL", $max_sleep));
}

$replica_psql->quit;
$node_primary->stop();
$node_replica->stop();

done_testing();
