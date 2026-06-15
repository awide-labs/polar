# pg_visible_in_snapshot isolation test
#
# Tests all visibility outcomes for pg_visible_in_snapshot(xid, snapshot)

setup
{
	CREATE TABLE xid_table (descr text, id xid8); -- stores different xids
	CREATE TABLE snap_table (snap pg_snapshot); -- stores taken snapshot
	CREATE TABLE dummy (id xid8); -- for silent xid allocations

	-- This strange function is required to proper test csn=on and csn=off configurations
	CREATE OR REPLACE FUNCTION test_csn_quirk(pattern text, vanilla_res boolean)
	RETURNS boolean
	LANGUAGE plpgsql
	AS $$
	DECLARE
		csn_enabled boolean;
		result      boolean;
	BEGIN
		csn_enabled := current_setting('polar_csn_enable', true)::boolean;

		IF csn_enabled THEN
			BEGIN
				PERFORM pg_visible_in_snapshot(
					(SELECT id FROM xid_table WHERE descr LIKE pattern),
					(SELECT snap FROM snap_table)
				);
				-- Expected error
				RETURN false;
			EXCEPTION
				WHEN OTHERS THEN
					IF SQLERRM ILIKE '%snapshot too old for CSN visibility check%' THEN
						RETURN true;
					ELSE
						RAISE;
					END IF;
			END;
		ELSE
			SELECT pg_visible_in_snapshot(
				(SELECT id FROM xid_table WHERE descr LIKE pattern),
				(SELECT snap FROM snap_table)
			) INTO result;
			RETURN result = vanilla_res;
		END IF;
	END;
	$$;
}

teardown
{
	DROP TABLE IF EXISTS xid_table;
	DROP TABLE IF EXISTS snap_table;
	DROP TABLE IF EXISTS dummy;
	DROP FUNCTION IF EXISTS test_csn_quirk;
}

# open transaction that defines snapshot xmin
session s_xmin
step xmin_begin { BEGIN; INSERT INTO dummy VALUES (pg_current_xact_id()); }
step xmin_end   { COMMIT; }

# Witness: records xids of in-progress transactions via counter trick.
session s_witness
step w0 { INSERT INTO xid_table VALUES ('0 lt_xmin_aborted', (pg_current_xact_id()::text::integer - 1)::text::xid8); }
step w4 { INSERT INTO xid_table VALUES ('4 range_committed_after_snap', (pg_current_xact_id()::text::integer - 1)::text::xid8); }
step w5 { INSERT INTO xid_table VALUES ('5 range_aborted', (pg_current_xact_id()::text::integer - 1)::text::xid8); }
step w6 { INSERT INTO xid_table VALUES ('6 range_in_progress', (pg_current_xact_id()::text::integer - 1)::text::xid8); }

# xid < xmin, committed
session s_before
step t1 { INSERT INTO xid_table VALUES ('1 lt_xmin', pg_current_xact_id()); }
step t3 { INSERT INTO xid_table VALUES ('3 range_committed_before_snap', pg_current_xact_id()); }

# xid in [xmin,xmax), commits after snapshot
session s_after_committed
step t4_begin  { BEGIN; INSERT INTO dummy VALUES (pg_current_xact_id()); }
step t4_commit { COMMIT; }

# xid in [xmin,xmax), aborts after snapshot
session s_aborted
step t5_begin  { BEGIN; INSERT INTO dummy VALUES (pg_current_xact_id()); }
step t5_abort  { ROLLBACK; }

# xid in [xmin,xmax), stays in-progress
session s_in_progress
step t6_begin  { BEGIN; INSERT INTO dummy VALUES (pg_current_xact_id()); }
step t6_abort  { ROLLBACK; }

# xid < xmin, aborted
session s_aborted_before
step t0_begin  { BEGIN; INSERT INTO dummy VALUES (pg_current_xact_id()); }
step t0_abort  { ROLLBACK; }

# Observer
session s_obs
step snap        { INSERT INTO snap_table SELECT pg_current_snapshot(); }
step t2          { INSERT INTO xid_table VALUES ('2 ge_xmax', pg_current_xact_id()); }
step show_xids   { SELECT descr FROM xid_table ORDER BY id; }
step check_0     { SELECT pg_visible_in_snapshot((SELECT id FROM xid_table WHERE descr LIKE '0 %'), (SELECT snap FROM snap_table)); }
step check_1     { SELECT pg_visible_in_snapshot((SELECT id FROM xid_table WHERE descr LIKE '1 %'), (SELECT snap FROM snap_table)); }
step check_2     { SELECT pg_visible_in_snapshot((SELECT id FROM xid_table WHERE descr LIKE '2 %'), (SELECT snap FROM snap_table)); }
step check_3     { SELECT pg_visible_in_snapshot((SELECT id FROM xid_table WHERE descr LIKE '3 %'), (SELECT snap FROM snap_table)); }
step check_4     { SELECT pg_visible_in_snapshot((SELECT id FROM xid_table WHERE descr LIKE '4 %'), (SELECT snap FROM snap_table)); }
step check_5     { SELECT pg_visible_in_snapshot((SELECT id FROM xid_table WHERE descr LIKE '5 %'), (SELECT snap FROM snap_table)); }
step check_6     { SELECT pg_visible_in_snapshot((SELECT id FROM xid_table WHERE descr LIKE '6 %'), (SELECT snap FROM snap_table)); }

step check_3_quirk     { SELECT test_csn_quirk('3 %', true); }
step check_4_quirk     { SELECT test_csn_quirk('4 %', false); }
step check_5_quirk     { SELECT test_csn_quirk('5 %', false); }

# Easy case: all xids are always >= XMin
permutation
	t0_begin
	w0
	t0_abort
	t1
	xmin_begin
	t3
	t4_begin
	w4
	t5_begin
	w5
	t6_begin
	w6
	snap
	t4_commit
	t5_abort
	t2

	show_xids
	check_0
	check_1
	check_2
	check_3
	check_4
	check_5
	check_6

	t6_abort
	xmin_end

# CSN erroneous case: rotted snapshot
permutation
	t0_begin
	w0
	t0_abort
	t1
	xmin_begin
	t3
	t4_begin
	w4
	t5_begin
	w5
	t6_begin
	w6
	snap
	t4_commit
	t5_abort
	t2
	xmin_end

	show_xids
	check_0
	check_1
	check_2
	check_3_quirk
	check_4_quirk
	check_5_quirk
	check_6

	t6_abort
