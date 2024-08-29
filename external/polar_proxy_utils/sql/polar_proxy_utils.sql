-- test for polar_proxy_utils extension

create extension polar_proxy_utils;

select name, type, rw_mode from polar_list_proxy_routing_strategy();
-- corner case
select polar_add_proxy_routing_strategy('', 't', 'r');
-- invalid type
select polar_add_proxy_routing_strategy('test', 'x', 'r');
-- invalid rw mode
select polar_add_proxy_routing_strategy('test', 'f', 'x');
-- add ok
select polar_add_proxy_routing_strategy('test', 'f', 'r');
select polar_add_proxy_routing_strategy('test1', 't', 'r');
select polar_add_proxy_routing_strategy('test2', 'f', 'w');

select name, type, rw_mode from polar_list_proxy_routing_strategy();
select name, type, rw_mode from polar_proxy_all_routing_strategy;
-- duplicated name, diff type
select polar_add_proxy_routing_strategy('test', 't', 'r');
-- duplicated name, error
select polar_add_proxy_routing_strategy('test', 'f', 'r');
-- too long name
select polar_add_proxy_routing_strategy('12345678901234567890123456789012345678901234567890123456789012345', 't', 'r');

select name, type, rw_mode from polar_list_proxy_routing_strategy();
-- no exist
select polar_delete_proxy_routing_strategy('xxx', 'f');
-- delete ok
select polar_delete_proxy_routing_strategy('test', 'f');

select name, type, rw_mode from polar_list_proxy_routing_strategy();
-- truncate ok
select polar_truncate_proxy_routing_strategy();
-- empty table
select name, type, rw_mode from polar_list_proxy_routing_strategy();
-- insert 1w entry
insert into polar_proxy_routing_strategy(name, type, rw_mode) (select id as name, 0 as type, 0 as rw_mode from generate_series(1,10000) as id);
-- overflow
select polar_add_proxy_routing_strategy('test', 'f', 'r');

drop extension polar_proxy_utils;
