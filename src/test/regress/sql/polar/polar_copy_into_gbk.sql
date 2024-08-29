-- directory paths are passed to us in environment variables
\getenv abs_srcdir PG_ABS_SRCDIR

-- get filename 
\set filename :abs_srcdir '/data/polar_gbk_tbl.csv'

create table polar_tbl
("serialno" numeric,
"appntname" varchar,
"appntbirthday" date,
"appntsex" int,
"appntidtype" int,
"appntidno" varchar,
"resulttype" varchar,
"reultstatus" varchar,
"remark" varchar,
"makedate" date,
"maketime" time,
"modifydate" date,
"modifytime" time,
"operator" varchar);
\COPY polar_tbl ("serialno","appntname","appntbirthday","appntsex","appntidtype","appntidno","resulttype","reultstatus","remark","makedate","maketime","modifydate","modifytime","operator") FROM :'filename'  DELIMITER '|' ESCAPE '\' CSV QUOTE '"';
COPY polar_tbl ("serialno","appntname","appntbirthday","appntsex","appntidtype","appntidno","resulttype","reultstatus","remark","makedate","maketime","modifydate","modifytime","operator") FROM :'filename'  DELIMITER '|' ESCAPE '\' CSV QUOTE '"';
SELECT COUNT(*) FROM polar_tbl;
drop table polar_tbl;
