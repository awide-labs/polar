#!/usr/bin/perl
#
# 053_polar_guc_configuration_discovery.pl
#
# Selected PolarDB operational and diagnostic settings must stay discoverable
# through `postgres --describe-config`: Patroni uses this output to accept
# parameters without built-in validators from DCS configuration, then queries
# pg_settings when comparing configuration changes.  Pin both discovery paths
# while keeping internal, test-only, and inactive settings out of discovery.
#
# Flag semantics being exercised:
#   --describe-config omits GUCs carrying GUC_NO_SHOW_ALL, GUC_NOT_IN_SAMPLE
#   or GUC_DISALLOW_IN_FILE (help_config.c); pg_settings/SHOW ALL omit only
#   GUC_NO_SHOW_ALL (and SUPERUSER-only rows for unprivileged roles).
#
# IDENTIFICATION
#	  src/test/polar_pl/t/053_polar_guc_configuration_discovery.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Polar GUCs newly exposed by this change (sorted), including operational
# controls and diagnostics that operators need to configure through Patroni.
my @exposed = (
	'polar_apply_global_guc_for_super',
	'polar_auto_cascade_extensions',
	'polar_clog_max_local_cache_segments',
	'polar_commit_ts_max_local_cache_segments',
	'polar_datamax_prealloc_walfile_num',
	'polar_enable_async_lock_replay',
	'polar_enable_async_lock_replay_debug',
	'polar_enable_coredump_cleanup',
	'polar_enable_coredump_print',
	'polar_enable_debug',
	'polar_enable_debug_proxy',
	'polar_enable_error_to_audit_log',
	'polar_enable_fullpage_snapshot',
	'polar_enable_multi_syslogger',
	'polar_enable_output_search_path_to_log',
	'polar_enable_persisted_logical_slot',
	'polar_enable_promote_wait_for_walreceive_done',
	'polar_enable_replica_copydata_optimization',
	'polar_enable_ro_resolve_conflict',
	'polar_enable_shared_storage_mode',
	'polar_enable_simply_redo_error_log',
	'polar_enable_stat_wait_info',
	'polar_enable_sync_ddl',
	'polar_enable_sync_ddl_legacy',
	'polar_enable_syslog_file_buffer',
	'polar_enable_syslog_pipe_buffer',
	'polar_enable_tde_warning',
	'polar_enable_track_lock_stat',
	'polar_enable_track_lock_timing',
	'polar_enable_track_network_stat',
	'polar_enable_track_network_timing',
	'polar_enable_xact_split_debug',
	'polar_force_trans_ro_non_sup',
	'polar_force_unlogged_to_logged_table',
	'polar_fullpage_keep_segments',
	'polar_fullpage_snapshot_min_modified_count',
	'polar_fullpage_snapshot_oldest_lsn_delay_threshold',
	'polar_fullpage_snapshot_replay_delay_threshold',
	'polar_has_partial_write',
	'polar_hold_truncate_interrupt',
	'polar_log_statement_with_duration',
	'polar_logindex_replay_delay_threshold',
	'polar_max_auditlog_files',
	'polar_max_log_files',
	'polar_max_logindex_files',
	'polar_max_non_super_conns',
	'polar_max_non_super_wal_snd',
	'polar_max_slowlog_files',
	'polar_max_super_conns',
	'polar_multixact_max_local_cache_segments',
	'polar_rsc_pool_sweep_times',
	'polar_rsc_shared_relations',
	'polar_suppress_preload_error',
	'polar_trace_logindex_messages',
	'polar_wait_old_version_page_timeout',
	'polar_wal_buffer_reserved_old_pages',
	'polar_wal_buffer_reserved_old_pages_ratio',
	'polar_write_logindex_active_table_delay',
	'polar_zero_extend_method'
);

# Polar GUCs that deliberately stay hidden, as fixed sets derived from
# guc_tables.c.  GUC_NO_SHOW_ALL hides a GUC from pg_settings/SHOW ALL;
# --describe-config additionally omits GUCs carrying GUC_NOT_IN_SAMPLE or
# GUC_DISALLOW_IN_FILE (11 PolarDB GUCs carry the latter as well, on top of
# GUC_NOT_IN_SAMPLE).  These are provisioning hints, test controls, unsafe
# recovery overrides, crash-handling overrides, internal preloads, session
# state, read-only metadata, maintenance actions, and an inactive setting.
my @hidden_from_pg_settings = (
	'polar_instance_spec_cpu',
	'polar_instance_spec_mem',
	'polar_query_delay_us',
	'polar_startup_replay_delay_size'
);

my @hidden_from_describe_config = (
	@hidden_from_pg_settings,
	'polar_cancel_key',
	'polar_commit',
	'polar_data_encryption_cipher',
	'polar_deploy_mode',
	# Registered for compatibility, but has no behavioral consumer.
	'polar_enable_standby_instant_recovery',
	'polar_force_change_checkpoint',
	'polar_force_flush_buffer',
	'polar_ignore_coredump_functions',
	'polar_ignore_coredump_fuzzy_match',
	'polar_ignore_coredump_level',
	'polar_ignore_ro_latency',
	'polar_internal_shared_preload_libraries',
	'polar_pfsd_version',
	'polar_release_date',
	'polar_rename_wal_ready_file',
	'polar_version',
	'polar_virtual_pid',
	'polar_xact_split_wait_lsn',
	'polar_xact_split_xids'
);

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->polar_init_primary;
$node_primary->start;

my $all_names = join ',', map { "'$_'" } @exposed;

# --describe-config must be the first argument: main.c intercepts it before
# option parsing, while postmaster's getopt treats any later "--long" word as
# a --NAME=VALUE GUC assignment and rejects it.  Patroni invokes it the same
# way; the dump never touches a data directory.
my ($describe_out, $describe_err) = ('', '');
IPC::Run::run [ 'postgres', '--describe-config' ],
  '>', \$describe_out, '2>', \$describe_err
  or die "postgres --describe-config failed: $describe_err";
ok(1, 'postgres --describe-config exited successfully');

my %listed;
for my $line (split /\n/, $describe_out)
{
	my ($name) = split /\t/, $line, 2;
	$listed{$name} = 1 if defined $name;
}

for my $name (@exposed)
{
	ok($listed{$name}, "\"$name\" is listed by --describe-config");
}

my %hidden_pgs = map { $_ => 1 } @hidden_from_pg_settings;
my %hidden_dc  = map { $_ => 1 } @hidden_from_describe_config;
ok(!grep($hidden_pgs{$_} || $hidden_dc{$_}, @exposed),
	'exposed GUCs do not overlap the hidden sets');

# None of the exposed GUCs may carry GUC_NOT_IN_SAMPLE.
my $not_in_sample = $node_primary->safe_psql('postgres', qq{
	SELECT string_agg(name, ',') FROM unnest(ARRAY[$all_names]) AS g(name)
	WHERE 'NOT_IN_SAMPLE' = ANY (pg_settings_get_flags(g.name));
  });
is($not_in_sample, '', "no exposed GUC carries GUC_NOT_IN_SAMPLE");

# The regression role is a superuser, so GUC_SUPERUSER_ONLY does not hide
# settings such as polar_suppress_preload_error.  Every exposed GUC must be
# visible in pg_settings, which also excludes GUC_NO_SHOW_ALL settings.
my $visible_count = $node_primary->safe_psql('postgres',
	"SELECT count(*) FROM pg_settings WHERE name = ANY (ARRAY[$all_names]);"
);
is($visible_count, scalar @exposed,
	"all exposed GUCs are visible in pg_settings");

# 1. All polar GUCs shown by pg_settings, compared against the fixed set of
# GUCs hidden from pg_settings: none of them may leak.
my @polar_in_pg_settings =
  split /\n/, $node_primary->safe_psql('postgres',
	"SELECT name FROM pg_settings WHERE name LIKE 'polar\\_%' ORDER BY 1");
my %in_pg_settings = map { $_ => 1 } @polar_in_pg_settings;
my @pgs_leaks = sort grep { $hidden_pgs{$_} } @polar_in_pg_settings;
is(join(',', @pgs_leaks), '',
	'pg_settings shows no polar GUC from the fixed hidden set');
ok(!grep(!$in_pg_settings{$_}, @exposed),
	'pg_settings shows every exposed polar GUC');

# 2. All polar GUCs listed by --describe-config, compared against the fixed
# set of GUCs hidden from describe-config: none of them may leak.
my @polar_in_describe = grep { /^polar_/ } keys %listed;
my %in_describe = map { $_ => 1 } @polar_in_describe;
my @dc_leaks = sort grep { $hidden_dc{$_} } @polar_in_describe;
is(join(',', @dc_leaks), '',
	'--describe-config lists no polar GUC from the fixed hidden set');
ok(!grep(!$in_describe{$_}, @exposed),
	'--describe-config lists every exposed polar GUC');

$node_primary->stop;
done_testing();
