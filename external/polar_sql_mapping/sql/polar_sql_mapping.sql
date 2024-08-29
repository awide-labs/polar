--=========================================================
--================= SQL MAPPING ===========================
--=========================================================
-- setup
SET CLIENT_MIN_MESSAGES = ERROR;
DROP EXTENSION IF EXISTS polar_sql_mapping;
CREATE EXTENSION IF NOT EXISTS polar_sql_mapping;
CREATE SCHEMA IF NOT EXISTS polar_sql_mapping_test;
-- Check all sql_mapping object collation is C
-- Because the ScanKeyInit we use when mapping uses the C collation by default
select attrelid::regclass::text as relname,attname
from pg_catalog.pg_attribute where
attrelid in (select oid from pg_catalog.pg_class where relnamespace = 'polar_sql_mapping'::regnamespace::oid)
and attcollation != 0 and attcollation != (select oid from pg_catalog.pg_collation  where collname = 'C');
-- start up
DROP TABLE IF EXISTS polar_sql_mapping_test.t;
RESET CLIENT_MIN_MESSAGES;
CREATE TABLE polar_sql_mapping_test.t (a int);
INSERT INTO polar_sql_mapping_test.t (SELECT a FROM generate_series(10, 15) a);

-- test sql mapping insertion
SELECT * FROM polar_sql_mapping.polar_sql_mapping_table;

SELECT polar_sql_mapping.insert_mapping('a','b');
SELECT polar_sql_mapping.insert_mapping('c','b');
SELECT polar_sql_mapping.insert_mapping('b','a');

SELECT * FROM polar_sql_mapping.polar_sql_mapping_table;

-- test sql mapping duplicate insertion
SELECT polar_sql_mapping.insert_mapping('a','c');

SELECT * FROM polar_sql_mapping.polar_sql_mapping_table;

-- test sql mapping when GUC is off
SELECT polar_sql_mapping.insert_mapping('SELEC * FROM polar_sql_mapping_test.t;','SELECT * FROM polar_sql_mapping_test.t;');
SET polar_sql_mapping.use_sql_mapping TO OFF;
SHOW polar_sql_mapping.use_sql_mapping;
SELECT * FROM polar_sql_mapping.polar_sql_mapping_table;
SELEC * FROM polar_sql_mapping_test.t;

-- test sql mapping when GUC is on
SET polar_sql_mapping.use_sql_mapping TO ON;

SHOW polar_sql_mapping.use_sql_mapping;
SELECT * FROM polar_sql_mapping.polar_sql_mapping_table;
SELEC * FROM polar_sql_mapping_test.t;

-- test sql mapping after deleting
DELETE FROM polar_sql_mapping.polar_sql_mapping_table WHERE source_sql = 'SELEC * FROM polar_sql_mapping_test.t;';

SHOW polar_sql_mapping.use_sql_mapping;
SELECT * FROM polar_sql_mapping.polar_sql_mapping_table;
SELEC * FROM polar_sql_mapping_test.t;

-- test sql mapping after transaction failed
SELECT polar_sql_mapping.insert_mapping('SELEE * FROM polar_sql_mapping_test.t;','SELECT * FROM polar_sql_mapping_test.t;');

BEGIN TRANSACTION;
SELEC * FROM polar_sql_mapping_test.t;
SELEE * FROM polar_sql_mapping_test.t;
COMMIT;

BEGIN TRANSACTION;
SELEC * FROM polar_sql_mapping_test.t;
COMMIT;

SELEE * FROM polar_sql_mapping_test.t;

-- test turn on after dropping extension
DROP EXTENSION polar_sql_mapping;
SET polar_sql_mapping.use_sql_mapping TO ON;

SELECT * FROM polar_sql_mapping_test.t;
SELEE * FROM polar_sql_mapping_test.t;

DROP SCHEMA polar_sql_mapping_test CASCADE;

--=========================================================
--================= ERROR DETECTIVE =======================
--=========================================================
--  * error detective test *
--  * initial *
SET CLIENT_MIN_MESSAGES = ERROR;
DROP EXTENSION IF EXISTS polar_sql_mapping;
CREATE SCHEMA IF NOT EXISTS polar_error_detective_test;
RESET CLIENT_MIN_MESSAGES;

CREATE EXTENSION polar_sql_mapping ;
SET polar_sql_mapping.record_error_sql TO TRUE; 

SET search_path TO polar_error_detective_test;

--=========================================================
-- * crate tables *
SET CLIENT_MIN_MESSAGES = ERROR;
DROP TABLE IF EXISTS films;
DROP TABLE IF EXISTS t;
RESET CLIENT_MIN_MESSAGES;

CREATE TABLE polar_error_detective_test.films (
    code        char(5) CONSTRAINT firstkey PRIMARY KEY,
    title       varchar(40) NOT NULL,
    did         integer NOT NULL,
    date_prod   date,
    kind        varchar(10),
    len         interval hour to minute
);

CREATE TABLE polar_error_detective_test.t (
    a           integer PRIMARY KEY,
    b           integer NOT NULL
);


--=========================================================
-- * reset polar_sql_mapping.error_sql_info view *
select polar_sql_mapping.error_sql_info_clear();
select * from polar_sql_mapping.error_sql_info order by (id);

--=========================================================
-- * different error type *
set client_min_messages to 'error';
select * from t1;

select abc;

-- no error
prepare s as select * from t where now() > $1;
execute s(1);

select abcd(1);

select sum(distinct a) over (partition by b) from t;

--=========================================================
-- * show polar_sql_mapping.error_sql_info view *
select * from polar_sql_mapping.error_sql_info order by (id);

--=========================================================
-- * not record the error caused by "Class 23 — Integrity Constraint Violation" & 
-- * "Class 25 — Invalid Transaction State"
-- * more details refer to https://www.postgresql.org/docs/12/errcodes-appendix.html *
-- no error 
insert into films values ('abc', 'polar_sql_mapping', 1, '2021,8,18', 'art', '2:30');
-- error but not record
insert into films values ('abc', 'polar_sql_mapping', 1, '2021,8,18', 'art', '2:30');
-- error but not record
insert into films values ('abdc', NULL, 1, '2021,8,18', 'art', '2:30');

select * from polar_sql_mapping.error_sql_info order by (id);

--=========================================================
-- * repeat errors just increase their calls *
select * from t1;
select abc;
select abc;
select abc;

select * from polar_sql_mapping.error_sql_info order by (id);

--=========================================================
-- * number of records more than max number, not record anymore * 
-- max number of record
show polar_sql_mapping.max_num;

select dd from films;
select ff from films;
select gg from films;
select hh from films;
select abcd;
-- number of records equal to max number
select * from polar_sql_mapping.error_sql_info order by (id);

-- not record
select not_record;
select * from polar_sql_mapping.error_sql_info order by (id);

--=========================================================
-- * reset the record view *
select polar_sql_mapping.error_sql_info_clear();
select * from polar_sql_mapping.error_sql_info order by (id);

--=========================================================
-- * length of a error sql more than 1024 will not be record *
-- length is 1100 > 1024, not record

-- we expanded the max length of an error sql to 1024 * 1024
-- so this test case is ignored.

-- aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
-- aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
-- aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
-- aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
-- aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
-- aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
-- aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
-- aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
-- aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
-- aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
-- aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
-- aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
-- aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
-- aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
-- aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
-- aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
-- aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
-- aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
-- aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
-- aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
-- aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
-- aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
-- aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa;

select * from polar_sql_mapping.error_sql_info order by (id);

--=========================================================
-- * errors occuring in a transaction *
BEGIN TRANSACTION;
-- record
SELEC * FROM polar_sql_mapping_test.t;
-- not record
SELECT 2;
-- record
SELEE * FROM polar_sql_mapping_test.t;
COMMIT; 

select * from polar_sql_mapping.error_sql_info order by (id);

--=========================================================
-- * test polar_sql_mapping.insert_mapping_id(id, targetsql) *
select polar_sql_mapping.insert_mapping_id(15, 'select 2;');
select * from polar_sql_mapping.polar_sql_mapping_table where target_sql =  'select 2;';

select polar_sql_mapping.insert_mapping_id(11, 'select 2;');
select * from polar_sql_mapping.polar_sql_mapping_table where target_sql =  'select 2;';

-- * test work with sql mapping *
set polar_sql_mapping.use_sql_mapping to true;
-- well done!
SELEC * FROM polar_sql_mapping_test.t;
reset polar_sql_mapping.use_sql_mapping;
-- error again
SELEC * FROM polar_sql_mapping_test.t;

--=========================================================
-- * test configure of unexpected_error_catagory *
select polar_sql_mapping.error_sql_info_clear();
show polar_sql_mapping.unexpected_error_catagory;
-- not record  errcode = "23505"
insert into films values ('abc', 'polar_sql_mapping', 1, '2021,8,18', 'art', '2:30');
select * from polar_sql_mapping.error_sql_info order by (id);

set polar_sql_mapping.unexpected_error_catagory to '25';
-- record 
insert into films values ('abc', 'polar_sql_mapping', 1, '2021,8,18', 'art', '2:30');
select * from polar_sql_mapping.error_sql_info order by (id);

set polar_sql_mapping.unexpected_error_catagory to '23503,25';
-- record
insert into films values ('abc', 'polar_sql_mapping', 1, '2021,8,18', 'art', '2:30');
select * from polar_sql_mapping.error_sql_info order by (id);

set polar_sql_mapping.unexpected_error_catagory to '23505,25';
-- not record
insert into films values ('abc', 'polar_sql_mapping', 1, '2021,8,18', 'art', '2:30');
select * from polar_sql_mapping.error_sql_info order by (id);

-- illegal unexpected_error_catagory
set polar_sql_mapping.unexpected_error_catagory to '235057,25';
-- record
insert into films values ('abc', 'polar_sql_mapping', 1, '2021,8,18', 'art', '2:30');
select * from polar_sql_mapping.error_sql_info order by (id);
reset client_min_messages;

reset polar_sql_mapping.unexpected_error_catagory;
--=========================================================
-- * test error_pattern *
SELECT polar_sql_mapping.error_sql_info_clear();
reset polar_sql_mapping.error_pattern;
show polar_sql_mapping.error_pattern;
-- not record
select count(*) from films;
select query,emessage from polar_sql_mapping.error_sql_info order by (id);
--record
set polar_sql_mapping.error_pattern to 'select%count%';
select count(*) from films;
select query,emessage from polar_sql_mapping.error_sql_info order by (id);
RESET polar_sql_mapping.error_pattern;
--mapping to right
select polar_sql_mapping.insert_mapping(query,'select count(*)::numeric as "count(*)" from films;') from polar_sql_mapping.error_sql_info;
set polar_sql_mapping.use_sql_mapping to true;
select count(*) from films;
reset polar_sql_mapping.use_sql_mapping;
--=========================================================
-- * test done *
RESET polar_sql_mapping.use_sql_mapping;
RESET polar_sql_mapping.error_pattern;
RESET polar_sql_mapping.record_error_sql;
SELECT polar_sql_mapping.error_sql_info_clear();
DROP SCHEMA polar_error_detective_test CASCADE;
RESET search_path;
DROP extension polar_sql_mapping;
