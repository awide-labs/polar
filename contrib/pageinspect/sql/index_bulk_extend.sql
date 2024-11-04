create schema test_index_bulk_extend;
set search_path to test_index_bulk_extend;
set client_min_messages to error; 

create extension if not exists pageinspect;

show polar_index_bulk_extend_size;

drop table if exists p;
create table p(a int, b varchar, c numeric,d int8);


-- INSERT DATA
insert into p select
i,
md5(i::text),
i / 982.0,
i * -1
from
generate_series(0,100000 - 1)i;
-- INIT INDEX
create index p_c_d_idx on p(c,d);

-- DML
insert into p select
i,
i::text || 'sdha&$#*&',
i / 160.0,
i / 32
from
generate_series(0,100000 - 1)i;

-- For index bulk extend expansion
select pg_relation_size('p_c_d_idx') < 3 * pg_relation_size('p');
-- For index bulk extend core
select count(*) from generate_series(1, pg_relation_size('p_c_d_idx') / current_setting('block_size')::bigint - 1) AS blkno,bt_page_items('p_c_d_idx', blkno);

drop schema test_index_bulk_extend cascade;