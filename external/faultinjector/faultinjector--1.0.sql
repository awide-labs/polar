/* contrib/faultinjector/faultinjector--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION faultinjector" to load this file. \quit

--
-- Inject a fault that is predefined in backend code.  The fault is
-- associated with an action.  The specified action will be taken by a
-- backend process when the fault point is reached during execution.
--
--    faultname: name of the fault, this should match the definition
--    in backend code.
--
--    type: action to be taken when the fault is reached during
--    execution.  E.g. "error", "panic".  See below for explanation of
--    each fault type.
--
--    database (optional): the fault will be triggered only if current
--    database of a backend process name matches this one.
--
--    tablename (optional): the fault will be triggered only if
--    current table name matches this one.
--
--    start_occurrence (optional): the fault will start triggering
--    after it is reached as many times during in a backend process
--    during execution.
--
--    end_occurrence (optional): the fault will stop triggering after
--    it has been triggered as many times.
--
--    extra_arg (optional): used to specify the number of seconds to
--    sleep when injecting a "sleep" type of fault.
--
CREATE FUNCTION inject_fault(
  faultname text,
  type text,
  database text,
  tablename text,
  start_occurrence int4,
  end_occurrence int4,
  extra_arg int4)
RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C VOLATILE;

-- Simpler version to inject fault such that it is triggered only one
-- time, starting at the first occurrence.  Not tied to any database /
-- table.
CREATE FUNCTION inject_fault(
  faultname text,
  type text)
RETURNS text
AS $$ select inject_fault($1, $2, '', '', 1, 1, 0) $$
LANGUAGE SQL;

-- Simpler version, always trigger until fault it is reset.
CREATE FUNCTION inject_fault_infinite(
  faultname text,
  type text)
RETURNS text
AS $$ select inject_fault($1, $2, '', '', 1, -1, 0) $$
LANGUAGE SQL;

-- Wait until a fault is triggered desired number of times.
CREATE FUNCTION wait_until_triggered_fault(
  faultname text,
  numtimestriggered int4)
RETURNS text
AS $$ select inject_fault($1, 'wait_until_triggered', '', '', 1, 1, $2) $$
LANGUAGE SQL;

