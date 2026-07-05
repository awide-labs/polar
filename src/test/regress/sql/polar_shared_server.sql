create extension polar_monitor;
create extension polar_monitor_preload;

select polar_dispatcher_pid() - polar_dispatcher_pid(), polar_session_backend_pid() - polar_session_backend_pid(), pg_backend_pid() - pg_backend_pid();
select polar_is_dedicated_backend();

-- test guc cache
\c -
set enable_indexscan=0;
set max_parallel_maintenance_workers=99;
set jit_above_cost=123.456;
set application_name='test1';
set synchronous_commit='off';
select polar_dispatcher_pid() - polar_dispatcher_pid(), polar_session_backend_pid() - polar_session_backend_pid(), pg_backend_pid() - pg_backend_pid();
select polar_is_dedicated_backend();

show enable_indexscan;
show max_parallel_maintenance_workers;
show jit_above_cost;
show application_name;
show synchronous_commit;

create table t1_ss(c1 int);
begin;
insert into t1_ss values (1);
commit;

show enable_indexscan;
show max_parallel_maintenance_workers;
show jit_above_cost;
show application_name;
show synchronous_commit;
select polar_is_dedicated_backend();

drop table t1_ss;

-- test custom guc
\c -
select polar_is_dedicated_backend();
set test.ab=1;
select polar_is_dedicated_backend();
show test.ab;
select polar_is_dedicated_backend();

-- test polar_ss_dedicated_guc_names
\c -
alter system set polar_ss_dedicated_guc_names ='enable_indexscan,max_parallel_maintenance_workers,application_name';
select pg_reload_conf();
\c -
set enable_indexscan=0;
select polar_is_dedicated_backend();
show enable_indexscan;

\c -
set max_parallel_maintenance_workers=100;
select polar_is_dedicated_backend();
show max_parallel_maintenance_workers;

\c -
set application_name='test1';
select polar_is_dedicated_backend();
show application_name;

alter system set polar_ss_dedicated_guc_names ='';
select pg_reload_conf();

-- test polar_ss_dedicated_extension_names
drop extension polar_monitor;
drop extension polar_monitor_preload;

\c -
alter system set polar_ss_dedicated_extension_names ='polar_monitor_preload';
select pg_reload_conf();

\c -
create extension polar_monitor;
select polar_session_backend_pid() - polar_session_backend_pid();

\c - 
select polar_is_dedicated_backend();

\c -
create extension polar_monitor_preload;

\c - 
select polar_session_backend_pid() - polar_session_backend_pid();
select polar_is_dedicated_backend();

\c -
alter system set polar_ss_dedicated_extension_names ='';
select pg_reload_conf();


-- test polar_ss_dedicated_dbuser_names
\c regression postgres
DROP DATABASE IF EXISTS d1;
CREATE DATABASE d1;
DROP ROLE IF EXISTS u1;
CREATE ROLE u1 LOGIN;

\c d1
create extension polar_monitor;

\c d1 u1
select polar_is_dedicated_backend();

\c regression postgres
alter system set polar_ss_dedicated_dbuser_names ='d1/*';
select pg_reload_conf();

\c d1 postgres
select polar_is_dedicated_backend();

\c regression postgres
select polar_is_dedicated_backend();


\c regression postgres
alter system set polar_ss_dedicated_dbuser_names ='*/u1';
select pg_reload_conf();

\c regression u1
select polar_is_dedicated_backend();

\c regression postgres
select polar_is_dedicated_backend();


\c regression postgres
alter system set polar_ss_dedicated_dbuser_names ='d1/u1';
select pg_reload_conf();

\c d1 u1
select polar_is_dedicated_backend();

\c d1 postgres
select polar_is_dedicated_backend();

\c regression u1
select polar_is_dedicated_backend();

\c regression postgres
alter system set polar_ss_dedicated_dbuser_names ='';
select pg_reload_conf();

DROP DATABASE IF EXISTS d1;
DROP ROLE IF EXISTS u1;
-- end

\c regression postgres
drop extension polar_monitor;
drop extension polar_monitor_preload;
