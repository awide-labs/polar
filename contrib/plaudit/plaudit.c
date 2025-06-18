/*------------------------------------------------------------------------------
 * plaudit.c
 *
 * An audit logging extension for PostgreSQL. Provides detailed logging classes,
 * and fully-qualified object names for all DML and DDL statements where possible
 * (See README.md for details).
 *
 * Copyright (c) 2024-2025, PostgreSQL Global Development Group
 *------------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/xact.h"
#include "access/relation.h"
#include "catalog/catalog.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_class.h"
#include "catalog/namespace.h"
#include "commands/dbcommands.h"
#include "commands/extension.h"
#include "catalog/pg_proc.h"
#include "commands/event_trigger.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "libpq/auth.h"
#include "nodes/nodes.h"
#include "nodes/params.h"
#include "tcop/utility.h"
#include "tcop/deparse_utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/varlena.h"

PG_MODULE_MAGIC;

void		_PG_init(void);

/* GUC variable for plaudit.log, which defines the classes to log. */
static char *auditLog = NULL;

/*
 * GUC variable for plaudit.log_catalog
 *
 * Log queries or not when relations used in the query are in pg_catalog.
 */
static bool auditLogCatalog = true;

/*
 * GUC variable for plaudit.log_relation
 *
 * Log each relation separately when relation involved in READ/WRITE class queries.
 */
static bool auditLogRelation = false;

/*
 * GUC variable for plaudit.log_statement
 *
 * Administrators can choose to not have the full statement text logged.
 */
static bool auditLogStatement = true;

/*
 * GUC variable for plaudit.role
 *
 * Master role.
 */
static char *auditRole = NULL;

/*
 * Hook functions
 */
static ExecutorCheckPerms_hook_type next_ExecutorCheckPerms_hook = NULL;
static ProcessUtility_hook_type next_ProcessUtility_hook = NULL;
static object_access_hook_type next_object_access_hook = NULL;
static ExecutorStart_hook_type next_ExecutorStart_hook = NULL;

/*
 * Hook ExecutorStart to get the query text and basic command type for queries
 * that do not contain a table and so can't be idenitified accurately in
 * ExecutorCheckPerms.
 */
static void
plaudit_ExecutorStart_hook(QueryDesc *queryDesc, int eflags)
{
	return;
}

/*
 * Hook ExecutorCheckPerms to do session and object auditing for DML.
 */
static bool
plaudit_ExecutorCheckPerms_hook(List *rangeTabls, bool abort)
{
	return false;
}


/*
 * Hook ProcessUtility to do session auditing for DDL and utility commands.
 */
static void
plaudit_ProcessUtility_hook(PlannedStmt *pstmt,
							const char *queryString,
							bool readOnlyTree,
							ProcessUtilityContext context,
							ParamListInfo params,
							QueryEnvironment *queryEnv,
							DestReceiver *dest,
							QueryCompletion *qc)
{
	return;
}

/*
 * Hook object_access_hook to provide fully-qualified object names for function
 * calls.
 */
static void
plaudit_object_access_hook(ObjectAccessType access,
						   Oid classId,
						   Oid objectId,
						   int subId,
						   void *arg)
{
	return;
}

/*
 * Define GUC variables and install hooks upon module load.
 */
void
_PG_init(void)
{
	/* Be sure we do initialization only once */
	static bool inited = false;

	if (inited)
		return;

	/* Must be loaded with shared_preload_libraries */
	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("plaudit must be loaded via shared_preload_libraries")));

	/* Define plaudit.log */
	DefineCustomStringVariable(
							   "plaudit.log",

							   "Specifies which classes of statements will be logged by audit logging. "
							   "Multiple classes can be provided using a comma-separated list and "
							   "classes can be subtracted by prefacing the class with a - sign.",

							   NULL,
							   &auditLog,
							   "none",
							   PGC_SUSET,
							   GUC_LIST_INPUT | GUC_NOT_IN_SAMPLE,
							   NULL,
							   NULL,
							   NULL);

	/* Define plaudit.log_catalog */
	DefineCustomBoolVariable(
							 "plaudit.log_catalog",

							 "Specifies that logging should be enabled in the case where all "
							 "relations in a statement are in pg_catalog. Disabling this setting "
							 "will reduce noise in the log from tools like psql and PgAdmin that "
							 "query the catalog heavily.",

							 NULL,
							 &auditLogCatalog,
							 true,
							 PGC_SUSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);

	/* Define plaudit.log_relation */
	DefineCustomBoolVariable(
							 "plaudit.log_relation",

							 "Specifies whether audit logging should create a separate log entry "
							 "for each relation referenced in a SELECT or DML statement.",

							 NULL,
							 &auditLogRelation,
							 false,
							 PGC_SUSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);

	/* Define plaudit.log_statement */
	DefineCustomBoolVariable(
							 "plaudit.log_statement",

							 "Specifies whether logging will include the statement text and "
							 "parameters. Depending on settings, the full statement text might "
							 "not be required in the audit log.",

							 NULL,
							 &auditLogStatement,
							 true,
							 PGC_SUSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);

	/* Define plaudit.role */
	DefineCustomStringVariable(
							   "plaudit.role",

							   "Specifies the master role to use for audit logging.  Multiple audit "
							   "roles can be defined by granting the master role to them. Only the "
							   "master role and its members have permission to write the audit log.",

							   NULL,
							   &auditRole,
							   "",
							   PGC_SUSET,
							   GUC_NOT_IN_SAMPLE,
							   NULL, NULL, NULL);

	/*
	 * Install our hook functions after saving the existing pointers to
	 * preserve the chains.
	 */
	next_ExecutorStart_hook = ExecutorStart_hook;
	ExecutorStart_hook = plaudit_ExecutorStart_hook;

	next_ExecutorCheckPerms_hook = ExecutorCheckPerms_hook;
	ExecutorCheckPerms_hook = plaudit_ExecutorCheckPerms_hook;

	next_ProcessUtility_hook = ProcessUtility_hook;
	ProcessUtility_hook = plaudit_ProcessUtility_hook;

	next_object_access_hook = object_access_hook;
	object_access_hook = plaudit_object_access_hook;

#ifndef EXEC_BACKEND
	ereport(LOG, (errmsg("plaudit extension initialized")));
#else
	ereport(DEBUG1, (errmsg("plaudit extension initialized")));
#endif							/* EXEC_BACKEND */

	inited = true;
}
