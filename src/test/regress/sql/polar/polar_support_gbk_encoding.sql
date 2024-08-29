--
-- gbk
--
CREATE DATABASE db_gbk template template0 encoding = gbk LC_COLLATE = 'zh_CN.gbk' LC_CTYPE = 'zh_CN.gbk';
\c db_gbk;
SET client_encoding = 'utf-8';

CREATE TABLE polar_gbk_test_table(f1 varchar(3));
INSERT INTO polar_gbk_test_table (f1) VALUES ('綦容新');
INSERT INTO polar_gbk_test_table (f1) VALUES ('王公硕');
INSERT INTO polar_gbk_test_table (f1) VALUES ('佟逸良');
INSERT INTO polar_gbk_test_table (f1) VALUES ('赵一帆');
INSERT INTO polar_gbk_test_table (f1) VALUES ('刘彦坤');
INSERT INTO polar_gbk_test_table (f1) VALUES ('靳鑫');
-- error
INSERT INTO polar_gbk_test_table (f1) VALUES ('赵一帆2');
INSERT INTO polar_gb18030 (f1) VALUES ('𣘗𧄧');

-- order by
SELECT * FROM polar_gbk_test_table ORDER BY f1;

-- regular expression query
SELECT * FROM polar_gbk_test_table WHERE f1 ~ '^王' ORDER BY f1;

-- query encoding length
SELECT OCTET_LENGTH(f1) FROM polar_gbk_test_table ORDER BY f1;

-- clean 
DROP TABLE polar_gbk_test_table;

-- for coverage (polar_wchar2gbk_with_len) 
SELECT regexp_split_to_table('汔亓', 'T');

-- MATERIALIZED VIEW join
CREATE TABLE polar_view_test(i int, n varchar(32));
INSERT INTO polar_view_test VALUES (1, '阮雄风');
INSERT INTO polar_view_test VALUES (2, '张雷');
CREATE TABLE polar_view_test2(id int, name varchar(32));
INSERT INTO polar_view_test2 VALUES (1, '一片');
INSERT INTO polar_view_test2 VALUES (2, '海洋');
CREATE MATERIALIZED VIEW T_MATER AS SELECT * FROM polar_view_test  WITH NO DATA;
REFRESH MATERIALIZED VIEW T_MATER;
SELECT * FROM T_MATER p JOIN polar_view_test2 n on p.i = n.id order by i;
SELECT * FROM T_MATER p JOIN polar_view_test2 n on p.i = n.id order by name;
SELECT * FROM T_MATER p JOIN polar_view_test2 n on p.i = n.id order by n;
DROP MATERIALIZED VIEW T_MATER;
DROP TABLE polar_view_test;
DROP TABLE polar_view_test2;

--
-- gb18030
--
CREATE DATABASE db_gb18030 template template0 encoding = gb18030 LC_COLLATE = 'zh_CN.gb18030' LC_CTYPE = 'zh_CN.gb18030';
\c db_gb18030;
SET client_encoding = 'utf-8';

CREATE TABLE polar_gb18030(f1 varchar(3));
INSERT INTO polar_gb18030 (f1) VALUES ('綦容新');
INSERT INTO polar_gb18030 (f1) VALUES ('王公硕');
INSERT INTO polar_gb18030 (f1) VALUES ('佟逸良');
INSERT INTO polar_gb18030 (f1) VALUES ('赵一帆');
INSERT INTO polar_gb18030 (f1) VALUES ('刘彦坤');
INSERT INTO polar_gb18030 (f1) VALUES ('靳鑫');
INSERT INTO polar_gb18030 (f1) VALUES ('𣘗𧄧');
INSERT INTO polar_gb18030 (f1) VALUES ('赵一帆2');
INSERT INTO polar_gb18030 (f1) VALUES ('𣘗𧄧2');

-- clean 
DROP TABLE polar_gb18030;

-- for coverage (polar_gb18030_2_wchar_with_len) 
SELECT regexp_split_to_table('汔亓', 'T');
SELECT regexp_split_to_table('🉑️','x');

-- text
CREATE TABLE polar_text(i int, f1 text);
INSERT INTO polar_text (f1) VALUES ('綦容新');
INSERT INTO polar_text (f1) VALUES ('王公硕');
INSERT INTO polar_text (f1) VALUES ('佟逸良');
INSERT INTO polar_text (f1) VALUES ('赵一帆');
INSERT INTO polar_text (f1) VALUES ('刘彦坤');
INSERT INTO polar_text (f1) VALUES ('靳鑫');
INSERT INTO polar_text (f1) VALUES ('𣘗𧄧');
SELECT * FROM polar_text ORDER BY f1;

-- clean 
DROP TABLE polar_text;

-- nvarchar2
CREATE TABLE polar_nvarchar2(i int, f1 varchar(3) );
INSERT INTO polar_nvarchar2 (f1) VALUES ('綦容新');
INSERT INTO polar_nvarchar2 (f1) VALUES ('王公硕');
INSERT INTO polar_nvarchar2 (f1) VALUES ('佟逸良');
INSERT INTO polar_nvarchar2 (f1) VALUES ('赵一帆');
INSERT INTO polar_nvarchar2 (f1) VALUES ('刘彦坤');
INSERT INTO polar_nvarchar2 (f1) VALUES ('靳鑫');
INSERT INTO polar_nvarchar2 (f1) VALUES ('𣘗𧄧');
SELECT * FROM polar_nvarchar2 ORDER BY f1;

-- clean 
DROP TABLE polar_nvarchar2;

-- bpchar
CREATE TABLE polar_bpchar(i int, f1 bpchar(3) );
INSERT INTO polar_bpchar (f1) VALUES ('綦容新');
INSERT INTO polar_bpchar (f1) VALUES ('王公硕');
INSERT INTO polar_bpchar (f1) VALUES ('佟逸良');
INSERT INTO polar_bpchar (f1) VALUES ('赵一帆');
INSERT INTO polar_bpchar (f1) VALUES ('刘彦坤');
INSERT INTO polar_bpchar (f1) VALUES ('靳鑫');
INSERT INTO polar_bpchar (f1) VALUES ('𣘗𧄧');
SELECT * FROM polar_bpchar ORDER BY f1;

-- clean 
DROP TABLE polar_bpchar;

-- char
CREATE TABLE polar_char(i int, f1 char(3) );
INSERT INTO polar_char (f1) VALUES ('綦容新');
INSERT INTO polar_char (f1) VALUES ('王公硕');
INSERT INTO polar_char (f1) VALUES ('佟逸良');
INSERT INTO polar_char (f1) VALUES ('赵一帆');
INSERT INTO polar_char (f1) VALUES ('王家1');
INSERT INTO polar_char (f1) VALUES ('王家2');
INSERT INTO polar_char (f1) VALUES ('刘彦坤');
INSERT INTO polar_char (f1) VALUES ('靳鑫');
INSERT INTO polar_char (f1) VALUES ('𣘗𧄧');
SELECT * FROM polar_char ORDER BY f1;

-- clean 
DROP TABLE polar_char;

-- MATERIALIZED VIEW join
CREATE TABLE polar_view_test(i int, n varchar(32));
INSERT INTO polar_view_test VALUES (1, '阮雄风');
INSERT INTO polar_view_test VALUES (2, '鲁晓');
CREATE TABLE polar_view_test2(id int, name varchar(32));
INSERT INTO polar_view_test2 VALUES (1, '一片');
INSERT INTO polar_view_test2 VALUES (2, '海洋');
CREATE MATERIALIZED VIEW T_MATER AS SELECT * FROM polar_view_test  WITH NO DATA;
REFRESH MATERIALIZED VIEW T_MATER;
SELECT * FROM polar_view_test2 n JOIN T_MATER p on n.id=p.i order by i;
SELECT * FROM polar_view_test2 n JOIN T_MATER p on n.id=p.i order by name;
SELECT * FROM polar_view_test2 n JOIN T_MATER p on n.id=p.i order by n;
DROP MATERIALIZED VIEW T_MATER;
DROP TABLE polar_view_test;
DROP TABLE polar_view_test2;

\c postgres
DROP DATABASE db_gbk;
DROP DATABASE db_gb18030;
reset client_encoding;
