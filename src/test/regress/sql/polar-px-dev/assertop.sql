-- For target queries optionally specify EXPLAIN (COSTS OFF, VERBOSE) option from outside
\getenv explain_only EXPLAIN_ONLY

\set opt_explain ''
\if :explain_only
    \set opt_explain 'EXPLAIN (COSTS OFF, VERBOSE)'
\endif

set polar_enable_px = 1;

-- simple SQL with assert
-- expect assertion failure because more than 1 row returned
:opt_explain select (select 1 union select 2);
-- expect assertion failure because more than 1 row returned
:opt_explain select (select generate_series(1,5));
-- expect assertion pass
:opt_explain select (select generate_series(1,1)) as series;


-- PX query over tables with assert
drop table if exists assert_test1;
create table assert_test1(id int);
insert into assert_test1(id) select generate_series(1, 1000) as id order by id desc;
analyze assert_test1;
-- expect assertion failure because more than 1 row returned
:opt_explain select * from assert_test1 where id = (select id from assert_test1);
-- expect assertion pass because only 1 row returned
:opt_explain select * from assert_test1 where id = (select id from assert_test1 where id = 1);

drop table if exists assert_test2;
create table assert_test2(id1 int, id2 int, id3 int, id4 int);
insert into assert_test2(id1, id2, id3, id4) select
    generate_series(1, 1000),
    generate_series(1, 1000),
    generate_series(1, 1000),
    generate_series(1, 1000);
analyze assert_test2;
-- expect assertion failure because more than 1 row returned
:opt_explain select * from assert_test2 where id2 = (select id3 from assert_test2);
-- expect assertion pass because only 1 row returned
:opt_explain select * from assert_test2 where id2 = (select id3 from assert_test2 where id4 = 1);

-- More DML with assert to be test seperately...
reset all;
drop table assert_test1;
drop table assert_test2;
