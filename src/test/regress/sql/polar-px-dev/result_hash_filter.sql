-- For target queries optionally specify EXPLAIN (COSTS OFF, VERBOSE) option from outside
\getenv explain_only EXPLAIN_ONLY

\if :explain_only
    \set opt_explain 'EXPLAIN (COSTS OFF, VERBOSE)'
    \set order_by_all ;
\else
    \set opt_explain ''
    -- Sort output and exclude header
    \t on
    \set order_by_all '\\g | LC_ALL=C sort -h'
\endif

create schema result_hash_filter;
-- start_ignore
SET search_path to result_hash_filter;
-- end_ignore


CREATE TABLE select_hash1_tb_part_1 (
    pk numeric(5,0) DEFAULT NULL::numeric NOT NULL,
    varchar2_test character varying(100) DEFAULT NULL::character varying,
    nvarchar2_test character varying(100) DEFAULT NULL::character varying,
    number_test numeric(10,2) DEFAULT NULL::numeric,
    smallint_test numeric(5,0) DEFAULT NULL::numeric,
    integer_test integer,
    decimal_test numeric(10,2) DEFAULT NULL::numeric,
    float_test numeric(10,2) DEFAULT NULL::numeric,
    double_test double precision,
    timestamp_test timestamp(6) without time zone DEFAULT NULL::timestamp without time zone,
    char_size_test character(100) DEFAULT NULL::bpchar,
    nchar_test character(10) DEFAULT NULL::bpchar
);


--
-- Data for Name: select_hash1_tb_part_1; Type: TABLE DATA; Schema: public; Owner: zx_test
--

COPY select_hash1_tb_part_1 (pk, varchar2_test, nvarchar2_test, number_test, smallint_test, integer_test, decimal_test, float_test, double_test, timestamp_test, char_size_test, nchar_test) FROM stdin;
3	nihaore	nihaore	5.34	12	12	40.48	5.34	40.4784500000000023	2012-08-20 01:45:09	nihaore                                                                                             	nihaore   
11	einoejk	einoejk	4.12	20	20	200.49	4.12	200.487869999999987	2012-08-20 01:45:09	einoejk                                                                                             	einoejk   
17	\N	\N	5.34	26	26	1825.47	5.34	1825.47484000000009	2019-01-15 07:45:09	word23                                                                                              	word23    
18	hello1234	hello1234	6.78	27	27	2000.23	6.78	2000.23232000000007	2018-08-15 07:45:09	feed32feed                                                                                          	feed32feed
21	feed32feed	feed32feed	5.78	30	30	35.15	5.78	35.1477999999999966	2020-12-18 13:45:00	hellorew                                                                                            	hellorew  
45	cdefeed	cdefeed	5.34	54	54	50.49	5.34	50.4874500000000026	2020-12-18 13:45:00	safdwe                                                                                              	safdwe    
64	cdefeed	cdefeed	0.45	73	73	48.15	0.45	48.1477999999999966	2028-12-16 13:45:00	he343243                                                                                            	he343243  
66	afsabcabcd	afsabcabcd	2.34	75	75	55.15	2.34	55.1477999999999966	2018-08-15 07:45:09	feed32feed                                                                                          	feed32feed
71	zhuoXUE	zhuoXUE	4.32	80	80	200.49	4.32	200.487869999999987	2016-12-15 14:45:09	cdefeed                                                                                             	cdefeed   
79	nihaore	nihaore	7.89	88	88	10.21	7.89	10.2144999999999992	2016-12-15 14:45:09	hello1234                                                                                           	hello1234 
86	sfdeiekd	sfdeiekd	4.23	95	95	55.15	4.23	55.1477999999999966	2008-12-08 13:45:00	abdfeed                                                                                             	abdfeed   
88	kisfe	kisfe	6.78	97	97	80.46	6.78	80.457800000000006	2028-12-16 13:45:00	adaabcwer                                                                                           	adaabcwer 
89	safdwe	safdwe	4.51	98	98	90.46	4.51	90.4587446999999969	2019-01-15 07:45:09	afsabcabcd                                                                                          	afsabcabcd
92	zhuoxue%yll	zhuoxue%yll	0.45	1	1	250.49	0.45	250.487400000000008	2010-08-25 01:40:09	kisfe                                                                                               	kisfe     
99	afdaewer	afdaewer	4.32	8	8	10.21	4.32	10.2144999999999992	2012-08-20 01:45:09	nihaore                                                                                             	nihaore   
107	kisfe	kisfe	7.89	16	16	58.12	7.89	58.1244999999999976	2012-08-20 01:45:09	einoejk                                                                                             	einoejk   
119	hellorew	hellorew	5.78	28	28	10.21	5.78	10.2144999999999992	2016-12-15 14:45:09	cdefeed                                                                                             	cdefeed   
129	zhuoxue_yll	zhuoxue_yll	5.34	38	38	90.46	5.34	90.4587446999999969	2019-01-15 07:45:09	word23                                                                                              	word23    
132	hello1234	hello1234	7.34	41	41	250.49	7.34	250.487400000000008	2010-08-25 01:40:09	afdaewer                                                                                            	afdaewer  
135	feed32feed	feed32feed	7.89	44	44	1110.47	7.89	1110.47469999999998	2016-12-15 14:45:09	cdefeed                                                                                             	cdefeed   
142	afsabcabcd	afsabcabcd	4.23	51	51	38.49	4.23	38.4879000000000033	2008-12-08 13:45:00	\N	\N
144	einoejk	einoejk	6.78	53	53	48.15	6.78	48.1477999999999966	2028-12-16 13:45:00	he343243                                                                                            	he343243  
153	word23	word23	4.55	62	62	301.46	4.55	301.456999999999994	2019-01-15 07:45:09	afsabcabcd                                                                                          	afsabcabcd
158	abdfeed	abdfeed	6.78	67	67	2000.23	6.78	2000.23232000000007	2008-12-08 13:45:00	\N	\N
189	hello1234	hello1234	5.78	98	98	90.46	5.78	90.4587446999999969	2020-12-18 13:45:00	safdwe                                                                                              	safdwe    
193	nihaore	nihaore	4.12	2	2	301.46	4.12	301.456999999999994	2019-01-15 07:45:09	word23                                                                                              	word23    
201	einoejk	einoejk	4.51	10	10	35.15	4.51	35.1477999999999966	2019-01-15 07:45:09	afsabcabcd                                                                                          	afsabcabcd
210	word23	word23	4.56	19	19	180.46	4.56	180.457844999999992	2018-08-15 07:45:09	feed32feed                                                                                          	feed32feed
212	nihaore	nihaore	4.23	21	21	250.49	4.23	250.487400000000008	2010-08-25 01:40:09	afdaewer                                                                                            	afdaewer  
215	abdfeed	abdfeed	4.51	24	24	1110.47	4.51	1110.47469999999998	2016-12-15 14:45:09	cdefeed                                                                                             	cdefeed   
226	\N	\N	4.23	35	35	55.15	4.23	55.1477999999999966	2018-08-15 07:45:09	feed32feed                                                                                          	feed32feed
235	cdefeed	cdefeed	4.12	44	44	1110.47	4.12	1110.47469999999998	2012-08-20 01:45:09	einoejk                                                                                             	einoejk   
240	kisfe	kisfe	4.23	49	49	21.26	4.23	21.2579999999999991	2028-12-16 13:45:00	he343243                                                                                            	he343243  
249	feed32feed	feed32feed	4.12	58	58	90.46	4.12	90.4587446999999969	2019-01-15 07:45:09	afsabcabcd                                                                                          	afsabcabcd
256	afsabcabcd	afsabcabcd	6.78	65	65	1414.15	6.78	1414.14747000000011	2028-12-16 13:45:00	he343243                                                                                            	he343243  
276	sfdeiekd	sfdeiekd	2.34	85	85	1414.15	2.34	1414.14747000000011	2010-08-25 01:40:09	afdaewer                                                                                            	afdaewer  
301	zhuoxue%yll	zhuoxue%yll	5.78	10	10	35.15	5.78	35.1477999999999966	2020-12-18 13:45:00	safdwe                                                                                              	safdwe    
304	he343243	he343243	2.34	13	13	48.15	2.34	48.1477999999999966	2028-12-16 13:45:00	he343243                                                                                            	he343243  
322	hello1234	hello1234	4.56	31	31	38.49	4.56	38.4879000000000033	2018-08-15 07:45:09	feed32feed                                                                                          	feed32feed
349	cdefeed	cdefeed	4.55	58	58	90.46	4.55	90.4587446999999969	2020-12-18 13:45:00	safdwe                                                                                              	safdwe    
354	kisfe	kisfe	6.78	63	63	800.15	6.78	800.147000000000048	2018-08-15 07:45:09	feed32feed                                                                                          	feed32feed
399	he343243	he343243	5.78	8	8	10.21	5.78	10.2144999999999992	2016-12-15 14:45:09	hello1234                                                                                           	hello1234 
410	einoejk	einoejk	6.78	19	19	180.46	6.78	180.457844999999992	2018-08-15 07:45:09	sfdeiekd                                                                                            	sfdeiekd  
430	kisfe	kisfe	2.34	39	39	180.46	2.34	180.457844999999992	2008-12-08 13:45:00	\N	\N
440	nihaore	nihaore	7.34	49	49	21.26	7.34	21.2579999999999991	2028-12-16 13:45:00	adaabcwer                                                                                           	adaabcwer 
443	abdfeed	abdfeed	7.89	52	52	40.48	7.89	40.4784500000000023	2012-08-20 01:45:09	einoejk                                                                                             	einoejk   
458	feed32feed	feed32feed	2.34	67	67	2000.23	2.34	2000.23232000000007	2018-08-15 07:45:09	sfdeiekd                                                                                            	sfdeiekd  
465	afsabcabcd	afsabcabcd	5.34	74	74	50.49	5.34	50.4874500000000026	2019-01-15 07:45:09	word23                                                                                              	word23    
485	sfdeiekd	sfdeiekd	7.89	94	94	50.49	7.89	50.4874500000000026	2020-12-18 13:45:00	hellorew                                                                                            	hellorew  
489	zhuoXUE	zhuoXUE	4.55	98	98	90.46	4.55	90.4587446999999969	2019-01-15 07:45:09	afsabcabcd                                                                                          	afsabcabcd
513	he343243	he343243	7.89	22	22	301.46	7.89	301.456999999999994	2019-01-15 07:45:09	word23                                                                                              	word23    
518	hellorew	hellorew	4.56	27	27	2000.23	4.56	2000.23232000000007	2008-12-08 13:45:00	abdfeed                                                                                             	abdfeed   
525	kisfe	kisfe	5.78	34	34	50.49	5.78	50.4874500000000026	2020-12-18 13:45:00	safdwe                                                                                              	safdwe    
551	he343243	he343243	4.51	60	60	200.49	4.51	200.487869999999987	2016-12-15 14:45:09	cdefeed                                                                                             	cdefeed   
562	einoejk	einoejk	4.23	71	71	38.49	4.23	38.4879000000000033	2018-08-15 07:45:09	feed32feed                                                                                          	feed32feed
587	\N	\N	4.55	96	96	58.12	4.55	58.1244999999999976	2012-08-20 01:45:09	einoejk                                                                                             	einoejk   
593	afdaewer	afdaewer	4.51	2	2	301.46	4.51	301.456999999999994	2019-01-15 07:45:09	word23                                                                                              	word23    
594	hellorew	hellorew	7.34	3	3	800.15	7.34	800.147000000000048	2018-08-15 07:45:09	feed32feed                                                                                          	feed32feed
598	afsabcabcd	afsabcabcd	2.34	7	7	2000.23	2.34	2000.23232000000007	2008-12-08 13:45:00	abdfeed                                                                                             	abdfeed   
619	einoejk	einoejk	5.34	28	28	10.21	5.34	10.2144999999999992	2012-08-20 01:45:09	einoejk                                                                                             	einoejk   
623	zhuoxue_yll	zhuoxue_yll	5.78	32	32	40.48	5.78	40.4784500000000023	2016-12-15 14:45:09	hello1234                                                                                           	hello1234 
631	afdaewer	afdaewer	4.32	40	40	200.49	4.32	200.487869999999987	2016-12-15 14:45:09	cdefeed                                                                                             	cdefeed   
633	abdfeed	abdfeed	5.34	42	42	301.46	5.34	301.456999999999994	2019-01-15 07:45:09	afsabcabcd                                                                                          	afsabcabcd
634	cdefeed	cdefeed	6.78	43	43	800.15	6.78	800.147000000000048	2018-08-15 07:45:09	sfdeiekd                                                                                            	sfdeiekd  
636	afsabcabcd	afsabcabcd	7.34	45	45	1414.15	7.34	1414.14747000000011	2010-08-25 01:40:09	kisfe                                                                                               	kisfe     
641	zhuoXUE	zhuoXUE	4.12	50	50	35.15	4.12	35.1477999999999966	2019-01-15 07:45:09	word23                                                                                              	word23    
655	afsabcabcd	afsabcabcd	4.12	64	64	1110.47	4.12	1110.47469999999998	2016-12-15 14:45:09	hello1234                                                                                           	hello1234 
690	abdfeed	abdfeed	6.78	99	99	180.46	6.78	180.457844999999992	2018-08-15 07:45:09	feed32feed                                                                                          	feed32feed
693	afsabcabcd	afsabcabcd	5.78	2	2	301.46	5.78	301.456999999999994	2020-12-18 13:45:00	hellorew                                                                                            	hellorew  
717	zhuoXUE	zhuoXUE	5.34	26	26	1825.47	5.34	1825.47484000000009	2020-12-18 13:45:00	safdwe                                                                                              	safdwe    
726	afdaewer	afdaewer	45.23	35	35	55.15	45.23	55.1477999999999966	2008-12-08 13:45:00	abdfeed                                                                                             	abdfeed   
727	hellorew	hellorew	4.55	36	36	58.12	4.55	58.1244999999999976	2016-12-15 14:45:09	cdefeed                                                                                             	cdefeed   
730	adaabcwer	adaabcwer	4.23	39	39	180.46	4.23	180.457844999999992	2018-08-15 07:45:09	sfdeiekd                                                                                            	sfdeiekd  
736	zhuoXUE	zhuoXUE	0.45	45	45	1414.15	0.45	1414.14747000000011	2028-12-16 13:45:00	he343243                                                                                            	he343243  
745	afdaewer	afdaewer	5.34	54	54	50.49	5.34	50.4874500000000026	2019-01-15 07:45:09	afsabcabcd                                                                                          	afsabcabcd
746	hellorew	hellorew	6.78	55	55	55.15	6.78	55.1477999999999966	2018-08-15 07:45:09	sfdeiekd                                                                                            	sfdeiekd  
749	adaabcwer	adaabcwer	5.78	58	58	90.46	5.78	90.4587446999999969	2020-12-18 13:45:00	safdwe                                                                                              	safdwe    
764	afdaewer	afdaewer	0.45	73	73	48.15	0.45	48.1477999999999966	2010-08-25 01:40:09	kisfe                                                                                               	kisfe     
765	hellorew	hellorew	7.89	74	74	50.49	7.89	50.4874500000000026	2020-12-18 13:45:00	safdwe                                                                                              	safdwe    
772	kisfe	kisfe	4.23	81	81	250.49	4.23	250.487400000000008	2010-08-25 01:40:09	afdaewer                                                                                            	afdaewer  
774	zhuoXUE	zhuoXUE	6.78	83	83	800.15	6.78	800.147000000000048	2008-12-08 13:45:00	abdfeed                                                                                             	abdfeed   
787	adaabcwer	adaabcwer	5.34	96	96	58.12	5.34	58.1244999999999976	2012-08-20 01:45:09	nihaore                                                                                             	nihaore   
789	sfdeiekd	sfdeiekd	4.51	98	98	90.46	4.51	90.4587446999999969	2020-12-18 13:45:00	hellorew                                                                                            	hellorew  
802	afdaewer	afdaewer	6.78	11	11	38.49	6.78	38.4879000000000033	2018-08-15 07:45:09	feed32feed                                                                                          	feed32feed
824	cdefeed	cdefeed	45.23	33	33	48.15	45.23	48.1477999999999966	2028-12-16 13:45:00	adaabcwer                                                                                           	adaabcwer 
844	adaabcwer	adaabcwer	6.78	53	53	48.15	6.78	48.1477999999999966	2010-08-25 01:40:09	kisfe                                                                                               	kisfe     
857	feed32feed	feed32feed	5.34	66	66	1825.47	5.34	1825.47484000000009	2019-01-15 07:45:09	afsabcabcd                                                                                          	afsabcabcd
860	hellorew	hellorew	7.34	69	69	21.26	7.34	21.2579999999999991	2010-08-25 01:40:09	kisfe                                                                                               	kisfe     
873	hello1234	hello1234	4.51	82	82	301.46	4.51	301.456999999999994	2019-01-15 07:45:09	afsabcabcd                                                                                          	afsabcabcd
874	he343243	he343243	7.34	83	83	800.15	7.34	800.147000000000048	2018-08-15 07:45:09	sfdeiekd                                                                                            	sfdeiekd  
875	word23	word23	5.78	84	84	1110.47	5.78	1110.47469999999998	2012-08-20 01:45:09	einoejk                                                                                             	einoejk   
879	hellorew	hellorew	4.12	88	88	10.21	4.12	10.2144999999999992	2016-12-15 14:45:09	hello1234                                                                                           	hello1234 
880	abdfeed	abdfeed	45.23	89	89	21.26	45.23	21.2579999999999991	2028-12-16 13:45:00	he343243                                                                                            	he343243  
888	zhuoXUE	zhuoXUE	7.34	97	97	80.46	7.34	80.457800000000006	2028-12-16 13:45:00	adaabcwer                                                                                           	adaabcwer 
897	afdaewer	afdaewer	4.32	6	6	1825.47	4.32	1825.47484000000009	2019-01-15 07:45:09	word23                                                                                              	word23    
898	hellorew	hellorew	4.23	7	7	2000.23	4.23	2000.23232000000007	2018-08-15 07:45:09	feed32feed                                                                                          	feed32feed
916	afdaewer	afdaewer	7.34	25	25	1414.15	7.34	1414.14747000000011	2010-08-25 01:40:09	afdaewer                                                                                            	afdaewer  
926	zhuoXUE	zhuoXUE	4.23	35	35	55.15	4.23	55.1477999999999966	2008-12-08 13:45:00	\N	\N
929	\N	\N	4.51	38	38	90.46	4.51	90.4587446999999969	2019-01-15 07:45:09	word23                                                                                              	word23    
935	afdaewer	afdaewer	4.12	44	44	1110.47	4.12	1110.47469999999998	2016-12-15 14:45:09	cdefeed                                                                                             	cdefeed   
942	einoejk	einoejk	6.78	51	51	38.49	6.78	38.4879000000000033	2008-12-08 13:45:00	\N	\N
949	hello1234	hello1234	4.12	58	58	90.46	4.12	90.4587446999999969	2020-12-18 13:45:00	hellorew                                                                                            	hellorew  
952	feed32feed	feed32feed	4.56	61	61	250.49	4.56	250.487400000000008	2028-12-16 13:45:00	adaabcwer                                                                                           	adaabcwer 
966	zhuoxue%yll	zhuoxue%yll	4.56	75	75	55.15	4.56	55.1477999999999966	2008-12-08 13:45:00	abdfeed                                                                                             	abdfeed   
983	zhuoXUE	zhuoXUE	5.34	92	92	40.48	5.34	40.4784500000000023	2016-12-15 14:45:09	cdefeed                                                                                             	cdefeed   
988	he343243	he343243	0.45	97	97	80.46	0.45	80.457800000000006	2010-08-25 01:40:09	kisfe                                                                                               	kisfe     
996	adaabcwer	adaabcwer	4.23	5	5	1414.15	4.23	1414.14747000000011	2010-08-25 01:40:09	afdaewer                                                                                            	afdaewer  
998	sfdeiekd	sfdeiekd	6.78	7	7	2000.23	6.78	2000.23232000000007	2008-12-08 13:45:00	abdfeed                                                                                             	abdfeed   
\.

ANALYZE select_hash1_tb_part_1;

ALTER TABLE ONLY select_hash1_tb_part_1
    ADD CONSTRAINT select_hash1_tb_part_1_pkey PRIMARY KEY (pk);

set polar_enable_px = on;
set polar_px_dop_per_node = 1;

---
--- For all these tests Result + HashFilter node is used for the second Window
--- function in execution pipeline
---

:opt_explain
SELECT sum(smallint_test) OVER (PARTITION BY integer_test ) AS sum1,
       sum(integer_test) OVER (PARTITION BY char_size_test ) AS sum2
FROM select_hash1_tb_part_1
WHERE 'David!' LIKE 'David_'
OR 'David_' NOT LIKE 'abc'
:order_by_all

:opt_explain
SELECT sum(smallint_test) OVER (PARTITION BY integer_test ) AS sum1,
       sum(integer_test) OVER (PARTITION BY char_size_test ) AS sum2,
       sum(decimal_test) OVER (PARTITION BY smallint_test ) AS sum3
FROM select_hash1_tb_part_1
WHERE 'David!' LIKE 'David_'
OR 'David_' NOT LIKE 'abc'
:order_by_all

:opt_explain
SELECT sum(smallint_test) OVER (PARTITION BY integer_test ) AS sum1,
       sum(integer_test) OVER (PARTITION BY char_size_test, number_test ) AS sum2
FROM select_hash1_tb_part_1
WHERE 'David!' LIKE 'David_'
OR 'David_' NOT LIKE 'abc'
:order_by_all

--
-- These tests are the same except we do not allow hash filter to be enabled
--

set polar_px_enable_result_hash_filter = 0;

:opt_explain
SELECT sum(smallint_test) OVER (PARTITION BY integer_test ) AS sum1,
       sum(integer_test) OVER (PARTITION BY char_size_test ) AS sum2
FROM select_hash1_tb_part_1
WHERE 'David!' LIKE 'David_'
OR 'David_' NOT LIKE 'abc'
:order_by_all

:opt_explain
SELECT sum(smallint_test) OVER (PARTITION BY integer_test ) AS sum1,
       sum(integer_test) OVER (PARTITION BY char_size_test ) AS sum2,
       sum(decimal_test) OVER (PARTITION BY smallint_test ) AS sum3
FROM select_hash1_tb_part_1
WHERE 'David!' LIKE 'David_'
OR 'David_' NOT LIKE 'abc'
:order_by_all

:opt_explain
SELECT sum(smallint_test) OVER (PARTITION BY integer_test ) AS sum1,
       sum(integer_test) OVER (PARTITION BY char_size_test, number_test ) AS sum2
FROM select_hash1_tb_part_1
WHERE 'David!' LIKE 'David_'
OR 'David_' NOT LIKE 'abc'
:order_by_all


-- start_ignore
reset all;
DROP SCHEMA result_hash_filter CASCADE;
-- end_ignore
