#include "postgres.h"
#include "fmgr.h"

#include "nodes/pg_list.h"
#include "nodes/parsenodes.h"
#include "tcop/tcopprot.h"
#include "optimizer/px_opt.h"
#include "utils/builtins.h"
#include "utils/guc.h"

/* Refer from  px_optimizer_util*/
extern char *SerializeDXLPlan(Query *query);

void		polar_px_gpopt_sql_run(const char *sql);

char	   *polar_px_gpopt_sql_to_dxl(const char *sql);

void
polar_px_gpopt_sql_run(const char *query_string)
{
	List	   *parsetree_list;
	ListCell   *parsetree_item;

	parsetree_list = pg_parse_query(query_string);
	foreach(parsetree_item, parsetree_list)
	{
		List	   *querytree_list;
		ListCell   *query_list;
		RawStmt    *parsetree = lfirst_node(RawStmt, parsetree_item);

		querytree_list = pg_analyze_and_rewrite_fixedparams(
															parsetree, query_string, NULL, 0, NULL);
		foreach(query_list, querytree_list)
		{
			Query	   *query = lfirst_node(Query, query_list);

			if (query->commandType != CMD_UTILITY)
			{
				px_optimize_query(query, NULL);
			}
		}
	}
}

char *
polar_px_gpopt_sql_to_dxl(const char *query_string)
{
	char	   *dxl;
	List	   *parsetree_list;
	ListCell   *parsetree_item;

	dxl = NULL;
	parsetree_list = pg_parse_query(query_string);
	foreach(parsetree_item, parsetree_list)
	{
		List	   *querytree_list;
		ListCell   *query_list;
		RawStmt    *parsetree = lfirst_node(RawStmt, parsetree_item);

		querytree_list = pg_analyze_and_rewrite_fixedparams(
															parsetree, query_string, NULL, 0, NULL);
		foreach(query_list, querytree_list)
		{
			Query	   *query = lfirst_node(Query, query_list);

			if (query->commandType != CMD_UTILITY)
			{
				dxl = SerializeDXLPlan(query);
			}
		}
	}
	return dxl;
}

/* dry run a sql*/
PG_FUNCTION_INFO_V1(test_px_gpopt_sql);
Datum
test_px_gpopt_sql(PG_FUNCTION_ARGS)
{
	char	   *sql = PG_GETARG_CSTRING(0);

	if (!sql)
		PG_RETURN_VOID();
	polar_px_gpopt_sql_run(sql);
	PG_RETURN_VOID();
}

/* turn a sql into dxl*/
PG_FUNCTION_INFO_V1(test_px_gpopt_sql_to_dxl);
Datum
test_px_gpopt_sql_to_dxl(PG_FUNCTION_ARGS)
{
	char	   *dxl;
	char	   *sql = PG_GETARG_CSTRING(0);

	if (!sql)
		PG_RETURN_NULL();
	dxl = polar_px_gpopt_sql_to_dxl(sql);
	PG_RETURN_TEXT_P(cstring_to_text(dxl));
}

/* enum all config param*/
PG_FUNCTION_INFO_V1(test_px_gpopt_config_param);
Datum
test_px_gpopt_config_param(PG_FUNCTION_ARGS)
{
	char	   *sql = PG_GETARG_CSTRING(0);

	if (sql)
	{
		int			i;
		int			enable_count;
		bool	   *lastValues;
		bool	   *options[] = {
			&px_optimizer_enable_indexjoin,
			&px_optimizer_enable_bitmapscan,
			&px_optimizer_enable_outerjoin_to_unionall_rewrite,
			&px_optimizer_enable_assert_maxonerow,
			&px_optimizer_enable_hashjoin,
			&px_optimizer_enable_dynamictablescan,
			&px_optimizer_enable_dynamicindexscan,
			&px_optimizer_enable_seqscan,
			&px_optimizer_enable_seqsharescan,
			&px_optimizer_enable_indexscan,
			&px_optimizer_enable_shareindexscan,
			&px_optimizer_enable_indexonlyscan,
			&px_optimizer_enable_hashagg,
			&px_optimizer_enable_groupagg,
			&px_optimizer_enable_mergejoin,
			&px_optimizer_enable_nestloopjoin,
			&px_optimizer_enable_lasj_notin,
			&px_optimizer_enable_associativity
		};

		/* stash param and open all */
		enable_count = sizeof(options) / sizeof(bool *);
		lastValues = palloc(enable_count * sizeof(bool));
		for (i = 0; i < enable_count; i++)
		{
			lastValues[i] = *options[i];
			*options[i] = false;
		}
		{
			int			last_px_optimizer_join_order = px_optimizer_join_order;

			px_optimizer_join_order = JOIN_ORDER_IN_QUERY;
			polar_px_gpopt_sql_run(sql);
			px_optimizer_join_order = JOIN_ORDER_GREEDY_SEARCH;
			polar_px_gpopt_sql_run(sql);
			px_optimizer_join_order = JOIN_ORDER_EXHAUSTIVE_SEARCH;
			polar_px_gpopt_sql_run(sql);
			px_optimizer_join_order = JOIN_ORDER_EXHAUSTIVE2_SEARCH;
			polar_px_gpopt_sql_run(sql);
			px_optimizer_join_order = last_px_optimizer_join_order;
		}
		/* reset param */
		for (i = 0; i < enable_count; i++)
		{
			*options[i] = lastValues[i];
		}
	}
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(test_px_gpopt_cost_module);
Datum
test_px_gpopt_cost_module(PG_FUNCTION_ARGS)
{
	char	   *sql = PG_GETARG_CSTRING(0);

	if (sql)
	{
		int			last_cost_model;

		last_cost_model = px_optimizer_cost_model;
		px_optimizer_cost_model = OPTIMIZER_GPDB_CALIBRATED;
		polar_px_gpopt_sql_run(sql);
		px_optimizer_cost_model = OPTIMIZER_POLARDB;
		polar_px_gpopt_sql_run(sql);
		px_optimizer_cost_model = last_cost_model;
	}
	PG_RETURN_VOID();
}
