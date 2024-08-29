--
-- ADVISORY LOCKS
--

BEGIN;

SELECT
	pg_advisory_xact_lock(1), pg_advisory_xact_lock_shared(2),
	pg_advisory_xact_lock(1, 1), pg_advisory_xact_lock_shared(2, 2);

SELECT locktype, classid, objid, objsubid, mode, granted
    FROM pg_locks pl JOIN pg_stat_activity pa USING (pid)
    WHERE
        locktype = 'advisory'
        AND pa.datname = current_database()
        AND pa.application_name = 'pg_regress/advisory_lock'
    ORDER BY 1, 2, 3, 4;

-- pg_advisory_unlock_all() shouldn't release xact locks
SELECT pg_advisory_unlock_all();

SELECT count(*) FROM pg_locks pl JOIN pg_stat_activity pa USING (pid)
	WHERE locktype = 'advisory'
	AND pa.datname = current_database()
    AND pa.application_name = 'pg_regress/advisory_lock';


-- can't unlock xact locks
SELECT
	pg_advisory_unlock(1), pg_advisory_unlock_shared(2),
	pg_advisory_unlock(1, 1), pg_advisory_unlock_shared(2, 2);


-- automatically release xact locks at commit
COMMIT;

SELECT count(*) FROM pg_locks pl JOIN pg_stat_activity pa USING (pid)
	WHERE locktype = 'advisory'
	AND pa.datname = current_database()
    AND pa.application_name = 'pg_regress/advisory_lock';


BEGIN;

-- holding both session and xact locks on the same objects, xact first
SELECT
	pg_advisory_xact_lock(1), pg_advisory_xact_lock_shared(2),
	pg_advisory_xact_lock(1, 1), pg_advisory_xact_lock_shared(2, 2);

SELECT locktype, classid, objid, objsubid, mode, granted
    FROM pg_locks pl JOIN pg_stat_activity pa USING (pid)
    WHERE
        locktype = 'advisory'
        AND pa.datname = current_database()
        AND pa.application_name = 'pg_regress/advisory_lock'
    ORDER BY 1, 2, 3, 4;

SELECT
	pg_advisory_lock(1), pg_advisory_lock_shared(2),
	pg_advisory_lock(1, 1), pg_advisory_lock_shared(2, 2);

ROLLBACK;

SELECT locktype, classid, objid, objsubid, mode, granted
    FROM pg_locks pl JOIN pg_stat_activity pa USING (pid)
    WHERE
        locktype = 'advisory'
        AND pa.datname = current_database()
        AND pa.application_name = 'pg_regress/advisory_lock'
    ORDER BY 1, 2, 3, 4;


-- unlocking session locks
SELECT
	pg_advisory_unlock(1), pg_advisory_unlock(1),
	pg_advisory_unlock_shared(2), pg_advisory_unlock_shared(2),
	pg_advisory_unlock(1, 1), pg_advisory_unlock(1, 1),
	pg_advisory_unlock_shared(2, 2), pg_advisory_unlock_shared(2, 2);

SELECT count(*) FROM pg_locks pl JOIN pg_stat_activity pa USING (pid)
	WHERE locktype = 'advisory'
	AND pa.datname = current_database()
    AND pa.application_name = 'pg_regress/advisory_lock';


BEGIN;

-- holding both session and xact locks on the same objects, session first
SELECT
	pg_advisory_lock(1), pg_advisory_lock_shared(2),
	pg_advisory_lock(1, 1), pg_advisory_lock_shared(2, 2);

SELECT locktype, classid, objid, objsubid, mode, granted
    FROM pg_locks pl JOIN pg_stat_activity pa USING (pid)
    WHERE
        locktype = 'advisory'
        AND pa.datname = current_database()
        AND pa.application_name = 'pg_regress/advisory_lock'
    ORDER BY 1, 2, 3, 4;

SELECT
	pg_advisory_xact_lock(1), pg_advisory_xact_lock_shared(2),
	pg_advisory_xact_lock(1, 1), pg_advisory_xact_lock_shared(2, 2);

ROLLBACK;

SELECT locktype, classid, objid, objsubid, mode, granted
    FROM pg_locks pl JOIN pg_stat_activity pa USING (pid)
    WHERE
        locktype = 'advisory'
        AND pa.datname = current_database()
        AND pa.application_name = 'pg_regress/advisory_lock'
    ORDER BY 1, 2, 3, 4;


-- releasing all session locks
SELECT pg_advisory_unlock_all();

SELECT count(*) FROM pg_locks pl JOIN pg_stat_activity pa USING (pid)
	WHERE locktype = 'advisory'
	AND pa.datname = current_database()
    AND pa.application_name = 'pg_regress/advisory_lock';


BEGIN;

-- grabbing txn locks multiple times

SELECT
	pg_advisory_xact_lock(1), pg_advisory_xact_lock(1),
	pg_advisory_xact_lock_shared(2), pg_advisory_xact_lock_shared(2),
	pg_advisory_xact_lock(1, 1), pg_advisory_xact_lock(1, 1),
	pg_advisory_xact_lock_shared(2, 2), pg_advisory_xact_lock_shared(2, 2);

SELECT locktype, classid, objid, objsubid, mode, granted
    FROM pg_locks pl JOIN pg_stat_activity pa USING (pid)
    WHERE
        locktype = 'advisory'
        AND pa.datname = current_database()
        AND pa.application_name = 'pg_regress/advisory_lock'
    ORDER BY 1, 2, 3, 4;

COMMIT;

SELECT count(*) FROM pg_locks pl JOIN pg_stat_activity pa USING (pid)
	WHERE locktype = 'advisory'
	AND pa.datname = current_database()
    AND pa.application_name = 'pg_regress/advisory_lock';

-- grabbing session locks multiple times

SELECT
	pg_advisory_lock(1), pg_advisory_lock(1),
	pg_advisory_lock_shared(2), pg_advisory_lock_shared(2),
	pg_advisory_lock(1, 1), pg_advisory_lock(1, 1),
	pg_advisory_lock_shared(2, 2), pg_advisory_lock_shared(2, 2);

SELECT locktype, classid, objid, objsubid, mode, granted
    FROM pg_locks pl JOIN pg_stat_activity pa USING (pid)
    WHERE
        locktype = 'advisory'
        AND pa.datname = current_database()
        AND pa.application_name = 'pg_regress/advisory_lock'
    ORDER BY 1, 2, 3, 4;

SELECT
	pg_advisory_unlock(1), pg_advisory_unlock(1),
	pg_advisory_unlock_shared(2), pg_advisory_unlock_shared(2),
	pg_advisory_unlock(1, 1), pg_advisory_unlock(1, 1),
	pg_advisory_unlock_shared(2, 2), pg_advisory_unlock_shared(2, 2);

SELECT count(*) FROM pg_locks pl JOIN pg_stat_activity pa USING (pid)
	WHERE locktype = 'advisory'
	AND pa.datname = current_database()
    AND pa.application_name = 'pg_regress/advisory_lock';

-- .. and releasing them all at once

SELECT
	pg_advisory_lock(1), pg_advisory_lock(1),
	pg_advisory_lock_shared(2), pg_advisory_lock_shared(2),
	pg_advisory_lock(1, 1), pg_advisory_lock(1, 1),
	pg_advisory_lock_shared(2, 2), pg_advisory_lock_shared(2, 2);

SELECT locktype, classid, objid, objsubid, mode, granted
    FROM pg_locks pl JOIN pg_stat_activity pa USING (pid)
    WHERE
        locktype = 'advisory'
        AND pa.datname = current_database()
        AND pa.application_name = 'pg_regress/advisory_lock'
    ORDER BY 1, 2, 3, 4;

SELECT pg_advisory_unlock_all();

SELECT count(*) FROM pg_locks pl JOIN pg_stat_activity pa USING (pid)
	WHERE locktype = 'advisory'
	AND pa.datname = current_database()
    AND pa.application_name = 'pg_regress/advisory_lock';
