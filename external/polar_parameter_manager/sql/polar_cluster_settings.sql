--begin test
create schema test_cluster_settings;
set search_path to test_cluster_settings;
create view desensitized_file_settings as 
select case when sourcefile like '%polar_settings.conf%' then 'polar_settings.conf' 
            when sourcefile like '%postgresql.auto.conf%' then 'postgresql.auto.conf'
            when sourcefile like '%postgresql.conf%' then 'postgresql.conf' end as sourcefile,
    name,setting,applied from pg_file_settings 
    where sourcefile like '%polar_settings.conf%' or 
          sourcefile like '%postgresql.auto.conf%' or
          sourcefile like '%postgresql.conf%';
-- ############### test SQL ##############
ALTER SYSTEM FOR CLUSTER reset debug_print_rewritten;
alter system reset debug_print_rewritten;
select * from desensitized_file_settings where name = 'debug_print_rewritten';
show debug_print_rewritten;

ALTER SYSTEM FOR CLUSTER set debug_print_rewritten to on;
select pg_reload_conf();
select pg_sleep(1);
select * from desensitized_file_settings where name = 'debug_print_rewritten';
show debug_print_rewritten;

ALTER SYSTEM FOR CLUSTER set debug_print_rewritten to default;
select pg_reload_conf();
select pg_sleep(1);
select * from desensitized_file_settings where name = 'debug_print_rewritten';
show debug_print_rewritten;

ALTER SYSTEM FOR CLUSTER set debug_print_rewritten to on;
select pg_reload_conf();
select pg_sleep(1);
select * from desensitized_file_settings where name = 'debug_print_rewritten';
show debug_print_rewritten;

ALTER SYSTEM FOR CLUSTER reset debug_print_rewritten;
select pg_reload_conf();
select pg_sleep(1);
select * from desensitized_file_settings where name = 'debug_print_rewritten';
show debug_print_rewritten;
-- ############### test priority ##############
ALTER SYSTEM FOR CLUSTER set debug_print_rewritten to on;--polar_settings set on
alter system set debug_print_rewritten to off;--autoconf set off
select pg_reload_conf();
select pg_sleep(1);
select * from desensitized_file_settings where name = 'debug_print_rewritten';
--should be off
show debug_print_rewritten;

ALTER SYSTEM FOR CLUSTER reset debug_print_rewritten;
alter system reset debug_print_rewritten;

-- make postgresql.conf debug_print_rewritten = off
copy
(select 'echo ''debug_print_rewritten = off'' >> ' || current_setting('data_directory') || '/postgresql.conf')
to '/tmp/set_conf';
\! sh /tmp/set_conf
\! rm /tmp/set_conf
-- set polar_settings
ALTER SYSTEM FOR CLUSTER set debug_print_rewritten to on;--polar_settings set on
select pg_reload_conf();
select pg_sleep(1);
select * from desensitized_file_settings where name = 'debug_print_rewritten';
--should be off
show debug_print_rewritten;
ALTER SYSTEM FOR CLUSTER reset debug_print_rewritten;
copy (select 'sed -i ''$d'' ' || current_setting('data_directory') || '/postgresql.conf') to '/tmp/reset_conf';
\! sh /tmp/reset_conf
\! rm /tmp/reset_conf

select pg_reload_conf();
select pg_sleep(1);
-- ############### end ##############
drop schema test_cluster_settings cascade;