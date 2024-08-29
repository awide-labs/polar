create extension polar_monitor_preload;

select name from polar_get_local_mcxt() where name = 'TopMemoryContext';

select * from polar_get_mcxt(pg_backend_pid());

SELECT COUNT(*) >= 1 AS result FROM polar_backends_mcxt WHERE name = 'CacheMemoryContext';

/** polar_stat_lock && polar_stat_lwlock **/

CREATE OR REPLACE FUNCTION test_lock_count() RETURNS bool AS
$x$
DECLARE
	ret bool;
	before int;
	after int;
BEGIN
	SELECT exclusivelock INTO STRICT before from polar_stat_lock where lock_type='virtualxid';

	PERFORM 'SELECT COUNT(*) from pg_locks';
	PERFORM 'SELECT COUNT(*) from pg_locks';
	PERFORM 'SELECT COUNT(*) from polar_stat_lock';
	PERFORM 'SELECT COUNT(*) from pg_class';

	SELECT exclusivelock INTO STRICT after from polar_stat_lock where lock_type='virtualxid';

	IF before = after THEN
	    ret = true;
	ELSE
            ret = false;
	END IF;

	return ret;
END
$x$
LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION test_lwlock_count() RETURNS bool AS
$x$
DECLARE
	ret bool;
	before int;
	after int;
BEGIN
	SELECT sh_acquire_count INTO STRICT before FROM polar_stat_lwlock WHERE name='ProcArray';

	PERFORM 'SELECT accesssharelock from polar_stat_lock';
	PERFORM 'SELECT accesssharelock from polar_stat_lock';
	PERFORM 'SELECT accesssharelock from polar_stat_lock';
	PERFORM 'SELECT accesssharelock from polar_stat_lock';

	SELECT sh_acquire_count INTO STRICT after FROM polar_stat_lwlock WHERE name='ProcArray';

	IF before = after THEN
	    ret = true;
	ELSE
            ret = false;
	END IF;

	return ret;
END
$x$
LANGUAGE plpgsql;

/** polar_proc_stat_lock && polar_proc_stat_lwlock **/
CREATE OR REPLACE FUNCTION test_proc_stat_lock() RETURNS bool AS
$x$
DECLARE
	ret bool;
	before int;
	after int;
BEGIN
	SELECT COUNT(*) INTO STRICT before FROM (SELECT DISTINCT pid FROM polar_proc_stat_lock()) a JOIN (SELECT pid FROM pg_stat_activity) b ON a.pid = b.pid;

	SELECT COUNT(DISTINCT pid) INTO STRICT after FROM polar_proc_stat_lock();

	IF before = after THEN
	    ret = true;
	ELSE
            ret = false;
	END IF;

	return ret;
END
$x$
LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION test_proc_stat_lwlock() RETURNS bool AS
$x$
DECLARE
	ret bool;
	before int;
	after int;
BEGIN
	SELECT COUNT(*) INTO STRICT before FROM (SELECT DISTINCT pid FROM polar_proc_stat_lwlock()) a JOIN (SELECT pid FROM pg_stat_activity) b ON a.pid = b.pid;

	SELECT COUNT(DISTINCT pid) INTO STRICT after FROM polar_proc_stat_lwlock();

	IF before = after THEN
	    ret = true;
	ELSE
            ret = false;
	END IF;

	return ret;
END
$x$
LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION test_network_stat() RETURNS bool AS
$x$
DECLARE
	ret bool;
	before bigint;
	after bigint;
BEGIN
	SELECT send_bytes INTO STRICT before from polar_stat_network;

	PERFORM 'SELECT COUNT(*) from pg_locks';
	PERFORM 'SELECT COUNT(*) from pg_locks';
	PERFORM 'SELECT COUNT(*) from polar_stat_lock';
	PERFORM 'SELECT COUNT(*) from pg_class';

	SELECT send_bytes INTO STRICT after from polar_stat_network;

	IF before = after THEN
	    ret = true;
	ELSE
            ret = false;
	END IF;

	return ret;
END
$x$
LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION test_proc_network_stat() RETURNS bool AS
$x$
DECLARE
	ret bool;
	before int;
	after int;
BEGIN
	SELECT COUNT(*) INTO STRICT before FROM (SELECT DISTINCT pid FROM polar_proc_stat_network()) a JOIN (SELECT pid FROM pg_stat_activity) b ON a.pid = b.pid;

	SELECT COUNT(DISTINCT pid) INTO STRICT after FROM polar_proc_stat_network();

	IF before = after THEN
	    ret = true;
	ELSE
            ret = false;
	END IF;

	return ret;
END
$x$
LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION empty_loop() RETURNS void AS
$x$
BEGIN
	PERFORM (extract(epoch from now()))::int8;
	PERFORM tcpinfo_update_time FROM polar_proc_stat_network() WHERE pid = (SELECT pg_backend_pid());
	PERFORM PG_SLEEP(1);
	PERFORM (extract(epoch from now()))::int8;
	PERFORM tcpinfo_update_time FROM polar_proc_stat_network() WHERE pid = (SELECT pg_backend_pid());
	PERFORM PG_SLEEP(1);
	PERFORM (extract(epoch from now()))::int8;
	PERFORM tcpinfo_update_time FROM polar_proc_stat_network() WHERE pid = (SELECT pg_backend_pid());
	PERFORM PG_SLEEP(1);
	PERFORM (extract(epoch from now()))::int8;
	PERFORM tcpinfo_update_time FROM polar_proc_stat_network() WHERE pid = (SELECT pg_backend_pid());
	PERFORM PG_SLEEP(1);
	PERFORM (extract(epoch from now()))::int8;
	PERFORM tcpinfo_update_time FROM polar_proc_stat_network() WHERE pid = (SELECT pg_backend_pid());
	PERFORM PG_SLEEP(1);
END
$x$
LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION test_network_tcpinfo_and_sendrecvq_collect() RETURNS bool AS
$x$
DECLARE
	ret bool;
	begin_time int;
BEGIN
	SELECT (extract(epoch from now()))::int8 INTO STRICT begin_time;
	PERFORM * FROM empty_loop();
	SELECT tcpinfo_update_time > begin_time INTO STRICT ret FROM polar_proc_stat_network() WHERE pid = (SELECT pg_backend_pid());
	return ret;
END
$x$
LANGUAGE plpgsql;

SELECT * FROM test_lock_count();
SELECT * FROM test_lwlock_count();
SELECT * FROM test_proc_stat_lock();
SELECT * FROM test_proc_stat_lwlock();
SELECT * FROM test_proc_network_stat();

-- lwlock waiters
SELECT COUNT(*)>48 res FROM polar_stat_lwlock;
SELECT COUNT(*)>48 res FROM polar_lwlock_stat_waiters;

SELECT COUNT(*)>10 res FROM polar_lwlock_stat_waiters WHERE max_lock_waiters > 0;

-- network tcpinfo and sendq/recvq
ALTER SYSTEM SET polar_enable_track_network_stat=off;
SELECT PG_RELOAD_CONF();
SELECT * FROM empty_loop();
ALTER SYSTEM SET polar_enable_track_network_stat=on;
SELECT PG_RELOAD_CONF();
SELECT * FROM empty_loop();
SELECT * FROM test_network_tcpinfo_and_sendrecvq_collect();
