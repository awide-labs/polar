-- set system-wide parameters for testing PX engine
alter system set polar_px_enable_replay_wait to on;
select pg_reload_conf();

-- check the number of available segments for query running
select count(1)-1 as segments_count from polar_cluster_info;
show polar_px_nodes;

create extension polar_px;
