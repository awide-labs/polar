create extension faultinjector;

set polar_enable_px to on;
set polar_px_enable_replay_wait to on;

create table fault_test(id int);
insert into fault_test values(generate_series(1,10000));
create index t1 on fault_test(id);

create table fault_test2(id int);
insert into fault_test2 values(generate_series(1,10000));
create index t2 on fault_test2(id);

-- px_disp_async.c
select inject_fault('all', 'reset');
select inject_fault('px_check_dispatch_no_result', 'enable', '', '', 1, 3, 0);
select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

-- TODO: pg15_px: enable it after fix of non-recoverable `numNonExtendedDispatcherState` counter
--  select inject_fault('all', 'reset');
--  select inject_fault('px_dispatch_command_error', 'enable', '', '', 1, 1, 0);
--  select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;
--  select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

--  select inject_fault('all', 'reset');
--  select inject_fault('px_process_results_input', 'enable', '', '', 1, 1, 0);
--  select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;
--  select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

--  select inject_fault('all', 'reset');
--  select inject_fault('px_process_bad_connection', 'enable', '', '', 1, 1, 0);
--  select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;
--  select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

-- px_conn.c
select inject_fault('all', 'reset');
select inject_fault('px_forward_notices_error', 'enable', '', '', 1, 1, 0);
select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

select inject_fault('all', 'reset');
select inject_fault('px_forward_notices_old_style', 'enable', '', '', 1, 1, 0);
select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

-- px_snapshot.c
select inject_fault('all', 'reset');
select inject_fault('pxsh_snapshot_equal', 'enable', '', '', 1, 3, 0);
select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

select inject_fault('all', 'reset');
select inject_fault('pxsh_snapshot1_xmin_smaller', 'enable', '', '', 1, 3, 0);
select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

select inject_fault('all', 'reset');
select inject_fault('pxsh_snapshot1_xmin_bigger', 'enable', '', '', 1, 3, 0);
select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

select inject_fault('all', 'reset');
select inject_fault('pxsh_snapshot1_xmax_smaller', 'enable', '', '', 1, 3, 0);
select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

select inject_fault('all', 'reset');
select inject_fault('pxsh_snapshot1_xmax_bigger', 'enable', '', '', 1, 3, 0);
select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

select inject_fault('all', 'reset');
select inject_fault('pxsh_snapshot1_subxcnt_more', 'enable', '', '', 1, 3, 0);
select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

select inject_fault('all', 'reset');
select inject_fault('pxsh_snapshot1_subxcnt_same', 'enable', '', '', 1, 3, 0);
select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

select inject_fault('all', 'reset');
select inject_fault('pxsh_snapshot1_subxcnt_less', 'enable', '', '', 1, 3, 0);
select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

-- px_disp_query.c
--  select inject_fault('all', 'reset');
--  select inject_fault('pxdisp_dispatch_slices', 'enable', '', '', 1, 1, 0);
--  select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

select inject_fault('all', 'reset');
select inject_fault('pxconn_disconnect', 'enable', '', '', 1, 1, 0);
select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

select inject_fault('all', 'reset');
select inject_fault('pxconn_entry_db', 'enable', '', '', 1, 1, 0);
select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

select inject_fault('all', 'reset');
select inject_fault('get_database_name', 'enable', '', '', 1, 1, 0);
select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

select inject_fault('all', 'reset');
select inject_fault('get_user_name', 'enable', '', '', 1, 1, 0);
select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

-- px_disp_async.c
select inject_fault('all', 'reset');
select inject_fault('pxdisp_flush_nonblock', 'enable', '', '', 1, 1, 0);
select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

select inject_fault('all', 'reset');
select inject_fault('pxdisp_flush_nonblock', 'enable', '', '', 1, 1, 0);
select inject_fault('pxdisp_nonblock_zero', 'enable', '', '', 1, 1, 0);
select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

select inject_fault('all', 'reset');
select inject_fault('pxdisp_flush_nonblock', 'enable', '', '', 1, 1, 0);
select inject_fault('pxdisp_nonblock_over_zero', 'enable', '', '', 1, 1, 0);
select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

-- TODO: pg15_px: enable it after fix of non-recoverable `numNonExtendedDispatcherState` counter
--  select inject_fault('all', 'reset');
--  select inject_fault('pxdisp_flush_nonblock', 'enable', '', '', 1, 1, 0);
--  select inject_fault('pxdisp_nonblock_less_zero', 'enable', '', '', 1, 1, 0);
--  select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

select inject_fault('all', 'reset');
select inject_fault('handle_poll_bad', 'enable', '', '', 1, 1, 0);
select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

select inject_fault('all', 'reset');
select inject_fault('nextval_inject', 'enable', '', '', 1, 1, 0);
select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

--  select inject_fault('all', 'reset');
--  select inject_fault('pgres_polling_failed', 'enable', '', '', 1, 1, 0);
--  select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

-- ic_udpifc.c
--  select inject_fault('all', 'reset');
--  select inject_fault('rx_thread_error', 'enable', '', '', 1, 3, 0);
--  select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

--  select inject_fault('all', 'reset');
--  select inject_fault('prunce_cursor_ic_entry', 'enable', '', '', 1, 1, 0);
--  select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

--  select inject_fault('all', 'reset');
--  select inject_fault('check_alive', 'enable', '', '', 1, 1, 0);
--  select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

--  select inject_fault('all', 'reset');
--  select inject_fault('active_count_fake', 'enable', '', '', 1, 1, 0);
--  select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

-- ic_tcp.c
--  select inject_fault('all', 'reset');
--  select inject_fault('px_flush_buffer', 'enable', '', '', 1, 1, 0);
--  select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

--  select inject_fault('all', 'reset');
--  select inject_fault('udp_listen_error', 'enable', '', '', 1, 1, 0);
--  select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

--  select inject_fault('all', 'reset');
--  select inject_fault('px_conn_not_null', 'enable', '', '', 1, 1, 0);
--  select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

--  select inject_fault('all', 'reset');
--  select inject_fault('px_handle_disorder_packet', 'enable', '', '', 1, 1, 0);
--  select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

--  select inject_fault('all', 'reset');
--  select inject_fault('send_status_query_message', 'enable', '', '', 1, 1, 0);
--  select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

--  select inject_fault('all', 'reset');
--  select inject_fault('set_socket_buffer_error', 'enable', '', '', 1, 1, 0);
--  select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

--  select inject_fault('all', 'reset');
--  select inject_fault('ic_buffer_list_null', 'enable', '', '', 1, 1, 0);
--  select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

--  select inject_fault('all', 'reset');
--  select inject_fault('ic_buffer_list_length_error', 'enable', '', '', 1, 1, 0);
--  select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

--  select inject_fault('all', 'reset');
--  select inject_fault('ic_buffer_list_length_error_2', 'enable', '', '', 1, 1, 0);
--  select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

--  select inject_fault('all', 'reset');
--  select inject_fault('ic_buffer_link_error', 'enable', '', '', 1, 1, 0);
--  select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

--  select inject_fault('all', 'reset');
--  select inject_fault('send_once_error', 'enable', '', '', 1, 1, 0);
--  select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

--  select inject_fault('all', 'reset');
--  select inject_fault('send_chunk_stop', 'enable', '', '', 1, 1, 0);
--  select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

--  select inject_fault('all', 'reset');
--  select inject_fault('set_udp_connect_error', 'enable', '', '', 1, 1, 0);
--  select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

--  select inject_fault('all', 'reset');
--  select inject_fault('miss_slice_table', 'enable', '', '', 1, 1, 0);
--  select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

--  select inject_fault('all', 'reset');
--  select inject_fault('miss_stats', 'enable', '', '', 1, 1, 0);
--  select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

--  select inject_fault('all', 'reset');
--  select inject_fault('pkt_duplicate', 'enable', '', '', 1, 1, 0);
--  select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

--  select inject_fault('all', 'reset');
--  select inject_fault('pkt_disorder', 'enable', '', '', 1, 1, 0);
--  select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

-- output is not always the same, ignore it.
-- px_conn.c
-- select inject_fault('all', 'reset');
-- select inject_fault('hostaddr_info', 'enable', '', '', 1, 1, 0);
-- select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

-- this will make database core, ignore it.
-- px_disp_async.c
-- select inject_fault('all', 'reset');
-- select inject_fault('pxdisp_poll_fd', 'enable', '', '', 1, 1, 0);
-- select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

-- this will make database core, ignore it.
-- px_dispatchresult.c
-- select inject_fault('all', 'reset');
-- select inject_fault('pxdisp_makeresult_error', 'enable', '', '', 1, 1, 0);
-- select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;
-- select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

-- this will make database core, ignore it.
-- px_gang.c
-- select inject_fault('all', 'reset');
-- select inject_fault('pxdisp_makeresult_error', 'enable', '', '', 1, 3, 0);
-- select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

-- select inject_fault('all', 'reset');
-- select inject_fault('px_build_gang_definition_error', 'enable', '', '', 1, 1, 0);
-- select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;

-- this will make database core, ignore it.
-- px_motion.c
-- select inject_fault('all', 'reset');
-- select inject_fault('px_send_stop_msg', 'enable', '', '', 1, 1, 0);
-- select count(*) from fault_test, fault_test2 where fault_test.id = fault_test2.id;
