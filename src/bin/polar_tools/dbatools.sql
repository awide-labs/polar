set client_min_messages to 'warning' ;
create table if not exists  polar_tools_info (name text primary key, example text, memo text, category text);
create or replace procedure polar_load_tool_comment(
       p_name text,
       p_example text,
       p_memo text,
       p_category text
)
language plpgsql
as $$
begin
    insert into polar_tools_info values (p_name, p_example, p_memo, p_category)
    on conflict (name)
    do
    update set memo =excluded.memo, category = excluded.category, example = excluded.example;
    commit;
end;$$
;
create extension if not exists pg_stat_statements;
reset client_min_messages;

create or replace view polar_tools as
select p.name, statement, category, memo, example
from pg_catalog.pg_prepared_statements p left join polar_tools_info o
on p.name = o.name
order by category, prepare_time;

-- Start to load our tool here, please put the tools with similar function together
prepare list_command as
select
name,
category,
memo,
example,
case when $1 = 1 then statement else '...' end as statement
from polar_tools;
call polar_load_tool_comment(
'list_command',
'execute list_command(0);',
'List all the avaiable commands. input 1 shows the real SQL.',
'Help');

prepare command_stats as
select
category,
count(*),
string_agg(name, '
') as cmds
from polar_tools
group by category
order by 1;
call polar_load_tool_comment(
'command_stats',
'Show supported command stats;',
'Show the summary stats for supported commands.',
'Help');

prepare list_command_by_category as
select * from polar_tools where category = $1;
call polar_load_tool_comment('list_command_by_category',
'execute list_command_by_category(''Monitor'');',
'See all the suppored category with `command_stats`.',
'Help');

-- Some Monitor commands.

prepare wal_size_check as
select rownum as seq, * from (
select 'wal_keep_segments = ' || n_segs
       || ' and wal_segment_size = ' || wal_size
       || ', So we can have up to ' || n_segs
       || ' * ' || wal_size
       ||  ' Wal files based on this settings.' as sistuation,
       'decrease the value of `wal_keep_segments`' as Action
from (select current_setting('wal_keep_segments') as n_segs,
      current_setting('wal_segment_size') as wal_size)
UNION ALL
select 'slot name ' || slot_name || ' is ' ||
case when active then 'active ' else 'inactive ' end ||
'whose restart_scn is ' || restart_lsn::text ||
' which will keep wal size ' ||
pg_size_pretty(pg_wal_lsn_diff(pg_current_wal_lsn(), restart_lsn))::text
|| ' at least.',
'Either fix the replication delay or drop it with
select pg_drop_replication_slot(''' ||
slot_name || ''');
once you know the slot is not needed'
from pg_replication_slots
where
(active = 'f'
or
pg_wal_lsn_diff(pg_current_wal_lsn(), restart_lsn) > 1024 * 1024 * 1024)
AND
restart_lsn is not null
) t;

call polar_load_tool_comment(
'wal_size_check', 'execute wal_size_check;',
'Used to check why the wal size is so big.',
'Alert');


prepare active_session_by_waitevent as
select wait_event, count(*)
from pg_stat_activity
where state = 'active'
group by wait_event
UNION ALL
SELECT 'TOTAL:', count(*)
from pg_stat_activity
where state = 'active'
;

call polar_load_tool_comment(
'active_session_by_waitevent',
'execute active_session_by_waitevent;',
'Show the wait event info',
'Connection');


prepare connections_by_clients as
select client_addr::text, count(*) from pg_stat_activity
group by client_addr::text
union all
select 'Total Conn:', count(*) from pg_stat_activity
;

call polar_load_tool_comment('connections_by_clients',
'execute connections_by_clients;', 'Show total connectioin info.', 'Connection');

prepare query_locking as
with
t_wait as
(
select a.mode,a.locktype,a.database,a.relation,a.page,a.tuple,a.classid,a.granted,
a.objid,a.objsubid,a.pid,a.virtualtransaction,a.virtualxid,a.transactionid,a.fastpath,
b.state,b.query,b.xact_start,b.query_start,b.usename,b.datname,b.client_addr,b.client_port,b.application_name
from pg_locks a,pg_stat_activity b where a.pid=b.pid and not a.granted
),
t_run as
(
select a.mode,a.locktype,a.database,a.relation,a.page,a.tuple,a.classid,a.granted,
a.objid,a.objsubid,a.pid,a.virtualtransaction,a.virtualxid,a.transactionid,a.fastpath,
b.state,b.query,b.xact_start,b.query_start,b.usename,b.datname,b.client_addr,b.client_port,b.application_name
from pg_locks a,pg_stat_activity b where a.pid=b.pid and a.granted
),
t_overlap as
(
select distinct r.* from t_wait w join t_run r on
(
r.locktype is not distinct from w.locktype and
r.database is not distinct from w.database and
r.relation is not distinct from w.relation and
r.page is not distinct from w.page and
r.tuple is not distinct from w.tuple and
r.virtualxid is not distinct from w.virtualxid and
r.transactionid is not distinct from w.transactionid and
r.classid is not distinct from w.classid and
r.objid is not distinct from w.objid and
r.objsubid is not distinct from w.objsubid and
r.pid <> w.pid
)
),
t_unionall as
(
select r.* from t_overlap r
union all
select w.* from t_wait w
)
select locktype,datname,relation::regclass,page,tuple,virtualxid,transactionid::text,classid::regclass,objid,objsubid,
string_agg(
'Pid: '||case when pid is null then 'NULL' else pid::text end||chr(10)||
'Lock_Granted: '||case when granted is null then 'NULL' else granted::text end||
' , Mode: '||case when mode is null then 'NULL' else mode::text end||
' , FastPath: '||case when fastpath is null then 'NULL' else fastpath::text end||
' , VirtualTransaction: '||case when virtualtransaction is null then 'NULL' else virtualtransaction::text end||
' , Session_State: '||case when state is null then 'NULL' else state::text end||chr(10)||
'Username: '||case when usename is null then 'NULL' else usename::text end||
' , Database: '||case when datname is null then 'NULL' else datname::text end||
' , Client_Addr: '||case when client_addr is null then 'NULL' else client_addr::text end||
' , Client_Port: '||case when client_port is null then 'NULL' else client_port::text end||
' , Application_Name: '||case when application_name is null then 'NULL' else application_name::text end||chr(10)||
'Xact_Start: '||case when xact_start is null then 'NULL' else xact_start::text end||
' , Query_Start: '||case when query_start is null then 'NULL' else query_start::text end||
' , Xact_Elapse: '||case when (now()-xact_start) is null then 'NULL' else (now()-xact_start)::text end||
' , Query_Elapse: '||case when (now()-query_start) is null then 'NULL' else (now()-query_start)::text end||chr(10)||
'SQL (Current SQL in Transaction): '||chr(10)|| case when query is null then 'NULL' else query::text end,
chr(10)||'--------'||chr(10)
order by
(  case mode
when 'INVALID' then 0
when 'AccessShareLock' then 1
when 'RowShareLock' then 2
when 'RowExclusiveLock' then 3
when 'ShareUpdateExclusiveLock' then 4
when 'ShareLock' then 5
when 'ShareRowExclusiveLock' then 6
when 'ExclusiveLock' then 7
when 'AccessExclusiveLock' then 8
else 0
end  ) desc,
(case when granted then 0 else 1 end)
) as lock_conflict
from t_unionall
group by
locktype,datname,relation,page,tuple,virtualxid,transactionid::text,classid,objid,objsubid ;

call polar_load_tool_comment('query_locking', 'execute query_locking;', 'Query the locking state', 'Locking');


prepare top_n_long_queries as
select datname, usename, application_name, client_addr, state,
wait_event, left(query,40) query,
query_start,now()-query_start as Elapsed
from pg_stat_activity
where state<>'idle'
and (backend_xid is not null or backend_xmin is not null)
order by now()-query_start desc
limit $1;

call polar_load_tool_comment(
'top_n_long_queries',
'execute top_n_long_queries(10);',
'Query the top $1 long running query',
'Monitor');


prepare top_n_long_txn as
select datname, usename, application_name, client_addr, state,
wait_event, left(query,40) query,
xact_start,now()-xact_start as Elapsed
from pg_stat_activity
where state<>'idle'
and (backend_xid is not null or backend_xmin is not null)
order by now()-query_start desc
limit $1;

call polar_load_tool_comment(
'top_n_long_txn',
'execute top_n_long_txn(10);',
'Query the top $1 long running transactions',
'Monitor');

prepare top_n_long_queries_in_hist as
select left(query,50),mean_time, min_time, max_time, calls
from pg_stat_statements
order by mean_time, calls desc
limit $1;

call polar_load_tool_comment('top_n_long_queries_in_hist',
'execute top_n_long_queries_in_hist(10);',
'Query the top-n long queries in history, you can use
`select pg_stat_statements_reset();`
to reset history statistics',
'Query');


prepare query_recovery_info as
select *,
pg_size_pretty(pg_wal_lsn_diff(WP, AP)) as "WP-AP",
pg_size_pretty(pg_wal_lsn_diff(AP, CP)) as "AP-CP",
pg_size_pretty(pg_wal_lsn_diff(WP, CP)) as "WP-CP",
pg_size_pretty(pg_wal_lsn_diff(WP, redo_lsn)) as "WP-REDO"
from (
select pg_current_wal_insert_lsn() as WP,
       polar_oldest_apply_lsn() as AP,
       polar_consistent_lsn() as CP,
       redo_lsn from pg_control_checkpoint()) as lsn_info;

call polar_load_tool_comment(
'query_recovery_info',
'execute query_recovery_info;',
'Show the checkpoint, constistent reocvery info.',
'Recovery');


prepare top_n_dead_tuple_ratio_tables as
SELECT
schemaname||'.'||relname as table_name,
pg_size_pretty(pg_relation_size(schemaname||'.'||relname)) as table_size,
n_dead_tup,
n_live_tup,
round(n_dead_tup * 100 / (n_live_tup + n_dead_tup),2) AS dead_tup_ratio,
last_vacuum,
last_autovacuum
FROM
pg_stat_all_tables
WHERE
n_dead_tup >= 1000
ORDER BY dead_tup_ratio DESC
limit $1
;

call polar_load_tool_comment(
'top_n_dead_tuple_ratio_tables',
'execute top_n_dead_tuple_ratio_tables(10);',
'List Top-N tables with the biggest dead tuple ratio,
run `vacuum {tablename};` if necessary',
'Vacuum');

prepare miss_optimizer_stats as
SELECT
schemaname||'.'||t.relname as table_name,
last_vacuum,
last_autovacuum,
last_analyze,
last_autoanalyze
from pg_stat_all_tables t, pg_class c
where last_analyze is null
and last_autoanalyze is null
and c.oid = t.relid
and c.relkind in ('p', 'r')
and relnamespace::regnamespace::text != 'pg_catalog'
;

call polar_load_tool_comment(
'miss_optimizer_stats',
'execute miss_optimizer_stats;',
'Check which tables are missing optmizer statistics, you can use
exec gen_cmd_fix_miss_stats; to fix the issues',
'Optimizer');

prepare gen_cmd_fix_miss_stats as
select 'analyze ' || schemaname || '.' || t.relname || ';'
from pg_stat_all_tables t, pg_class c
where last_analyze is null
and last_autoanalyze is null
and c.oid = t.relid
and c.relkind in ('p', 'r')
and relnamespace::regnamespace::text != 'pg_catalog'
;

call polar_load_tool_comment('gen_cmd_fix_miss_stats',
'execute gen_cmd_fix_miss_stats;',
'Just generate the command. Really run it needing
 `execute gen_cmd_fix_miss_stats; \gexec`',
'Optimizer');

prepare kill_session_by_user as
select pg_terminate_backend(pid) from pg_stat_activity
where usename = $1;

call polar_load_tool_comment('kill_session_by_user',
'execute kill_session_by_user(''xxx_user'');',
'Kill session by user.',
'Emergency');


prepare kill_session_by_client as
select pg_terminate_backend(pid) from pg_stat_activity
where client_addr = $1;

call polar_load_tool_comment('kill_session_by_client',
'execute kill_session_by_client(''192.168.0.3'');',
'Kill session by client addrs',
'Emergency');

prepare kill_session_by_long_query as
select pg_terminate_backend(pid) from pg_stat_activity
where clock_timestamp()-query_start > $1
and backend_type='client backend';

call polar_load_tool_comment('kill_session_by_long_query',
'execute kill_session_by_long_query(''10 min'');',
'Terminated the sessions which has run a single query exceeds n minutes',
'Emergency');


prepare kill_session_by_long_txn as
select pg_terminate_backend(pid) from pg_stat_activity
where clock_timestamp()- xact_start >  $1
and backend_type='client backend';
;

call polar_load_tool_comment(
'kill_session_by_long_txn',
'execute kill_session_by_long_txn(''10 min'');',
'Kill the session whose transatioin has run for more than xxx minutes',
'Emergency');


prepare gen_cmd_limit_conns_per_user as
select 'alter user ' || $1 || ' connection limit ' || $2 || ';';

call polar_load_tool_comment('gen_cmd_limit_conns_per_user',
'execute gen_cmd_limit_conns_per_user(''user_name'', 10);',
'Just generate the query to limit the connection
run `execute gen_cmd_limit_conns_per_user(''xxx'', 10); \gexec`
to really execute it.',
'Emergency');


prepare current_temp_table as
select nspname, relname
from pg_namespace ns, pg_class c
where ns.nspname like 'pg_temp_%'
and c.relnamespace = ns.oid
;

call polar_load_tool_comment('current_temp_table',
'execute current_temp_table;',
'Show current existing temp table.',
'Admin');
