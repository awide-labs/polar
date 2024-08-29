SET enable_seqscan = off;

CREATE TABLE t (val vector(3));
INSERT INTO t (val) VALUES ('[0,0,0]'), ('[1,2,3]'), ('[1,1,1]'), (NULL);
CREATE INDEX ON t (val);

SELECT * FROM t WHERE val = '[1,2,3]';
SELECT * FROM t ORDER BY val LIMIT 1;

DROP TABLE t;

-- uperuser
create user test superuser;
\c - test
create database test_db;
\c test_db
create extension vector;
CREATE TABLE t (val vector(3));
INSERT INTO t (val) VALUES ('[0,0,0]'), ('[1,2,3]'), ('[1,1,1]'), (NULL);
CREATE INDEX ON t (val);
SELECT * FROM t WHERE val = '[1,2,3]';
SELECT * FROM t ORDER BY val LIMIT 1;
DROP TABLE t;
drop extension vector;
\c - postgres
\c postgres
drop database test_db;
drop user test;
