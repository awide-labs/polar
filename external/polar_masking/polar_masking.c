/*-------------------------------------------------------------------------
*
* polar_masking.c
*		Process data masking for query after rewrite.
*
* IDENTIFICATION
*    external/polar_masking/polar_masking.c
*
*-------------------------------------------------------------------------
*/

#include "postgres.h"
#include "polar_masking.h"

#include "rewrite/rewriteHandler.h"
#include "utils/guc.h"

PG_MODULE_MAGIC;

void		_PG_init(void);
void		_PG_fini(void);

bool		polar_masking_enabled = false;

static polar_post_rewrite_query_hook_type next_polar_post_rewrite_query_hook = NULL;
static void polar_process_masking_after_rewrite(List *query_list);

static void
polar_process_masking_after_rewrite(List *query_list)
{

	if (!polar_masking_enabled || query_list == NIL)
	{
		return;
	}

	if (next_polar_post_rewrite_query_hook)
	{
		next_polar_post_rewrite_query_hook(query_list);
	}
}

/*
 * Module load function
 */
void
_PG_init(void)
{
	next_polar_post_rewrite_query_hook = polar_post_rewrite_query_hook;
	polar_post_rewrite_query_hook = polar_process_masking_after_rewrite;

	DefineCustomBoolVariable(
							 "polar_masking.polar_masking_enabled",
							 "if polar_masking is enabled",
							 NULL,
							 &polar_masking_enabled,
							 false,
							 PGC_SIGHUP,
							 POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE,
							 NULL, NULL, NULL);
}

/*
 * Module unload function
 */
void
_PG_fini(void)
{
	/* uninstall hook */
	polar_post_rewrite_query_hook = next_polar_post_rewrite_query_hook;

}
