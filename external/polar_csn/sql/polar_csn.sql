--
-- Test polar csn function
--

create extension polar_csn;

create table test(a int);

begin;
insert into test values(1);
select txid_csn(txid_current());
commit;

select txid_snapshot_csn(txid_current_snapshot()) > 0;

drop table test;
drop extension polar_csn;