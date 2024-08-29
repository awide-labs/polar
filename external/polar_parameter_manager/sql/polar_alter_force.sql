-- TEST ALTER FORCE
set client_min_messages to error; -- set client_min_messages to avoid other session reload_conf and cause unstable case
-- ## invalid ALTER FORCE ##
-- non-polar guc
alter system for cluster reload force set xxx to 'xxx';
alter system force set xxx to 'xxx';
alter system reload force set xxx to 'xxx';
alter system for cluster force set xxx to 'xxx';
-- value contain a newline
alter system for cluster force set polar_xxx to 'xxx
';
alter system for cluster reload force set xxx to '
xxx
';
alter system force set xxx to 'x
xx';
alter system reload force set xxx to '
xxx';
alter system for cluster force set xxx to 'x
xx';
-- ## valid ALTER FORCE ##
-- accept polar guc
alter system force set polar_xxx to 'xxx';
alter system force reset polar_xxx;
alter system for cluster force set polar_xxx to 'xxx';
alter system for cluster force reset polar_xxx;
-- accept extension guc
alter system force set xx1.xxx to 'xxx';
alter system force set polar_xx1.xxx to 'xxx';
alter system for cluster force set xx2.xxx to 'xxx';
alter system for cluster force set polar_xx2.xxx to 'xxx';
-- check force alter success
select count(distinct name) from pg_file_settings where
name in ('xx1.xxx','xx2.xxx','polar_xx1.xxx','polar_xx2.xxx');
-- reset extension guc at last.
alter system force reset xx1.xxx;
alter system force reset polar_xx1.xxx;
alter system for cluster force reset xx2.xxx;
alter system for cluster force reset polar_xx2.xxx;
-- check force alter reset success
select count(distinct name) from pg_file_settings where
name in ('xx1.xxx','xx2.xxx','polar_xx1.xxx','polar_xx2.xxx');
-- exists guc
alter system force set debug_print_parse to on;
alter system force reset debug_print_parse;
alter system for cluster force set debug_print_parse to on;
alter system for cluster force reset debug_print_parse;
-- exists guc sill check value
alter system force set debug_print_parse to invalid;
alter system for cluster force set debug_print_parse to invalid;
reset client_min_messages;
