CREATE EXTENSION faultinjector;

-- start with a clean slate
select inject_fault('all', 'reset');

-- inject fault of type skip
select inject_fault('checkpoint', 'skip', '', '', 1, 2, 0);

-- wait for fault triggered 0 times, should not block
select wait_until_triggered_fault('checkpoint', 0);

-- trigger a checkpoint which will trigger the fault
checkpoint;
select wait_until_triggered_fault('checkpoint', 1);

-- check status
select inject_fault('checkpoint', 'status');
select inject_fault('checkpoint', 'reset');

-- inject fault of type error, set it to trigger two times
select inject_fault('checkpoint', 'error', '', '', 1, 2, 0);

-- trigger once
checkpoint;
select inject_fault('checkpoint', 'status');

-- trigger twice
checkpoint;
select inject_fault('checkpoint', 'status');

-- no error the third time onwards
checkpoint;
select inject_fault('checkpoint', 'status');

-- reset the fault
select inject_fault('checkpoint', 'reset');
