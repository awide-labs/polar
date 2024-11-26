--
-- ADVISORY LOCKS
--

SELECT oid AS datoid FROM pg_database WHERE datname = current_database() \gset

BEGIN;

SELECT
	pg_advisory_xact_lock(1), pg_advisory_xact_lock_shared(2),
	pg_advisory_xact_lock(1, 1), pg_advisory_xact_lock_shared(2, 2);

SELECT locktype, classid, objid, objsubid, mode, granted
<<<<<<< HEAD
    FROM pg_locks pl JOIN pg_stat_activity pa USING (pid)
    WHERE
        locktype = 'advisory'
        AND pa.datname = current_database()
        AND pa.application_name = 'pg_regress/advisory_lock'
    ORDER BY 1, 2, 3, 4;
=======
	FROM pg_locks WHERE locktype = 'advisory' AND database = :datoid
	ORDER BY classid, objid, objsubid;

>>>>>>> REL_15_10

-- pg_advisory_unlock_all() shouldn't release xact locks
SELECT pg_advisory_unlock_all();

<<<<<<< HEAD
SELECT count(*) FROM pg_locks pl JOIN pg_stat_activity pa USING (pid)
	WHERE locktype = 'advisory'
	AND pa.datname = current_database()
    AND pa.application_name = 'pg_regress/advisory_lock';
=======
SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND database = :datoid;
>>>>>>> REL_15_10


-- can't unlock xact locks
SELECT
	pg_advisory_unlock(1), pg_advisory_unlock_shared(2),
	pg_advisory_unlock(1, 1), pg_advisory_unlock_shared(2, 2);


-- automatically release xact locks at commit
COMMIT;

<<<<<<< HEAD
SELECT count(*) FROM pg_locks pl JOIN pg_stat_activity pa USING (pid)
	WHERE locktype = 'advisory'
	AND pa.datname = current_database()
    AND pa.application_name = 'pg_regress/advisory_lock';
=======
SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND database = :datoid;
>>>>>>> REL_15_10


BEGIN;

-- holding both session and xact locks on the same objects, xact first
SELECT
	pg_advisory_xact_lock(1), pg_advisory_xact_lock_shared(2),
	pg_advisory_xact_lock(1, 1), pg_advisory_xact_lock_shared(2, 2);

SELECT locktype, classid, objid, objsubid, mode, granted
<<<<<<< HEAD
    FROM pg_locks pl JOIN pg_stat_activity pa USING (pid)
    WHERE
        locktype = 'advisory'
        AND pa.datname = current_database()
        AND pa.application_name = 'pg_regress/advisory_lock'
    ORDER BY 1, 2, 3, 4;
=======
	FROM pg_locks WHERE locktype = 'advisory' AND database = :datoid
	ORDER BY classid, objid, objsubid;
>>>>>>> REL_15_10

SELECT
	pg_advisory_lock(1), pg_advisory_lock_shared(2),
	pg_advisory_lock(1, 1), pg_advisory_lock_shared(2, 2);

ROLLBACK;

SELECT locktype, classid, objid, objsubid, mode, granted
<<<<<<< HEAD
    FROM pg_locks pl JOIN pg_stat_activity pa USING (pid)
    WHERE
        locktype = 'advisory'
        AND pa.datname = current_database()
        AND pa.application_name = 'pg_regress/advisory_lock'
    ORDER BY 1, 2, 3, 4;
=======
	FROM pg_locks WHERE locktype = 'advisory' AND database = :datoid
	ORDER BY classid, objid, objsubid;
>>>>>>> REL_15_10


-- unlocking session locks
SELECT
	pg_advisory_unlock(1), pg_advisory_unlock(1),
	pg_advisory_unlock_shared(2), pg_advisory_unlock_shared(2),
	pg_advisory_unlock(1, 1), pg_advisory_unlock(1, 1),
	pg_advisory_unlock_shared(2, 2), pg_advisory_unlock_shared(2, 2);

<<<<<<< HEAD
SELECT count(*) FROM pg_locks pl JOIN pg_stat_activity pa USING (pid)
	WHERE locktype = 'advisory'
	AND pa.datname = current_database()
    AND pa.application_name = 'pg_regress/advisory_lock';
=======
SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND database = :datoid;
>>>>>>> REL_15_10


BEGIN;

-- holding both session and xact locks on the same objects, session first
SELECT
	pg_advisory_lock(1), pg_advisory_lock_shared(2),
	pg_advisory_lock(1, 1), pg_advisory_lock_shared(2, 2);

SELECT locktype, classid, objid, objsubid, mode, granted
<<<<<<< HEAD
    FROM pg_locks pl JOIN pg_stat_activity pa USING (pid)
    WHERE
        locktype = 'advisory'
        AND pa.datname = current_database()
        AND pa.application_name = 'pg_regress/advisory_lock'
    ORDER BY 1, 2, 3, 4;
=======
	FROM pg_locks WHERE locktype = 'advisory' AND database = :datoid
	ORDER BY classid, objid, objsubid;
>>>>>>> REL_15_10

SELECT
	pg_advisory_xact_lock(1), pg_advisory_xact_lock_shared(2),
	pg_advisory_xact_lock(1, 1), pg_advisory_xact_lock_shared(2, 2);

ROLLBACK;

SELECT locktype, classid, objid, objsubid, mode, granted
<<<<<<< HEAD
    FROM pg_locks pl JOIN pg_stat_activity pa USING (pid)
    WHERE
        locktype = 'advisory'
        AND pa.datname = current_database()
        AND pa.application_name = 'pg_regress/advisory_lock'
    ORDER BY 1, 2, 3, 4;
=======
	FROM pg_locks WHERE locktype = 'advisory' AND database = :datoid
	ORDER BY classid, objid, objsubid;
>>>>>>> REL_15_10


-- releasing all session locks
SELECT pg_advisory_unlock_all();

<<<<<<< HEAD
SELECT count(*) FROM pg_locks pl JOIN pg_stat_activity pa USING (pid)
	WHERE locktype = 'advisory'
	AND pa.datname = current_database()
    AND pa.application_name = 'pg_regress/advisory_lock';
=======
SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND database = :datoid;
>>>>>>> REL_15_10


BEGIN;

-- grabbing txn locks multiple times

SELECT
	pg_advisory_xact_lock(1), pg_advisory_xact_lock(1),
	pg_advisory_xact_lock_shared(2), pg_advisory_xact_lock_shared(2),
	pg_advisory_xact_lock(1, 1), pg_advisory_xact_lock(1, 1),
	pg_advisory_xact_lock_shared(2, 2), pg_advisory_xact_lock_shared(2, 2);

SELECT locktype, classid, objid, objsubid, mode, granted
<<<<<<< HEAD
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
=======
	FROM pg_locks WHERE locktype = 'advisory' AND database = :datoid
	ORDER BY classid, objid, objsubid;

COMMIT;

SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND database = :datoid;
>>>>>>> REL_15_10

-- grabbing session locks multiple times

SELECT
	pg_advisory_lock(1), pg_advisory_lock(1),
	pg_advisory_lock_shared(2), pg_advisory_lock_shared(2),
	pg_advisory_lock(1, 1), pg_advisory_lock(1, 1),
	pg_advisory_lock_shared(2, 2), pg_advisory_lock_shared(2, 2);

SELECT locktype, classid, objid, objsubid, mode, granted
<<<<<<< HEAD
    FROM pg_locks pl JOIN pg_stat_activity pa USING (pid)
    WHERE
        locktype = 'advisory'
        AND pa.datname = current_database()
        AND pa.application_name = 'pg_regress/advisory_lock'
    ORDER BY 1, 2, 3, 4;
=======
	FROM pg_locks WHERE locktype = 'advisory' AND database = :datoid
	ORDER BY classid, objid, objsubid;
>>>>>>> REL_15_10

SELECT
	pg_advisory_unlock(1), pg_advisory_unlock(1),
	pg_advisory_unlock_shared(2), pg_advisory_unlock_shared(2),
	pg_advisory_unlock(1, 1), pg_advisory_unlock(1, 1),
	pg_advisory_unlock_shared(2, 2), pg_advisory_unlock_shared(2, 2);

<<<<<<< HEAD
SELECT count(*) FROM pg_locks pl JOIN pg_stat_activity pa USING (pid)
	WHERE locktype = 'advisory'
	AND pa.datname = current_database()
    AND pa.application_name = 'pg_regress/advisory_lock';
=======
SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND database = :datoid;
>>>>>>> REL_15_10

-- .. and releasing them all at once

SELECT
	pg_advisory_lock(1), pg_advisory_lock(1),
	pg_advisory_lock_shared(2), pg_advisory_lock_shared(2),
	pg_advisory_lock(1, 1), pg_advisory_lock(1, 1),
	pg_advisory_lock_shared(2, 2), pg_advisory_lock_shared(2, 2);

SELECT locktype, classid, objid, objsubid, mode, granted
<<<<<<< HEAD
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
=======
	FROM pg_locks WHERE locktype = 'advisory' AND database = :datoid
	ORDER BY classid, objid, objsubid;

SELECT pg_advisory_unlock_all();

SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND database = :datoid;
>>>>>>> REL_15_10
