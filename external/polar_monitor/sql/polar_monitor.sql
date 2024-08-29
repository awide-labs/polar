-- Test access privileges
--
--setup
create extension polar_monitor;

-- buffer monitor
select COUNT(*) >= 0 AS result from polar_normal_buffercache;
select COUNT(*) >= 0 AS result from polar_copy_buffercache;
select COUNT(polar_flushlist()) >= 0 As result;
select COUNT(polar_cbuf()) >= 0 As result;
select COUNT(polar_backend_flush()) >= 0 As result;
select COUNT(polar_lru_flush_info()) >= 0 As result;

-- polar_stat_activity
select a = b is_equal from (select
(select count(*) from polar_stat_activity) a,
(select count(*) from pg_stat_activity) b
) c;
-- polar_stat_io_info
select count(*)>6 from polar_stat_io_info;
-- polar_stat_io_latency
select count(*) from polar_stat_io_latency;

-- check polar_stat_io_info and polar_stat_activity are vaild
DO $$
<<test_polar_io_stat_block>>
DECLARE
    delta_a_sum_iops integer = 0;
    delta_a_sum_iothroughput bigint = 0;
    delta_a_sum_iolatency_us decimal = 0;
    delta_b_sum_iops integer = 0;
    delta_b_sum_iothroughput bigint = 0;
    delta_b_sum_iolatency_us decimal = 0;

    first_a_sum_iops integer = 0;
    first_a_sum_iothroughput bigint = 0;
    first_a_sum_iolatency_us decimal = 0;
    first_b_sum_iops integer = 0;
    first_b_sum_iothroughput bigint = 0;
    first_b_sum_iolatency_us decimal = 0;

    second_a_sum_iops integer = 0;
    second_a_sum_iothroughput bigint = 0;
    second_a_sum_iolatency_us decimal = 0;
    second_b_sum_iops integer = 0;
    second_b_sum_iothroughput bigint = 0;
    second_b_sum_iolatency_us decimal = 0;
BEGIN
    select sum(shared_read_ps+local_read_ps) ,sum(shared_read_throughput+local_read_throughput), sum(shared_read_latency_ms+local_read_latency_ms) into first_a_sum_iops,first_a_sum_iothroughput,first_a_sum_iolatency_us from polar_stat_activity ;
    select sum(read_count) + sum(write_count) , sum(read_throughput) + sum(write_throughput), sum(read_latency_us) +  sum(write_latency_us) into first_b_sum_iops,first_b_sum_iothroughput,first_b_sum_iolatency_us from polar_stat_io_info;
    create table polar_io_test(id int);
    insert into polar_io_test select generate_series(1,100);
    delete from polar_io_test;
    drop table polar_io_test;
    select sum(read_count) + sum(write_count) , sum(read_throughput) + sum(write_throughput), sum(read_latency_us) +  sum(write_latency_us)into second_b_sum_iops,second_b_sum_iothroughput,second_b_sum_iolatency_us from polar_stat_io_info;
    select sum(shared_read_ps+local_read_ps) ,sum(shared_read_throughput+local_read_throughput), sum(shared_read_latency_ms+local_read_latency_ms) into second_a_sum_iops,second_a_sum_iothroughput,second_a_sum_iolatency_us from polar_stat_activity;

    delta_a_sum_iops = second_a_sum_iops - first_a_sum_iops;
    delta_a_sum_iothroughput =  second_a_sum_iothroughput - first_a_sum_iothroughput;
    delta_a_sum_iolatency_us = (second_a_sum_iolatency_us - first_a_sum_iolatency_us)*1000;
    delta_b_sum_iops = second_b_sum_iops - first_b_sum_iops;
    delta_b_sum_iolatency_us = second_b_sum_iolatency_us - first_b_sum_iolatency_us;
    delta_b_sum_iothroughput = second_b_sum_iothroughput - first_b_sum_iothroughput;

    if (delta_b_sum_iops - delta_a_sum_iops> 10000) then
        RAISE NOTICE 'The delta_sum_iops is more than %', delta_b_sum_iops - delta_a_sum_iops;
    END if;
    if (delta_b_sum_iothroughput-delta_a_sum_iothroughput > 100000000) then
        RAISE NOTICE 'The delta_sum_iothroughput is more than %', delta_b_sum_iothroughput-delta_a_sum_iothroughput;
    END if;
    if (delta_b_sum_iolatency_us-delta_a_sum_iolatency_us > 1000000) then
        RAISE NOTICE 'The delta_sum_iolatency_us is more than %', delta_b_sum_iolatency_us-delta_a_sum_iolatency_us;
    END if;
END test_polar_io_stat_block $$;

-- check polar dynamic bgworker
SELECT COUNT(*) = 1 FROM polar_stat_activity WHERE backend_type='checkpointer';

-- check polar stat activity_rt
select COUNT(*) >= 4 AS backends from polar_stat_activity_rt;

-- check polar_set_available and polar_is_available
select polar_is_available();
select polar_set_available(false);
select polar_is_available();
select polar_set_available(true);
select polar_is_available();

-- pfsadm du for unexists path
select * from pfs_du_with_depth(1, 'mock/du_path');

-- pfsadm info for unexists pdbName
select * from pfs_info();

--cleanup
drop extension polar_monitor;
