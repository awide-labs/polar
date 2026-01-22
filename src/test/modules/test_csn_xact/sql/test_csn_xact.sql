CREATE EXTENSION test_csn_xact;

set polar_csn_xid_snapshot = true;
SELECT test_csn_xact_xmin(false);
SELECT test_csn_xact_xmin(true);
SELECT test_csn_xact_xmax(false);
SELECT test_csn_xact_xmax(true);
SELECT test_csn_xact_multixact(false);
SELECT test_csn_xact_multixact(true);
set polar_csn_xid_snapshot = false;
SELECT test_csn_xact_xmin(false);
SELECT test_csn_xact_xmin(true);
SELECT test_csn_xact_xmax(false);
SELECT test_csn_xact_xmax(true);
SELECT test_csn_xact_multixact(false);
SELECT test_csn_xact_multixact(true);
