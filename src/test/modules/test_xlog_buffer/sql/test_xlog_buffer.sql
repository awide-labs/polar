CREATE EXTENSION IF NOT EXISTS test_xlog_buffer;
SET client_min_messages TO 'error';

CHECKPOINT;
-- See README for explanation of arguments:
BEGIN;
SET TRANSACTION ISOLATION LEVEL SERIALIZABLE, READ ONLY, DEFERRABLE;
SELECT test_xlog_buffer();
ABORT;

RESET client_min_messages;
