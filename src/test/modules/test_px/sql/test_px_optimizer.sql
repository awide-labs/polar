set client_min_messages = warning;
create extension if not exists test_px;
drop table if exists t;
create table t  (id int, age int, name text);
insert into t values (0),(1);

-- px_optimizer_util
select test_px_gpopt_wrapper();
select test_px_gpopt_sql('select * from t');
-- prevent matching of dynamically generated OID's
select regexp_replace(
    test_px_gpopt_sql_to_dxl('select * from t'),
    'TableDescriptor Mdid="\d+\.\d+\.\d+\.\d+"', 'TableDescriptor Mdid="OID"'
) test_px_gpopt_sql_to_dxl;
select test_px_gpopt_config_param('select * from t');

-- default is false
set polar_px_info_debug = true;
select test_px_gpopt_sql('select * from t');
set polar_px_info_debug = false;

-- default is 1.0, cost module * sort_factor
set polar_px_optimizer_sort_factor = 1.1;
    select test_px_gpopt_cost_module('select * from t');
set polar_px_optimizer_sort_factor = 1.0;
    select test_px_gpopt_cost_module('select * from t');

-- updaet polar_px_optimizer_mdcache_size
set polar_px_optimizer_mdcache_size = 20000;
select test_px_gpopt_sql('select * from t');
set polar_px_optimizer_mdcache_size = 16384;

-- update polar_px_optimizer_metadata_caching
set polar_px_optimizer_metadata_caching = false;
select test_px_gpopt_sql('select * from t');
set polar_px_optimizer_metadata_caching = true;

-- update polar_px_optimizer_use_px_allocators=on, restart pg, and re-run
