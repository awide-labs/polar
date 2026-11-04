-- pg15_px: since that seems bfvs
/*
-- MPP-22453: wrong result in indexscan when the indexqual compares different data types
create table mpp22453(a int, d date);
insert into mpp22453 values (1, '2012-01-01'), (2, '2012-01-02'), (3, '2012-12-31');
create index mpp22453_idx on mpp22453(d);
set polar_px_optimizer_enable_seqscan = off;
select * from mpp22453 where d > date '2012-01-31' + interval '1 day' ;
select * from mpp22453 where d > '2012-02-01';
reset polar_px_optimizer_enable_seqscan;

-- MPP-22791: SIGSEGV when querying a table with default partition only
create table mpp22791(a int, b int) partition by range(b);
CREATE TABLE d PARTITION of mpp22791 default;

insert into mpp22791 values (1, 1), (2, 2), (3, 3);
select * from mpp22791 where b > 1;
select * from mpp22791 where b <= 3;

-- MPP-20713, MPP-20714, MPP-20738: Const table get with a filter
select 1 as x where 1 in (2, 3);

-- MPP-23081: keys of partitioned tables
create table orca.p1(a int) partition by range(a);
CREATE TABLE orcap1_p1 PARTITION of orca.p1 for values from (1) to (10);
CREATE TABLE orcap1_p2 PARTITION of orca.p1 for values from (10) to (20);

insert into orca.p1 select * from generate_series(2,15);
select count(*) from (select _px_worker_id,ctid,tableoid from orca.p1 group by _px_worker_id,ctid,tableoid) as foo;

-- MPP-25194: histograms on text columns are dropped in ORCA. NDVs of these histograms should be added to NDVRemain
CREATE TABLE orca.tmp_verd_s_pp_provtabs_agt_0015_extract1 (
    uid136 character(16),
    tab_nr smallint,
    prdgrp character(5),
    bg smallint,
    typ142 smallint,
    ad_gsch smallint,
    guelt_ab date,
    guelt_bis date,
    guelt_stat text,
    verarb_ab timestamp(5) without time zone,
    u_erzeugen integer,
    grenze integer,
    erstellt_am_122 timestamp(5) without time zone,
    erstellt_am_135 timestamp(5) without time zone,
    erstellt_am_134 timestamp(5) without time zone
);

 set allow_system_table_mods=true;

 UPDATE pg_class
 SET
         relpages = 30915::int, reltuples = 7.28661e+07::real WHERE relname = 'tmp_verd_s_pp_provtabs_agt_0015_extract1' AND relnamespace = (SELECT oid FROM pg_namespace WHERE nspname = 'xvclin');
insert into pg_statistic
values (
  'orca.tmp_verd_s_pp_provtabs_agt_0015_extract1'::regclass,
  1::smallint,
  'f',
  0::real,
  17::integer,
  264682::real,
  1::smallint,
  2::smallint,
  0::smallint,
  0::smallint,
  0::smallint,
  1054::oid,
  1058::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  '{0.000161451,0.000107634,0.000107634,0.000107634,0.000107634,0.000107634,0.000107634,0.000107634,0.000107634,0.000107634,8.07255e-05,8.07255e-05,8.07255e-05,8.07255e-05,8.07255e-05,8.07255e-05,8.07255e-05,8.07255e-05,8.07255e-05,8.07255e-05,8.07255e-05,8.07255e-05,8.07255e-05,8.07255e-05,8.07255e-05}'::real[],
  NULL::real[],
  NULL::real[],
  NULL::real[],
  NULL::real[],
  '{8X8#1F8V92A2025G,YFAXQ§UBF210PA0P,2IIVIE8V92A2025G,9§BP8F8V92A2025G,35A9EE8V92A2025G,§AJ2Z§9MA210PA0P,3NUQ3E8V92A2025G,F7ZD4F8V92A2025G,$WHHEO§§E210PA0P,Z6EATH2BE210PA0P,N7I28E8V92A2025G,YU0K$E$§9210PA0P,3TAI1ANIF210PA0P,P#H8BF8V92A2025G,VTQ$N$D§92A201SC,N7ZD4F8V92A2025G,77BP8F8V92A2025G,39XOXY78H210PA01,#2#OX6NHH210PA01,2DG1J#XZH210PA01,MFEG$E8V92A2025G,M0HKWNGND210PA0P,FSXI67NSA210PA0P,C1L77E8V92A2025G,01#21E8V92A2025G}'::bpchar[],
  '{§00§1HKZC210PA0P,1D90GE8V92A2025G,2ULZI6L0O210PA01,489G7L8$I210PA01,5RE8FF8V92A2025G,76NIRFNIF210PA0P,8KOMKE8V92A2025G,#9Y#GPSHB210PA0P,BDAJ#D8V92A2025G,CV9Z7IYVK210PA01,#EC5FE8V92A2025G,FQWY§O1XC210PA0P,H8HL4E8V92A2025G,INC5FE8V92A2025G,K4MX0XHCF210PA0P,LKE8FF8V92A2025G,N03G9UM2F210PA0P,OHJ$#GFZ9210PA0P,PXU3T1OTB210PA0P,RCUA45F1H210PA01,SU§FRY#QI210PA01,UABHMLSLK210PA01,VRBP8F8V92A2025G,X65#KZIDC210PA0P,YLFG§#A2G210PA0P,ZZG8H29OC210PA0P,ZZZDBCEVA210PA0P}'::bpchar[],
  NULL::bpchar[],
  NULL::bpchar[],
  NULL::bpchar[]
),
(
  'orca.tmp_verd_s_pp_provtabs_agt_0015_extract1'::regclass,
  2::smallint,
  'f',
  0::real,
  2::integer,
  205.116::real,
  1::smallint,
  2::smallint,
  0::smallint,
  0::smallint,
  0::smallint,
  94::oid,
  95::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  '{0.278637,0.272448,0.0303797,0.0301106,0.0249442,0.0234373,0.0231682,0.0191319,0.0169793,0.0162527,0.0142884,0.0141539,0.0125394,0.0103329,0.0098216,0.00944488,0.00850308,0.00715766,0.0066464,0.00656567,0.00591987,0.0050588,0.00454753,0.00449372,0.0044399}'::real[],
  NULL::real[],
  NULL::real[],
  NULL::real[],
  NULL::real[],
  '{199,12,14,5,197,11,198,8,152,299,201,153,9,13,74,179,24,202,2,213,17,195,215,16,200}'::int2[],
  '{1,5,9,12,14,24,58,80,152,195,198,199,207,302,402}'::int2[],
  NULL::int2[],
  NULL::int2[],
  NULL::int2[]
),
(
  'orca.tmp_verd_s_pp_provtabs_agt_0015_extract1'::regclass,
  3::smallint,
  'f',
  0::real,
  6::integer,
  201.005::real,
  1::smallint,
  2::smallint,
  0::smallint,
  0::smallint,
  0::smallint,
  1054::oid,
  1058::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  '{0.0478164,0.0439146,0.0406856,0.0239755,0.0154186,0.0149073,0.0148804,0.0143422,0.0141808,0.0139386,0.0138848,0.0138848,0.0137502,0.0134812,0.0134004,0.0133197,0.0133197,0.013239,0.0131852,0.0130775,0.0130775,0.0130237,0.0129699,0.0129699,0.012943}'::real[],
  NULL::real[],
  NULL::real[],
  NULL::real[],
  NULL::real[],
  '{AG023,AG032,AG999,AG022,AG--B,AG-VB,AG--C,AG-VC,AG014,AG-VA,AG036,AGT4C,AG--A,AG037,AG009,AG015,AG003,AG002,AGT3C,AG025,AG019,AGT2C,AGT1C,AG005,AG031}'::bpchar[],
  '{AG001,AG004,AG007,AG010,AG013,AG017,AG020,AG022,AG023,AG026,AG030,AG032,AG034,AG037,AG040,AG045,AG122,AG999,AG--B,AGT1C,AGT4C,AG-VB,MA017,MA--A,MK081,MKRKV}'::bpchar[],
  NULL::bpchar[],
  NULL::bpchar[],
  NULL::bpchar[]
),
(
  'orca.tmp_verd_s_pp_provtabs_agt_0015_extract1'::regclass,
  4::smallint,
  'f',
  0::real,
  2::integer,
  34::real,
  1::smallint,
  2::smallint,
  0::smallint,
  0::smallint,
  0::smallint,
  94::oid,
  95::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  '{0.1135,0.112881,0.111778,0.100288,0.0895245,0.0851384,0.0821785,0.0723569,0.0565616,0.0368646,0.0309986,0.0160375,0.00621586,0.00594677,0.0050588,0.00489734,0.00487044,0.00487044,0.00468208,0.00462826,0.00460135,0.00441299,0.00438608,0.00417081,0.00414391}'::real[],
  NULL::real[],
  NULL::real[],
  NULL::real[],
  NULL::real[],
  '{1,3,2,7,9,4,5,16,10,6,8,77,12,11,14,13,22,23,64,61,24,51,53,15,54}'::int2[],
  '{1,2,3,4,5,6,7,9,10,14,16,17,53,98}'::int2[],
  NULL::int2[],
  NULL::int2[],
  NULL::int2[]
),
(
  'orca.tmp_verd_s_pp_provtabs_agt_0015_extract1'::regclass,
  5::smallint,
  'f',
  0::real,
  2::integer,
  1::real,
  1::smallint,
  2::smallint,
  0::smallint,
  0::smallint,
  0::smallint,
  94::oid,
  95::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  '{1}'::real[],
  NULL::real[],
  NULL::real[],
  NULL::real[],
  NULL::real[],
  '{1}'::int2[],
  '{1}'::int2[],
  NULL::int2[],
  NULL::int2[],
  NULL::int2[]
),
(
  'orca.tmp_verd_s_pp_provtabs_agt_0015_extract1'::regclass,
  6::smallint,
  'f',
  0::real,
  2::integer,
  2::real,
  1::smallint,
  2::smallint,
  0::smallint,
  0::smallint,
  0::smallint,
  94::oid,
  95::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  '{0.850227,0.149773}'::real[],
  NULL::real[],
  NULL::real[],
  NULL::real[],
  NULL::real[],
  '{1,2}'::int2[],
  '{1,2}'::int2[],
  NULL::int2[],
  NULL::int2[],
  NULL::int2[]
),
(
  'orca.tmp_verd_s_pp_provtabs_agt_0015_extract1'::regclass,
  7::smallint,
  'f',
  0::real,
  4::integer,
  591.134::real,
  1::smallint,
  2::smallint,
  0::smallint,
  0::smallint,
  0::smallint,
  1093::oid,
  1095::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  '{0.26042,0.0859995,0.0709308,0.0616473,0.0567231,0.0303797,0.0109787,0.0106289,0.00990232,0.00987541,0.00979469,0.00944488,0.00820709,0.00718457,0.00626968,0.00621586,0.00616204,0.00600059,0.00586605,0.00557006,0.00516643,0.00511261,0.0050857,0.0050857,0.0047628}'::real[],
  NULL::real[],
  NULL::real[],
  NULL::real[],
  NULL::real[],
  '{1994-01-01,1997-01-01,2005-07-01,1999-01-01,2003-09-01,2000-01-01,2001-01-01,1998-10-01,2001-05-01,1999-03-01,2013-01-01,2003-01-01,2008-01-01,2004-01-01,2009-01-01,2003-02-01,1998-09-01,2000-12-01,2007-04-01,1998-08-01,1998-01-01,2003-07-01,1998-11-01,2005-02-01,1999-04-01}'::date[],
  '{1900-01-01,1994-01-01,1997-01-01,1998-05-07,1999-01-01,1999-05-01,2000-01-01,2001-03-22,2002-10-01,2003-09-01,2004-08-01,2005-07-01,2007-01-01,2008-06-01,2010-04-01,2012-07-01,2014-12-01,2015-01-01}'::date[],
  NULL::date[],
  NULL::date[],
  NULL::date[]
),
(
  'orca.tmp_verd_s_pp_provtabs_agt_0015_extract1'::regclass,
  8::smallint,
  'f',
  0::real,
  4::integer,
  474.232::real,
  1::smallint,
  2::smallint,
  0::smallint,
  0::smallint,
  0::smallint,
  1093::oid,
  1095::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  '{0.52111,0.0709308,0.0591987,0.0587412,0.0148804,0.010279,0.00990232,0.00931034,0.00791109,0.00718457,0.00688857,0.00608132,0.00557006,0.0053817,0.00503189,0.00500498,0.00457444,0.004117,0.00395555,0.00390173,0.00357883,0.00352501,0.00352501,0.00344429,0.00333665}'::real[],
  NULL::real[],
  NULL::real[],
  NULL::real[],
  NULL::real[],
  '{9999-12-31,2005-07-01,1999-01-01,2003-09-01,2004-02-01,2001-05-01,2005-02-01,1999-03-01,1998-10-01,2000-01-01,2003-01-01,1998-09-01,1998-08-01,2008-01-01,2007-04-01,2000-12-01,1999-04-01,1998-11-01,2003-02-01,1994-01-01,2002-09-01,1999-02-01,2004-01-01,1998-07-01,2003-07-01}'::date[],
  '{1994-01-01,1998-11-01,1999-01-01,1999-04-01,2001-05-01,2003-07-01,2003-09-01,2004-02-01,2005-07-01,2007-02-01,2010-03-01,9999-12-31}'::date[],
  NULL::date[],
  NULL::date[],
  NULL::date[]
),
(
  'orca.tmp_verd_s_pp_provtabs_agt_0015_extract1'::regclass,
  9::smallint,
  'f',
  0::real,
  2::integer,
  1::real,
  1::smallint,
  2::smallint,
  0::smallint,
  0::smallint,
  0::smallint,
  98::oid,
  664::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  '{1}'::real[],
  NULL::real[],
  NULL::real[],
  NULL::real[],
  NULL::real[],
  '{H}'::text[],
  '{H}'::text[],
  NULL::text[],
  NULL::text[],
  NULL::text[]
),
(
  'orca.tmp_verd_s_pp_provtabs_agt_0015_extract1'::regclass,
  10::smallint,
  'f',
  0::real,
  8::integer,
  1::real,
  1::smallint,
  2::smallint,
  0::smallint,
  0::smallint,
  0::smallint,
  2060::oid,
  2062::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  '{1}'::real[],
  NULL::real[],
  NULL::real[],
  NULL::real[],
  NULL::real[],
  '{1900-01-01 00:00:00}'::timestamp[],
  '{1900-01-01 00:00:00}'::timestamp[],
  NULL::timestamp[],
  NULL::timestamp[],
  NULL::timestamp[]
),
(
  'orca.tmp_verd_s_pp_provtabs_agt_0015_extract1'::regclass,
  11::smallint,
  'f',
  1::real,
  0::integer,
  -1::real,
  0::smallint,
  0::smallint,
  0::smallint,
  0::smallint,
  0::smallint,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  NULL::real[],
  NULL::real[],
  NULL::real[],
  NULL::real[],
  NULL::real[],
  NULL::int4[],
  NULL::int4[],
  NULL::int4[],
  NULL::int4[],
  NULL::int4[]
),
(
  'orca.tmp_verd_s_pp_provtabs_agt_0015_extract1'::regclass,
  12::smallint,
  'f',
  0::real,
  4::integer,
  1::real,
  1::smallint,
  2::smallint,
  0::smallint,
  0::smallint,
  0::smallint,
  96::oid,
  97::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  '{1}'::real[],
  NULL::real[],
  NULL::real[],
  NULL::real[],
  NULL::real[],
  '{1}'::int4[],
  '{1}'::int4[],
  NULL::int4[],
  NULL::int4[],
  NULL::int4[]
),
(
  'orca.tmp_verd_s_pp_provtabs_agt_0015_extract1'::regclass,
  13::smallint,
  'f',
  0.991927::real,
  8::integer,
  301.416::real,
  1::smallint,
  2::smallint,
  0::smallint,
  0::smallint,
  0::smallint,
  2060::oid,
  2062::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  '{8.07255e-05,2.69085e-05,2.69085e-05,2.69085e-05,2.69085e-05,2.69085e-05,2.69085e-05,2.69085e-05,2.69085e-05,2.69085e-05,2.69085e-05,2.69085e-05,2.69085e-05,2.69085e-05,2.69085e-05,2.69085e-05,2.69085e-05,2.69085e-05,2.69085e-05,2.69085e-05,2.69085e-05,2.69085e-05,2.69085e-05,2.69085e-05,2.69085e-05}'::real[],
  NULL::real[],
  NULL::real[],
  NULL::real[],
  NULL::real[],
  '{2004-01-05 13:45:06.18894,2007-03-20 12:02:33.73888,2009-12-15 12:33:55.32684,2012-09-21 09:58:09.70321,2012-02-13 14:56:03.11625,2000-10-02 09:56:01.24836,1998-01-14 10:11:29.43055,2014-01-08 14:41:44.85935,2012-06-21 12:46:37.48899,2013-07-23 14:27:52.27322,2011-10-27 16:17:10.01694,2005-07-13 16:55:30.7964,2003-06-05 14:53:13.71932,2002-07-22 10:22:31.0967,2011-12-27 16:12:54.85765,2001-01-12 11:40:09.16207,2005-12-30 08:46:31.30943,2007-03-01 08:29:36.765,2011-06-16 09:09:43.8651,2000-12-15 14:33:29.20083,2006-04-25 13:46:46.09684,2011-06-20 16:26:23.65135,2004-01-23 12:37:06.92535,2002-03-04 10:02:08.92547,2003-08-01 10:33:57.33683}'::timestamp[],
  '{1997-12-05 10:59:43.94611,2014-11-18 08:48:18.32773}'::timestamp[],
  NULL::timestamp[],
  NULL::timestamp[],
  NULL::timestamp[]
),
(
  'orca.tmp_verd_s_pp_provtabs_agt_0015_extract1'::regclass,
  14::smallint,
  'f',
  0.329817::real,
  8::integer,
  38109.5::real,
  1::smallint,
  2::smallint,
  0::smallint,
  0::smallint,
  0::smallint,
  2060::oid,
  2062::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  '{0.0715766,0.0621317,0.00546242,0.0044399,0.000134542,8.07255e-05,8.07255e-05,8.07255e-05,8.07255e-05,8.07255e-05,8.07255e-05,8.07255e-05,8.07255e-05,8.07255e-05,8.07255e-05,8.07255e-05,8.07255e-05,8.07255e-05,8.07255e-05,8.07255e-05,8.07255e-05,8.07255e-05,8.07255e-05,8.07255e-05,8.07255e-05}'::real[],
  NULL::real[],
  NULL::real[],
  NULL::real[],
  NULL::real[],
  '{1997-11-25 19:05:00.83798,1998-12-29 16:19:18.50226,1998-01-28 23:01:18.41289,2000-12-21 18:03:30.34549,2003-08-28 11:14:33.26306,2003-08-28 11:14:33.2622,1999-03-20 13:04:33.24015,2003-08-28 11:14:33.22312,2003-08-28 11:14:33.21933,2003-08-28 11:14:33.22082,2003-08-28 11:14:33.21425,2003-08-28 11:14:33.22336,2003-08-28 11:14:33.2092,2003-08-28 11:14:33.20251,2003-08-28 11:14:33.22145,2003-08-28 11:14:33.26235,2003-08-28 11:14:33.26525,2003-08-28 11:14:33.23714,2003-08-28 11:14:33.26667,2003-08-28 11:14:33.23978,2003-08-28 11:14:33.21527,2003-08-28 11:14:33.2227,1999-04-16 09:01:42.92689,2003-08-28 11:14:33.21846,2003-08-28 11:14:33.24725}'::timestamp[],
  '{1997-11-25 19:05:00.83798,1998-01-05 11:57:12.19545,1998-10-14 09:06:01.87217,1998-12-29 16:19:18.50226,1999-01-18 15:01:52.77062,1999-12-28 07:57:37.93632,2001-05-16 10:55:44.78317,2003-05-23 10:32:40.1846,2003-08-28 11:14:33.23985,2004-02-04 14:01:57.60942,2005-07-26 17:01:10.98951,2005-07-26 18:41:33.09864,2006-04-25 16:52:03.49003,2008-02-18 14:17:08.58924,2010-04-19 10:16:19.03194,2012-07-23 10:47:40.65789,2014-12-05 10:59:02.25493}'::timestamp[],
  NULL::timestamp[],
  NULL::timestamp[],
  NULL::timestamp[]
),
(
  'orca.tmp_verd_s_pp_provtabs_agt_0015_extract1'::regclass,
  15::smallint,
  'f',
  0.678255::real,
  8::integer,
  7167.15::real,
  1::smallint,
  2::smallint,
  0::smallint,
  0::smallint,
  0::smallint,
  2060::oid,
  2062::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  0::oid,
  '{0.000376719,0.00034981,0.00034981,0.00034981,0.00034981,0.00034981,0.00034981,0.000322902,0.000322902,0.000322902,0.000295993,0.000295993,0.000295993,0.000295993,0.000295993,0.000295993,0.000295993,0.000295993,0.000295993,0.000295993,0.000295993,0.000295993,0.000295993,0.000269085,0.000269085}'::real[],
  NULL::real[],
  NULL::real[],
  NULL::real[],
  NULL::real[],
  '{2012-09-07 14:14:23.95552,2012-09-07 14:14:36.8171,2008-12-02 09:02:19.06415,2012-03-16 15:57:15.10939,1999-06-08 13:59:56.59862,1998-10-29 14:57:47.93588,1997-07-10 09:55:34.68999,2003-03-26 11:18:44.43314,2002-02-27 15:07:42.12004,2002-02-27 15:07:13.65666,2003-03-26 11:22:07.7484,2013-11-29 13:16:25.79261,2007-09-06 09:14:10.7907,1998-10-29 14:54:04.23854,2003-04-11 07:54:56.90542,2006-03-09 15:42:27.40086,2000-05-31 10:27:52.92485,2006-01-23 17:12:44.80256,2003-01-28 10:44:17.44046,2007-11-01 15:11:21.99194,2006-03-09 15:42:56.14013,2004-03-31 10:58:47.12524,1999-06-08 14:02:11.91465,1997-07-11 14:52:47.95918,1999-06-08 13:58:15.07927}'::timestamp[],
  '{1997-07-09 10:42:54.69421,1997-09-16 10:30:42.71499,1999-06-08 14:32:08.31914,2002-02-27 15:07:13.67355,2003-04-08 16:31:58.80724,2004-05-05 10:18:01.33179,2006-03-13 16:19:59.61215,2007-09-06 09:13:41.71774,2013-11-29 13:16:37.17591,2014-12-03 09:24:25.20945}'::timestamp[],
  NULL::timestamp[],
  NULL::timestamp[],
  NULL::timestamp[]
);

set polar_px_optimizer_segments = 256;
select a.*,  b.guelt_ab as guelt_ab_b, b.guelt_bis as guelt_bis_b
from orca.tmp_verd_s_pp_provtabs_agt_0015_extract1 a
left outer join  orca.tmp_verd_s_pp_provtabs_agt_0015_extract1 b
 ON  a.uid136=b.uid136 and a.tab_nr=b.tab_nr and  a.prdgrp=b.prdgrp and a.bg=b.bg and a.typ142=b.typ142 and a.ad_gsch=b.ad_gsch
AND a.guelt_ab <= b.guelt_ab AND a.guelt_bis > b.guelt_ab
;

set polar_px_optimizer_segments = 3;
set allow_system_table_mods=false;
*/ -- end bfvs


/* pg15_px For what ?
-- warning messages for missing stats
create table foo_missing_stats(a int, b int);
insert into foo_missing_stats select i, i%5 from generate_series(1,20) i;
create table bar_missing_stats(c int, d int);
insert into bar_missing_stats select i, i%8 from generate_series(1,30) i;

analyze foo_missing_stats;
analyze bar_missing_stats;

select count(*) from foo_missing_stats where a = 10;
with x as (select * from foo_missing_stats) select count(*) from x x1, x x2 where x1.a = x2.a;
with x as (select * from foo_missing_stats) select count(*) from x x1, x x2 where x1.a = x2.b;

set allow_system_table_mods=true;
delete from pg_statistic where starelid='foo_missing_stats'::regclass;
delete from pg_statistic where starelid='bar_missing_stats'::regclass;
set allow_system_table_mods=false;

select count(*) from foo_missing_stats where a = 10;
with x as (select * from foo_missing_stats) select count(*) from x x1, x x2 where x1.a = x2.a;
with x as (select * from foo_missing_stats) select count(*) from x x1, x x2 where x1.a = x2.b;

set polar_px_optimizer_print_missing_stats = off;

DROP TABLE IF EXISTS orca.table_with_small_statistic_precision_diff;
CREATE TABLE orca.table_with_small_statistic_precision_diff (
    col1 double precision
);

SET allow_system_table_mods=true;
DELETE FROM pg_statistic WHERE starelid='table_with_small_statistic_precision_diff'::regclass;

INSERT INTO pg_statistic VALUES (
'table_with_small_statistic_precision_diff'::regclass,
1::smallint,
True::boolean,
0::real,
8::integer,
0::real,
1::smallint,
2::smallint,
0::smallint,
0::smallint,
0::smallint,
670::oid,
672::oid,
0::oid,
0::oid,
0::oid,
0::oid,
0::oid,
0::oid,
0::oid,
0::oid,
E'{0.002}'::real[],
NULL::real[],
NULL::real[],
NULL::real[],
NULL::real[],
E'{-0.25475}'::float8[],
E'{-0.3,-0.2547399}'::float8[],
NULL::float8[],
NULL::float8[],
NULL::float8[]);
SET allow_system_table_mods=false;

SELECT *
FROM (
    SELECT
        *
    FROM orca.table_with_small_statistic_precision_diff
    UNION ALL
    SELECT
        *
    FROM orca.table_with_small_statistic_precision_diff
) x;
*/

/*
 pg15_px there is no bitmap index, see an error, what is actually testing
 moreover some tests was badly adopted at polar 11 by using alter table add partition syntax
 (which is only supported at gpdb) and also below tests for AO/AOCO parts
-- Bitmap indexes
drop table if exists orca.bm_test;
create table orca.bm_test (i int, t text);
insert into orca.bm_test select i % 10, (i % 10)::text  from generate_series(1, 100) i;
analyze orca.bm_test;
create index bm_test_idx on orca.bm_test using bitmap (i);

set polar_px_optimizer_enable_bitmapscan=on;
explain (costs off) select * from orca.bm_test where i=2 and t='2';
select * from orca.bm_test where i=2 and t='2';
reset polar_px_optimizer_enable_bitmapscan;

-- Dynamic bitmap indexes
drop table if exists orca.bm_dyn_test;
create table orca.bm_dyn_test (i int, to_be_dropped char(5), j int, t text)  partition by list(j);
CREATE TABLE part0 PARTITION OF orca.bm_dyn_test FOR VALUES IN (0);
CREATE TABLE part1 PARTITION OF orca.bm_dyn_test FOR VALUES IN (1);
CREATE TABLE part2 PARTITION OF orca.bm_dyn_test FOR VALUES IN (2);
CREATE TABLE part3 PARTITION OF orca.bm_dyn_test FOR VALUES IN (3);
CREATE TABLE part4 PARTITION OF orca.bm_dyn_test FOR VALUES IN (4);
insert into orca.bm_dyn_test select i % 10, 'drop', i % 5, (i % 10)::text  from generate_series(1, 100) i;
create index bm_dyn_test_idx on orca.bm_dyn_test using bitmap (i);

alter table orca.bm_dyn_test drop column to_be_dropped;
alter table orca.bm_dyn_test add partition part5 values(5);
insert into orca.bm_dyn_test values(2, 5, '2');

set polar_px_optimizer_enable_bitmapscan=on;
analyze orca.bm_dyn_test;
-- gather on 1 segment because of direct dispatch
explain (costs off) select * from orca.bm_dyn_test where i=2 and t='2';
select * from orca.bm_dyn_test where i=2 and t='2';

reset polar_px_optimizer_enable_bitmapscan;

-- Now, create a partial index
drop table if exists orca.bm_dyn_test_onepart;
create table orca.bm_dyn_test_onepart (i int, to_be_dropped char(5), j int, t text)
   partition by list(j);
CREATE TABLE part0_dyn PARTITION OF orca.bm_dyn_test_onepart FOR VALUES IN (0);
CREATE TABLE part1_dyn PARTITION OF orca.bm_dyn_test_onepart FOR VALUES IN (1);
CREATE TABLE part2_dyn PARTITION OF orca.bm_dyn_test_onepart FOR VALUES IN (2);
CREATE TABLE part3_dyn PARTITION OF orca.bm_dyn_test_onepart FOR VALUES IN (3);
CREATE TABLE part4_dyn PARTITION OF orca.bm_dyn_test_onepart FOR VALUES IN (4);
insert into orca.bm_dyn_test_onepart select i % 10, 'drop', i % 5, (i % 10)::text  from generate_series(1, 100) i;
create index bm_test_idx_part on orca.bm_dyn_test_onepart_1_prt_part2 using bitmap (i);

alter table orca.bm_dyn_test_onepart drop column to_be_dropped;
alter table orca.bm_dyn_test_onepart add partition part5 values(5);
insert into orca.bm_dyn_test_onepart values(2, 5, '2');

set polar_px_optimizer_enable_bitmapscan=on;
set polar_px_optimizer_enable_dynamictablescan = off;
analyze orca.bm_dyn_test_onepart;
-- gather on 1 segment because of direct dispatch
explain (costs off) select * from orca.bm_dyn_test_onepart where i=2 and t='2';
select * from orca.bm_dyn_test_onepart where i=2 and t='2';

reset polar_px_optimizer_enable_dynamictablescan;
reset polar_px_optimizer_enable_bitmapscan;

-- More BitmapTableScan & BitmapIndexScan tests

set polar_px_optimizer_enable_bitmapscan=on;
create schema bm;
-- Bitmap index scan on Heterogeneous parts with dropped columns
drop table if exists bm.het_bm;
create table bm.het_bm (i int, to_be_dropped char(5), j int, t text)  partition by list(j);
CREATE TABLE part0_het_bm PARTITION OF bm.het_bm FOR VALUES IN (0);
CREATE TABLE part1_het_bm PARTITION OF bm.het_bm FOR VALUES IN (1);
CREATE TABLE part2_het_bm PARTITION OF bm.het_bm FOR VALUES IN (2);
CREATE TABLE part3_het_bm PARTITION OF bm.het_bm FOR VALUES IN (3);
CREATE TABLE part4_het_bm PARTITION OF bm.het_bm FOR VALUES IN (4);

insert into bm.het_bm select i % 10, 'drop', i % 5, (i % 10)::text  from generate_series(1, 100) i;
create index het_bm_idx on bm.het_bm using bitmap (i);

alter table bm.het_bm drop column to_be_dropped;
alter table bm.het_bm add partition part5 values(5);
insert into bm.het_bm values(2, 5, '2');

select sum(i) i_sum, sum(j) j_sum, sum(t::integer) t_sum from bm.het_bm where i=2 and t='2';

-- Bitmap index scan on heap parts with dropped columns
drop table if exists bm.hom_bm_heap;
create table bm.hom_bm_heap (i int, to_be_dropped char(5), j int, t text)  partition by list(j);
CREATE TABLE part0 PARTITION OF bm.hom_bm_heap FOR VALUES IN (0);
CREATE TABLE part1 PARTITION OF bm.hom_bm_heap FOR VALUES IN (1);
CREATE TABLE part2 PARTITION OF bm.hom_bm_heap FOR VALUES IN (2);
CREATE TABLE part3 PARTITION OF bm.hom_bm_heap FOR VALUES IN (3);
CREATE TABLE part4 PARTITION OF bm.hom_bm_heap FOR VALUES IN (4);
insert into bm.hom_bm_heap select i % 10, 'drop', i % 5, (i % 10)::text  from generate_series(1, 100) i;
create index hom_bm_heap_idx on bm.hom_bm_heap using bitmap (i);

alter table bm.hom_bm_heap drop column to_be_dropped;
alter table bm.hom_bm_heap add partition part5 values(5);
insert into bm.hom_bm_heap values(2, 5, '2');

select sum(i) i_sum, sum(j) j_sum, sum(t::integer) t_sum from bm.hom_bm_heap where i=2 and t='2';

-- Bitmap index scan on AO parts with dropped columns
drop table if exists bm.hom_bm_ao;
create table bm.hom_bm_ao (i int, to_be_dropped char(5), j int, t text)
with ( compresslevel=5, orientation=row)
 partition by list(j);
 CREATE TABLE part0 PARTITION OF bm.hom_bm_ao FOR VALUES IN (0);
CREATE TABLE part1 PARTITION OF bm.hom_bm_ao FOR VALUES IN (1);
CREATE TABLE part2 PARTITION OF bm.hom_bm_ao FOR VALUES IN (2);
CREATE TABLE part3 PARTITION OF bm.hom_bm_ao FOR VALUES IN (3);
CREATE TABLE part4 PARTITION OF bm.hom_bm_ao FOR VALUES IN (4);
insert into bm.hom_bm_ao select i % 10, 'drop', i % 5, (i % 10)::text  from generate_series(1, 100) i;
create index hom_bm_ao_idx on bm.hom_bm_ao using bitmap (i);

alter table bm.hom_bm_ao drop column to_be_dropped;
alter table bm.hom_bm_ao add partition part5 values(5);
insert into bm.hom_bm_ao values(2, 5, '2');

select sum(i) i_sum, sum(j) j_sum, sum(t::integer) t_sum from bm.hom_bm_ao where i=2 and t='2';

-- Bitmap index scan on AOCO parts with dropped columns
drop table if exists bm.hom_bm_aoco;
create table bm.hom_bm_aoco (i int, to_be_dropped char(5), j int, t text)
with ( compresslevel=5, orientation=column)
 partition by list(j);
CREATE TABLE part0 PARTITION OF bm.hom_bm_aoco FOR VALUES IN (0);
CREATE TABLE part1 PARTITION OF bm.hom_bm_aoco FOR VALUES IN (1);
CREATE TABLE part2 PARTITION OF bm.hom_bm_aoco FOR VALUES IN (2);
CREATE TABLE part3 PARTITION OF bm.hom_bm_aoco FOR VALUES IN (3);
CREATE TABLE part4 PARTITION OF bm.hom_bm_aoco FOR VALUES IN (4);
insert into bm.hom_bm_aoco select i % 10, 'drop', i % 5, (i % 10)::text  from generate_series(1, 100) i;
create index hom_bm_aoco_idx on bm.hom_bm_aoco using bitmap (i);

alter table bm.hom_bm_aoco drop column to_be_dropped;
alter table bm.hom_bm_aoco add partition part5 values(5);
insert into bm.hom_bm_aoco values(2, 5, '2');

select sum(i) i_sum, sum(j) j_sum, sum(t::integer) t_sum from bm.hom_bm_aoco where i=2 and t='2';

-- Create index before dropped column
drop table if exists bm.het_bm;
create table bm.het_bm (i int, to_be_dropped char(5), j int, t text)  partition by list(j);
CREATE TABLE part0 PARTITION OF bm.het_bm FOR VALUES IN (0);
CREATE TABLE part1 PARTITION OF bm.het_bm FOR VALUES IN (1);
CREATE TABLE part2 PARTITION OF bm.het_bm FOR VALUES IN (2);
CREATE TABLE part3 PARTITION OF bm.het_bm FOR VALUES IN (3);
CREATE TABLE part4 PARTITION OF bm.het_bm FOR VALUES IN (4);
insert into bm.het_bm select i % 10, 'drop', i % 5, (i % 10)::text  from generate_series(1, 100) i;

-- Create index before dropping a column
create index het_bm_i_idx on bm.het_bm using bitmap (i);
create index het_bm_j_idx on bm.het_bm using bitmap (j);

alter table bm.het_bm drop column to_be_dropped;
alter table bm.het_bm add partition part5 values(5);
insert into bm.het_bm values(2, 5, '2');

select sum(i) i_sum, sum(j) j_sum, sum(t::integer) t_sum from bm.het_bm where i=2 and j=2;
select sum(i) i_sum, sum(j) j_sum, sum(t::integer) t_sum from bm.het_bm where i=2 and t='2';

-- Drop and recreate index after we dropped column
drop index bm.het_bm_i_idx;
drop index bm.het_bm_j_idx;
create index het_bm_i_idx on bm.het_bm using bitmap (i);
create index het_bm_j_idx on bm.het_bm using bitmap (j);

select sum(i) i_sum, sum(j) j_sum, sum(t::integer) t_sum from bm.het_bm where j=2 or j=3;

-- Create index after dropped column
drop table if exists bm.het_bm;
create table bm.het_bm (i int, to_be_dropped char(5), j int, t text)  partition by list(j);
CREATE TABLE part0 PARTITION OF bm.het_bm FOR VALUES IN (0);
CREATE TABLE part1 PARTITION OF bm.het_bm FOR VALUES IN (1);
CREATE TABLE part2 PARTITION OF bm.het_bm FOR VALUES IN (2);
CREATE TABLE part3 PARTITION OF bm.het_bm FOR VALUES IN (3);
CREATE TABLE part4 PARTITION OF bm.het_bm FOR VALUES IN (4);
insert into bm.het_bm select i % 10, 'drop', i % 5, (i % 10)::text  from generate_series(1, 100) i;

alter table bm.het_bm drop column to_be_dropped;
alter table bm.het_bm add partition part5 values(5);

-- Create index after dropping a column but before we insert into newly created part
create index het_bm_i_idx on bm.het_bm using bitmap (i);
create index het_bm_j_idx on bm.het_bm using bitmap (j);
insert into bm.het_bm values(2, 5, '2');

select sum(i) i_sum, sum(j) j_sum, sum(t::integer) t_sum from bm.het_bm where j=2 or j=3;

-- Rescan bitmap index
drop table if exists bm.outer_tab;
create table bm.outer_tab
(
id serial,
code character varying(25),
name character varying(40)
);

insert into bm.outer_tab select i % 10, i % 5, i % 2 from generate_series(1, 100) i;

set polar_px_optimizer_enable_hashjoin = off;
select count(1) from bm.outer_tab;
select count(1) from bm.het_bm;

select sum(id) id_sum, sum(i) i_sum, sum(j) j_sum from bm.outer_tab as ot join bm.het_bm as het_bm on ot.id = het_bm.j
where het_bm.i between 2 and 5;

set client_min_messages='warning';
drop schema bm cascade;
reset polar_px_optimizer_enable_hashjoin;
reset polar_px_optimizer_enable_bitmapscan;
-- End of BitmapTableScan & BitmapIndexScan tests
*/

/* pg15_px broken test: some of xforms droppen
   also the px test query is commented with '--', original query
   from gpdb were added, but it fallbacks
set polar_px_optimizer_enable_constant_expression_evaluation=on;

CREATE TABLE my_tt_agg_opt (
    symbol character(16),
    event_ts bigint,
    trade_price numeric,
    trade_volume bigint
);

CREATE TABLE my_tq_agg_opt_part (
    ets bigint,
    sym character varying(16),
    bid_price numeric,
    ask_price numeric,
    end_ts bigint
);

CREATE TABLE my_tq_agg_opt_part_p1 PARTITION of my_tq_agg_opt_part for values from (0) to (200000);
CREATE TABLE my_tq_agg_opt_part_p2 PARTITION of my_tq_agg_opt_part for values from (200000) to (900000);

CREATE INDEX my_tq_agg_opt_part_ets_end_ts_ix_1 ON my_tq_agg_opt_part_1_prt_p1 USING btree (ets, end_ts);
CREATE INDEX my_tq_agg_opt_part_ets_end_ts_ix_2 ON my_tq_agg_opt_part_1_prt_p2 USING btree (ets);

CREATE FUNCTION plusone(numeric) RETURNS numeric AS $$
BEGIN
    SELECT $1 + 1;
END;
$$ LANGUAGE plpgsql volatile;

-- select disable_xform('CXformInnerJoin2DynamicIndexGetApply'); not exists
select disable_xform('CXformInnerJoin2HashJoin');
-- select disable_xform('CXformInnerJoin2IndexGetApply'); not exists
select disable_xform('CXformInnerJoin2NLJoin');

set polar_px_optimizer_enable_indexjoin=on;

-- force_explain
SET polar_px_optimizer_segments = 3;
-- explain (costs off) select c tt, my_tq_agg_opt_part tq
-- WHERE tq.sym = tt.symbol AND
--       plusone(tq.bid_price) < tt.trade_price AND
--       tt.event_ts >= tq.ets AND
--       tt.event_ts <  tq.end_ts
-- GROUP BY 1
-- ORDER BY 1 asc ;
EXPLAIN (costs off)
SELECT (tt.event_ts / 100000) / 5 * 5 as fivemin, COUNT(*)
FROM my_tt_agg_opt tt, my_tq_agg_opt_part tq
WHERE tq.sym = tt.symbol AND
      plusone(tq.bid_price) < tt.trade_price AND
      tt.event_ts >= tq.ets AND
      tt.event_ts <  tq.end_ts
GROUP BY 1
ORDER BY 1 asc ;
reset polar_px_optimizer_segments;
reset polar_px_optimizer_enable_constant_expression_evaluation;
reset polar_px_optimizer_enable_indexjoin;

-- select enable_xform('CXformInnerJoin2DynamicIndexGetApply'); not exists
select enable_xform('CXformInnerJoin2HashJoin');
-- select enable_xform('CXformInnerJoin2IndexGetApply'); not exists
select enable_xform('CXformInnerJoin2NLJoin');
*/

/* pg15_px again bfv
-- MPP-25661: IndexScan crashing for qual with reference to outer tuple
drop table if exists idxscan_outer;
create table idxscan_outer
(
id serial,
code character varying(25),
name character varying(40)
);

drop table if exists idxscan_inner;
create table idxscan_inner
(
ordernum int unique,
productid int,
comment character varying(40)
);

-- Have more rows in one table, so that the planner will
-- choose to make that the outer side of the join.
insert into idxscan_outer values (1, 'a', 'aaa');
insert into idxscan_outer values (2, 'b', 'bbb');
insert into idxscan_outer values (3, 'c', 'ccc');
insert into idxscan_outer values (4, 'd', 'ddd');
insert into idxscan_outer values (5, 'e', 'eee');
insert into idxscan_outer values (6, 'f', 'fff');
insert into idxscan_outer values (7, 'g', 'ggg');
insert into idxscan_outer values (8, 'h', 'hhh');
insert into idxscan_outer values (9, 'i', 'iii');

insert into idxscan_inner values (11, 1, 'xxxx');
insert into idxscan_inner values (24, 2, 'yyyy');
insert into idxscan_inner values (13, 3, 'zzzz');

analyze idxscan_outer;
analyze idxscan_inner;

set polar_px_optimizer_enable_hashjoin = off;

explain (costs off) select id, comment from idxscan_outer as o join idxscan_inner as i on o.id = i.productid
where ordernum between 10 and 20;

select id, comment from idxscan_outer as o join idxscan_inner as i on o.id = i.productid
where ordernum between 10 and 20;

reset polar_px_optimizer_enable_hashjoin;

drop table idxscan_outer;
drop table idxscan_inner;

-- MPP-25700 Fix to TO_DATE on hybrid datums during constant expression evaluation
drop table if exists orca.t3;
create table orca.t3 (c1 timestamp without time zone);
insert into orca.t3 values  ('2015-07-03 00:00:00'::timestamp without time zone);
select to_char(c1, 'YYYY-MM-DD HH24:MI:SS') from orca.t3 where c1 = TO_DATE('2015-07-03','YYYY-MM-DD');
select to_char(c1, 'YYYY-MM-DD HH24:MI:SS') from orca.t3 where c1 = '2015-07-03'::date;

-- MPP-25806: multi-column index
create table orca.index_test (a int, b int, c int, d int, e int, constraint index_test_pkey PRIMARY KEY (a, b, c, d));
insert into orca.index_test select i,i%2,i%3,i%4,i%5 from generate_series(1,100) i;
analyze orca.index_test;
-- force_explain
explain (costs off) select * from orca.index_test where a = 5;

-- force_explain
explain (costs off) select * from orca.index_test where c = 5;

-- force_explain
explain (costs off) select * from orca.index_test where a = 5 and c = 5;

*/


/* pg15_px DML
--
-- Test to ensure sane behavior when DML queries are optimized by ORCA by
-- enforcing a non-master gather motion, controlled by
-- optimizer_enable_gather_on_segment_for_DML GUC
--

--
-- CTAS with global-local aggregation
--
create table test1 (a int, b int);
insert into test1 select generate_series(1,100),generate_series(1,100);
create table t_new as select avg(a) from test1 join (select i from unnest(array[1,2,3]) i) t on (test1.a = t.i);
select * from t_new;

drop table t_new;
set polar_px_optimizer_enable_gather_on_segment_for_DML=off;
create table t_new as select avg(a) from test1 join (select i from unnest(array[1,2,3]) i) t on (test1.a = t.i);
select * from t_new;

reset polar_px_optimizer_enable_gather_on_segment_for_DML;

--
-- Insert with outer references in the subquery
--
create table x_tab(a int);
create table y_tab(a int);
create table z_tab(a int);

insert into x_tab values(1);
insert into y_tab values(0);
insert into z_tab values(1);

insert into x_tab select * from x_tab where exists (select * from x_tab where x_tab.a = (select x_tab.a + y_tab.a from y_tab));
select * from x_tab;

--
-- Insert with Union All with an universal child
--
insert into y_tab select 1 union all select a from x_tab limit 10;
select * from y_tab;

--
-- Insert with a function containing a SQL
--
create or replace function test_func_pg_stats()
returns integer
as $$ declare cnt int; begin execute 'select count(*) from pg_statistic' into cnt; return cnt; end $$
language plpgsql volatile READS SQL DATA;

insert into y_tab select test_func_pg_stats() from x_tab limit 2;
select count(*) from y_tab;

--
-- Delete with Hash Join with a universal child
--
delete from x_tab where exists (select z_tab.a from z_tab join (select 1 as g) as tab on z_tab.a = tab.g);
select * from x_tab;

drop table bar;
*/
--------------------------------------------

/* pg15_px gpexpand test
-- test gpexpand phase 1
-- right now, these will fall back to planner

drop table if exists noexp_hash, gpexp_hash, gpexp_rand, gpexp_repl;
create table noexp_hash(a int, b int) ;
insert into  noexp_hash select i, i from generate_series(1,50) i;
analyze noexp_hash;

-- three tables that will be expanded (simulated)
create table gpexp_hash(a int, b int) ;
create table gpexp_rand(a int, b int) ;
create table gpexp_repl(a int, b int) distributed replicated;

-- simulate a cluster with one segment less than we have now
set allow_system_table_mods = true;
update gp_distribution_policy set numsegments = numsegments-1 where localoid = 'gpexp_hash'::regclass and numsegments > 1;
update gp_distribution_policy set numsegments = numsegments-1 where localoid = 'gpexp_rand'::regclass and numsegments > 1;
update gp_distribution_policy set numsegments = numsegments-1 where localoid = 'gpexp_repl'::regclass and numsegments > 1;
reset allow_system_table_mods;

-- populate the tables on this smaller cluster
explain (costs off) insert into gpexp_hash select i, i from generate_series(1,50) i;

insert into gpexp_hash select i, i from generate_series(1,50) i;
insert into gpexp_rand select i, i from generate_series(1,50) i;
insert into gpexp_repl select i, i from generate_series(1,50) i;
analyze gpexp_hash;
analyze gpexp_rand;
analyze gpexp_repl;

-- the segment ids in the unmodified table should have one extra number
select max(noexp_hash._px_worker_id) - max(gpexp_hash._px_worker_id) as expect_one
from noexp_hash, gpexp_hash;

-- join should have a redistribute motion for gpexp_hash
explain (costs off) select count(*) from noexp_hash n join gpexp_hash x on n.a=x.a;
select count(*) from noexp_hash n join gpexp_hash x on n.a=x.a;
delete from gpexp_hash where b between 21 and 50;
select count(*) from gpexp_hash;
update gpexp_hash set b=-1 where b between 11 and 100;
select b, count(*) from gpexp_hash group by b order by b;

explain (costs off) update gpexp_rand set b=(select b from gpexp_hash where gpexp_rand.a = gpexp_hash.a);
update gpexp_rand set b=(select b from gpexp_hash where gpexp_rand.a = gpexp_hash.a);
select b, count(*) from gpexp_rand group by b order by b;

delete from gpexp_repl where b >= 20;
explain (costs off) insert into gpexp_repl values (20, 20);
insert into gpexp_repl values (20, 20);

explain (costs off) select count(*) from gpexp_hash h join gpexp_repl r on h.a=r.a;
select count(*) as expect_20 from gpexp_hash h join gpexp_repl r on h.a=r.a;
explain (costs off) select count(*) as expect_20 from noexp_hash h join gpexp_repl r on h.a=r.a;
select count(*) as expect_20 from noexp_hash h join gpexp_repl r on h.a=r.a;
*/


/* pg15_px remove - gpdb AO tables
-- negative test, make sure we don't use a btree scan on an AO table
select disable_xform('CXformSelect2BitmapBoolOp');
select disable_xform('CXformSelect2DynamicBitmapBoolOp');
select disable_xform('CXformJoin2BitmapIndexGetApply');
select disable_xform('CXformInnerJoin2NLJoin');

-- Make sure we don't allow a regular (btree) index scan or index join for an AO table
-- We disabled hash join, and bitmap index joins, NLJs, so this should leave ORCA no other choices
-- expect a sequential scan, not an index scan, from these two queries
explain (costs off) select * from t_ao_btree where a = 3 and b = 3;
explain (costs off) select * from tpart_ao_btree where a = 3 and b = 3;
-- expect a fallback for all four of these queries
select * from tpart_dim d join t_ao_btree f on d.a=f.a where d.b=1;
select * from tpart_dim d join tpart_ao_btree f on d.a=f.a where d.b=1;

select enable_xform('CXformSelect2BitmapBoolOp');
select enable_xform('CXformSelect2DynamicBitmapBoolOp');
select enable_xform('CXformJoin2BitmapIndexGetApply');
select enable_xform('CXformInnerJoin2NLJoin');

reset polar_px_optimizer_enable_hashjoin;
*/

/* pg15_px validate test cases
-- Tests converted from MDPs that use tables partitioned on text columns and similar types,
-- which can't be handled in ORCA MDPs, since they would require calling the GPDB executor

-- GroupingOnSameTblCol-2.mdp
-- from dxl

create table asset_records(
  uid varchar,
  hostname varchar,
  active boolean,
  os varchar,
  vendor varchar,
  asset_type varchar,
  create_ts timestamp
)
partition by range(create_ts);

CREATE TABLE asset_records_p1 PARTITION of asset_records for values from ('2000-01-01') to ('2001-01-01');
CREATE TABLE asset_records_p2 PARTITION of asset_records for values from ('2001-01-01') to ('2002-01-01');
CREATE TABLE asset_records_p3 PARTITION of asset_records for values from ('2002-01-01') to ('2003-01-01');
CREATE TABLE asset_records_p4 PARTITION of asset_records for values from ('2003-01-01') to ('2004-01-01');
CREATE TABLE asset_records_p5 PARTITION of asset_records for values from ('2004-01-01') to ('2005-01-01');

create table coverage(
  date timestamp,
  hostname varchar,
  vendor_sla int
)
;

insert into asset_records
select 'u', 'h'||i::text, false, 'o', 'v', 'a', timestamp '2000-03-01 00:00:00' + i * interval '1' minute
from generate_series(1,100000) i;

analyze asset_records;

explain (costs off)
select asset_records.uid, asset_records.hostname, asset_records.asset_type, asset_records.os, asset_records.create_ts, 1
from asset_records left join coverage
     on upper(asset_records.hostname::text) = upper(coverage.hostname::text)
        and asset_records.create_ts = coverage.date
        and (asset_type::text = 'xx' or asset_type::text = 'yy')
        and asset_records.active
where  upper(coalesce(vendor, 'none')::text) <> 'some_vendor' and vendor_sla is not null
group by asset_records.uid, asset_records.hostname, asset_records.asset_type, asset_records.os, asset_records.create_ts;

-- IndexApply-PartResolverExpand.mdp
-- from comment

create table x ( i text, j text, k text, m text)  ;
create table y ( i text, j text, k text, m text)
PARTITION BY RANGE(i);
CREATE TABLE y_p1 PARTITION of y for values from ('a') to ('a');
CREATE TABLE y_p2 PARTITION of y for values from ('a') to ('i');
CREATE TABLE y_p3 PARTITION of y for values from ('i') to ('q');
CREATE TABLE y_p4 PARTITION of y for values from ('q') to ('x');
CREATE TABLE y_deflt PARTITION of y default;
create INDEX y_idx on y (j);

set polar_px_optimizer_enable_indexjoin=on;
explain (costs off) select count(*) from x, y where (x.i > y.j AND x.j <= y.i);
reset polar_px_optimizer_enable_indexjoin;

-- InferPredicatesBCC-vcpart-txt.mdp
-- from comment

create table infer_txt (a text);
insert into infer_txt select * from generate_series(1,1000);
analyze infer_txt;
CREATE TABLE infer_part_vc (id int, gender varchar(1))
  PARTITION BY LIST (gender);
CREATE TABLE girls PARTITION OF infer_part_vc FOR VALUES IN ('F');
CREATE TABLE boys PARTITION OF infer_part_vc FOR VALUES IN ('M');
CREATE TABLE other PARTITION OF infer_part_vc default;
insert into infer_part_vc select i, substring(i::varchar, 1, 1) from generate_series(1, 1000) i;
analyze infer_part_vc;

explain (costs off) select * from infer_part_vc inner join infer_txt on (infer_part_vc.gender = infer_txt.a) and infer_txt.a = 'M';

-- NewBtreeIndexScanCost.mdp
-- from comment

CREATE TABLE oip (
    id bigint NOT NULL,
    oid bigint,
    cidr inet NOT NULL,
    state smallint,
    asn text,
    cc text,
    expire timestamp without time zone NOT NULL,
    ts timestamp without time zone NOT NULL,
    metadata json
) ;

CREATE TABLE ria (
    id bigint NOT NULL,
    ip inet,
    file_id bigint DEFAULT (-1),
    auth_filter_id bigint,
    oid bigint,
    event_id bigint,
    ip_source inet,
    arecord inet,
    subdomain text,
    asn character varying,
    cc character varying,
    ts timestamp without time zone,
    filter_match smallint,
    filter_match_id bigint
) ;

explain (costs off) select *  from oip oip  join ria a on ip=cidr and oip.oid=194073;

-- PartTbl-ArrayCoerce.mdp
-- from comment

CREATE TABLE pt (id int, gender varchar(2))
PARTITION BY LIST (gender);
CREATE TABLE girls PARTITION OF infer_part_vc FOR VALUES IN ('F');
CREATE TABLE boys PARTITION OF infer_part_vc FOR VALUES IN ('M');
CREATE TABLE other PARTITION OF infer_part_vc default;
explain (costs off) select * from pt where gender in ( 'F', 'FM');

-- PartTbl-List-DPE-Varchar-Predicates.mdp
-- from comment

-- reuse DDL from previous test case
explain (costs off)
select * from pt where gender = 'F' union all
select * from pt where gender <= 'M' union all
select * from pt where gender in ('F', 'FM') union all
select * from pt where gender is null;

-- PartTbl-RangeJoinPred.mdp
-- from comment (query) and dxl (ddl)

create table stg_xdr_crce_cdr(
  id bigint,
  sessioncreationtimestamp timestamptz,
  subscriberaddress varchar
)
partition by range(sessioncreationtimestamp);
CREATE TABLE stg_xdr_crce_cdr_p1 PARTITION of stg_xdr_crce_cdr for values from ('2010-01-01 00:00:00') to ('2011-01-01 00:00:00');
CREATE TABLE stg_xdr_crce_cdr_p2 PARTITION of stg_xdr_crce_cdr for values from ('2011-01-01 00:00:00') to ('2012-01-01 00:00:00');

create table dim_customer_device(
  imei varchar,
  msisdn varchar,
  start_dtm timestamptz,
  end_dtm timestamptz,
  mkt_model varchar
)
;

explain (costs off)
select *
from  (select subscriberaddress,sessioncreationtimestamp from stg_xdr_crce_cdr) f,
       dim_customer_device d
where d.msisdn=f.subscriberaddress and
      f.sessioncreationtimestamp >= d.start_dtm and
      f.sessioncreationtimestamp <d.end_dtm and
      (mkt_model like '%IPHONE%');

-- PartTbl-Relabel-Equality.mdp
-- from dxl

create table ds_4(
  month_id varchar,
  cust_group_acc numeric(5,2),
  mobile_no varchar
)
partition by list(month_id) (values('Jan', 'Feb', 'Mar'), values('Apr', 'May', 'Jun'));
CREATE TABLE ds_4_p1 PARTITION OF ds_4 FOR VALUES IN ('Jan', 'Feb', 'Mar');
CREATE TABLE ds_4_p2 PARTITION OF ds_4 FOR VALUES IN ('Apr', 'May', 'Jun');

explain (costs off)
select month_id, cust_group_acc, mobile_no
from ds_4
where month_id::text = 'Apr';

-- PartTbl-Relabel-Range.mdp
-- from dxl

-- reuse DDL from previous example

-- currently falls back, "non-trivial part filter not supported"
explain (costs off)
select month_id, cust_group_acc, mobile_no
from ds_4
where month_id::text >= 'Feb' and month_id::text < 'Mar';


-- retail_28.mdp
-- from comment (query) and dxl (ddl)

create table order_lineitems(
  order_id varchar,
  item_shipment_status_code varchar,
  order_datetime timestamp
)
partition by range(order_datetime);
CREATE TABLE order_lineitems_p1 PARTITION of order_lineitems for values from ('2010-01-01'::timestamp) to ('2010-02-01'::timestamp);
CREATE TABLE order_lineitems_p1 PARTITION of order_lineitems for values from ('2010-02-01'::timestamp) to ('2010-03-01'::timestamp);
CREATE TABLE order_lineitems_p1 PARTITION of order_lineitems for values from ('2010-03-01'::timestamp) to ('2010-04-01'::timestamp);
CREATE TABLE order_lineitems_p1 PARTITION of order_lineitems for values from ('2010-04-01'::timestamp) to ('2010-05-01'::timestamp);
CREATE TABLE order_lineitems_p1 PARTITION of order_lineitems for values from ('2010-05-01'::timestamp) to ('2010-06-01'::timestamp);
CREATE TABLE order_lineitems_p1 PARTITION of order_lineitems for values from ('2010-06-01'::timestamp) to ('2010-07-01'::timestamp);
CREATE TABLE order_lineitems_p1 PARTITION of order_lineitems for values from ('2010-07-01'::timestamp) to ('2010-08-01'::timestamp);
CREATE TABLE order_lineitems_p1 PARTITION of order_lineitems for values from ('2010-08-01'::timestamp) to ('2010-09-01'::timestamp);
CREATE TABLE order_lineitems_p1 PARTITION of order_lineitems for values from ('2010-10-01'::timestamp) to ('2010-10-01'::timestamp);
CREATE TABLE order_lineitems_p1 PARTITION of order_lineitems for values from ('2010-11-01'::timestamp) to ('2010-11-01'::timestamp);
CREATE TABLE order_lineitems_p1 PARTITION of order_lineitems for values from ('2010-12-01'::timestamp) to ('2011-01-01'::timestamp);


-- currently falls back, "non-trivial part filter not supported"
explain (costs off)
SELECT to_char(order_datetime,'YYYY-Q') as ship_month
,      item_shipment_status_code
,      COUNT(DISTINCT order_id) AS num_orders
FROM   order_lineitems
WHERE  order_datetime BETWEEN timestamp '2010-04-01' AND date '2010-06-30'
GROUP BY to_char(order_datetime,'YYYY-Q')
,      item_shipment_status_code
ORDER BY to_char(order_datetime,'YYYY-Q')
,      item_shipment_status_code
;
*/

/* tto_15_px fix or remove
-- test index left outer joins on bitmap and btree indexes on partitioned tables with and without select clause
DROP TABLE IF EXISTS touter, tinner;
CREATE TABLE touter(a int, b int) ;
CREATE TABLE tinnerbitmap(a int, b int)  PARTITION BY range(b);
CREATE TABLE tinnerbtree(a int, b int)  PARTITION BY range(b);

CREATE TABLE tinnerbitmap_p1 PARTITION of tinnerbitmap for values from (0) to (3);
CREATE TABLE tinnerbitmap_p2 PARTITION of tinnerbitmap for values from (3) to (6);
CREATE TABLE tinnerbtree_p1 PARTITION of tinnerbtree for values from (0) to (3);
CREATE TABLE tinnerbtree_p2 PARTITION of tinnerbtree for values from (3) to (6);


INSERT INTO touter SELECT i, i%6 FROM generate_series(1,10) i;
INSERT INTO tinnerbitmap select i, i%6 FROM generate_series(1,1000) i;
INSERT INTO tinnerbtree select i, i%6 FROM generate_series(1,1000) i;
CREATE INDEX tinnerbitmap_ix ON tinnerbitmap USING bitmap(a);
CREATE INDEX tinnerbtree_ix ON tinnerbtree USING btree(a);

:prefix_cmd SELECT * FROM touter LEFT JOIN tinnerbitmap ON touter.a = tinnerbitmap.a;
:prefix_cmd SELECT * FROM touter LEFT JOIN tinnerbitmap ON touter.a = tinnerbitmap.a AND tinnerbitmap.b=10;

:prefix_cmd SELECT * FROM touter LEFT JOIN tinnerbtree ON touter.a = tinnerbtree.a;
:prefix_cmd SELECT * FROM touter LEFT JOIN tinnerbtree ON touter.a = tinnerbtree.a AND tinnerbtree.b=10;
*/

/* pg15_px bitmap again
-- Test Bitmap index scan with in list
CREATE TABLE bitmap_test as SELECT * FROM generate_series(1,100) as a ;
ANALYZE bitmap_test;
CREATE INDEX bitmap_index ON bitmap_test USING BITMAP(a);
explain (costs off) select * FROM bitmap_test WHERE a in (1);
explain (costs off) select * FROM bitmap_test WHERE a in (1, 47);
explain (costs off) select * FROM bitmap_test WHERE a in ('2', 47);
explain (costs off) select * FROM bitmap_test WHERE a in ('1', '2');
explain (costs off) select * FROM bitmap_test WHERE a in ('1', '2', 47);
*/

/*pg15_px remove
-- bitmap scan on bitmap index
-- create index tc_idx on tc using bitmap(c);
select * from tc where c='a';
-- explain (costs off) select * from tc where c='a';
*/
