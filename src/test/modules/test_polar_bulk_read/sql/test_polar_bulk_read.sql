SET client_min_messages = 'warning';
CREATE EXTENSION IF NOT EXISTS test_polar_bulk_read;
CREATE EXTENSION IF NOT EXISTS polar_monitor;

create table bulk_read_tbl(id int8, value int8);
select from pg_stat_reset();
select relname, heap_bulk_read_calls, heap_bulk_read_calls_io,heap_bulk_read_blks_io from polar_pg_statio_user_tables where relname = 'bulk_read_tbl';

--- 12801 pages with content, other pages is empty which is extend by bulk extend. 104MB
INSERT INTO bulk_read_tbl select generate_series,generate_series from generate_series(0*185, 12800*185 + 184);

select count(*) from bulk_read_tbl;
--- local test is too fast because of page cache, pg_sleep() let statcollector have enough time to deal with stats.
select pg_sleep(2);
select relname,
	   heap_bulk_read_calls > 0,
	   heap_bulk_read_calls_io >= 0,
	   heap_bulk_read_blks_io >= 0
from polar_pg_statio_user_tables where relname = 'bulk_read_tbl';

--- flush buffers of bulk_read_test.
--- only master exec checkpoint
do language plpgsql $$
    begin
	    if (select not pg_is_in_recovery()) then
	 	    checkpoint;
  	    end if;
    end
$$;

---------------------  test bulk read in select ---------------------------
--- drop buffers of bulk_read_test. 
SELECT polar_drop_relation_buffers('bulk_read_tbl', 'main', 0);
select from pg_stat_reset();

select count(*) from bulk_read_tbl;
--- local test is too fast because of page cache, pg_sleep() let statcollector have enough time to deal with stats.
select pg_sleep(2);
select relname,
	   heap_bulk_read_calls > 0,
	   heap_bulk_read_calls_io > 0,
	   heap_bulk_read_blks_io > 0,
	   heap_bulk_read_blks_io >= 2 * heap_bulk_read_calls_io
from polar_pg_statio_user_tables where relname = 'bulk_read_tbl';

--- drop buffers of bulk_read_test. 
SELECT polar_drop_relation_buffers('bulk_read_tbl', 'main', 0);
select from pg_stat_reset();

select * from bulk_read_tbl where id > 100000 and id < 100005 order by id;
--- local test is too fast because of page cache, pg_sleep() let statcollector have enough time to deal with stats.
select pg_sleep(2);
select relname,
	   heap_bulk_read_calls > 0,
	   heap_bulk_read_calls_io > 0,
	   heap_bulk_read_blks_io > 0,
	   heap_bulk_read_blks_io >= 2 * heap_bulk_read_calls_io
from polar_pg_statio_user_tables where relname = 'bulk_read_tbl';

--- drop buffers of bulk_read_test. 
SELECT polar_drop_relation_buffers('bulk_read_tbl', 'main', 0);
select from pg_stat_reset();

--- (2368184 + 2367785) * (2368184 - 2367785 + 1) / 2 = 947193800
select sum(value) from bulk_read_tbl where id >= 2367785;
--- local test is too fast because of page cache, pg_sleep() let statcollector have enough time to deal with stats.
select pg_sleep(2);
select relname,
	   heap_bulk_read_calls > 0,
	   heap_bulk_read_calls_io > 0,
	   heap_bulk_read_blks_io > 0,
	   heap_bulk_read_blks_io >= 2 * heap_bulk_read_calls_io
from polar_pg_statio_user_tables where relname = 'bulk_read_tbl';

---------------------  test bulk read in update ---------------------------
--- drop buffers of bulk_read_test. 
SELECT polar_drop_relation_buffers('bulk_read_tbl', 'main', 0);
select from pg_stat_reset();

update bulk_read_tbl set value = -1 where id % 2 = 0;
--- local test is too fast because of page cache, pg_sleep() let statcollector have enough time to deal with stats.
select pg_sleep(2);
select relname,
	   heap_bulk_read_calls > 0,
	   heap_bulk_read_calls_io > 0,
	   heap_bulk_read_blks_io > 0,
	   heap_bulk_read_blks_io >= 2 * heap_bulk_read_calls_io
from polar_pg_statio_user_tables where relname = 'bulk_read_tbl';

--- flush buffers of bulk_read_test.
--- only master exec checkpoint
do language plpgsql $$
    begin
	    if (select not pg_is_in_recovery()) then
	 	    checkpoint;
  	    end if;
    end
$$;

---------------------  test bulk read in vaccum ---------------------------
--- drop buffers of bulk_read_test. 
SELECT polar_drop_relation_buffers('bulk_read_tbl', 'main', 0);
select from pg_stat_reset();

vacuum (FREEZE) bulk_read_tbl;
--- local test is too fast because of page cache, pg_sleep() let statcollector have enough time to deal with stats.
select pg_sleep(2);
select relname,
	   heap_bulk_read_calls > 0,
	   heap_bulk_read_calls_io > 0,
	   heap_bulk_read_blks_io > 0,
	   heap_bulk_read_blks_io >= 2 * heap_bulk_read_calls_io
from polar_pg_statio_user_tables where relname = 'bulk_read_tbl';

SELECT polar_drop_relation_buffers('bulk_read_tbl', 'main', 0);
select count(*) from bulk_read_tbl where id % 2 = 0;

---------------------- test bulk read in create index ---------------------------
--- drop buffers of bulk_read_test. 
SELECT polar_drop_relation_buffers('bulk_read_tbl', 'main', 0);
select from pg_stat_reset();

CREATE INDEX bulk_read_tbl_id_index ON  bulk_read_tbl(id);
--- local test is too fast because of page cache, pg_sleep() let statcollector have enough time to deal with stats.
select pg_sleep(2);
select relname,
	   heap_bulk_read_calls > 0,
	   heap_bulk_read_calls_io > 0,
	   heap_bulk_read_blks_io > 0,
	   heap_bulk_read_blks_io >= 2 * heap_bulk_read_calls_io
from polar_pg_statio_user_tables where relname = 'bulk_read_tbl';

DROP TABLE bulk_read_tbl;
