-- Test parallel background writer.
--
-- alter parallel bgwriter workers to a big value.
alter system set polar_parallel_flush_workers = 20;

alter system set polar_parallel_flush_workers = 8;
select pg_reload_conf();
select pg_sleep(0.1);

-- alter parallel bgwriter workers to a small values.
alter system set polar_parallel_flush_workers = 0;
select pg_reload_conf();
select pg_sleep(0.1);

alter system set polar_parallel_flush_workers = 3;
select pg_reload_conf();
select pg_sleep(0.1);