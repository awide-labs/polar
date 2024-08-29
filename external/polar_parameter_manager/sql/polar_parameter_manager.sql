SET client_min_messages TO 'error';
DROP EXTENSION IF EXISTS polar_parameter_manager;
CREATE SCHEMA test_parameter_manger;
SET SEARCH_PATH to test_parameter_manger;
DROP TABLE IF EXISTS t1;
RESET client_min_messages;
CREATE EXTENSION polar_parameter_manager;

SELECT name, default_value, is_dynamic, is_visible, is_user_changable, optional, unit, divide_base, comment
FROM polar_parameter.parameter_infos where name = 'DateStyle';

SELECT name, default_value, setting_value FROM polar_parameter.parameter_infos where name = 'wal_receiver_create_temp_slot';
ALTER SYSTEM set wal_receiver_create_temp_slot to on;
SELECT pg_reload_conf();
SELECT pg_sleep(1);

SELECT name, default_value, setting_value FROM polar_parameter.parameter_infos where name = 'wal_receiver_create_temp_slot';
ALTER SYSTEM reset wal_receiver_create_temp_slot;
SELECT pg_reload_conf();
SELECT pg_sleep(1);

-- case sensitive in polar_parameter.polar_parameter_manager
SELECT count(*) FROM polar_parameter.parameter_infos where name = 'Wal_receiver_create_temp_slot';

-- need restart
CREATE TABLE t1 (cur_val varchar);
INSERT INTO t1 SELECT setting_value FROM polar_parameter.parameter_infos where name = 'unix_socket_directories';

ALTER SYSTEM set unix_socket_directories to '.';
SELECT pg_reload_conf();
SELECT pg_sleep(1);
SELECT setting_value = cur_val FROM polar_parameter.parameter_infos, t1 where name = 'unix_socket_directories';

ALTER SYSTEM set unix_socket_directories to '/tmp';
SELECT pg_reload_conf();
SELECT pg_sleep(1);
SELECT setting_value = cur_val FROM polar_parameter.parameter_infos, t1 where name = 'unix_socket_directories';

ALTER SYSTEM reset unix_socket_directories;
DROP TABLE t1;
--------------
-- check force
-- check no repeated guc
SELECT name,count(*) FROM polar_parameter.parameter_infos group by name having count(name) > 1;
-- checkforce is ok
select name,is_visible from polar_parameter.parameter_infos where name = 'debug_print_parse';
-- force change is_visible
update polar_parameter.parameter_infos_force set is_visible = 1 where name = 'debug_print_parse';
select name,is_visible from polar_parameter.parameter_infos where name = 'debug_print_parse';
-- force change is_visible with null
update polar_parameter.parameter_infos_force set is_visible = null where name = 'debug_print_parse';
select name,is_visible,is_user_changable,optional from polar_parameter.parameter_infos where name = 'debug_print_parse';
update polar_parameter.parameter_infos_force set is_visible = 1 where name = 'debug_print_parse';
select name,is_visible,is_user_changable,optional from polar_parameter.parameter_infos where name = 'debug_print_parse';
-- check parameter_infos,parameter_infos_force and parameter_infos_memory has same column
select * from polar_parameter.parameter_infos
UNION ALL
select * from polar_parameter.parameter_infos_force
UNION ALL
select * from polar_parameter.parameter_infos_memory
limit 0;
-- check is_list attribute for v1.1
select name, is_list from polar_parameter.parameter_infos where name in ('shared_preload_libraries', 'work_mem') order by 1;
