/*------------------------------------------------------------------------------
 * polar_audit.c
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

/* GUC variable for polar_audit.log, which defines the classes to log. */
static char *auditLog = NULL;

/*
 * GUC variable for polar_audit.log_catalog
 *
 * Log queries or not when relations used in the query are in pg_catalog.
 */
static bool auditLogCatalog = true;

/*
 * GUC variable for polar_audit.log_relation
 *
 * Log each relation separately when relation involved in READ/WRITE class queries.
 */
static bool auditLogRelation = false;

/*
 * GUC variable for polar_audit.log_parameter
 *
 * Administrators can choose if parameters passed into a statement are
 * included in the audit log.
 */
static bool auditLogParameter = false;

/*
 * GUC variable for polar_audit.log_statement
 *
 * Administrators can choose to not have the full statement text logged.
 */
static bool auditLogStatement = true;

/*
 * GUC variable for polar_audit.role
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
 * An AuditEvent represents an operation that potentially affects a single
 * object.  If a statement affects multiple objects then multiple AuditEvents
 * are created to represent them.
 */
typedef struct
{
	LogStmtLevel logStmtLevel;	/* From GetCommandLogLevel when possible,
								 * generated when not. */
	NodeTag		commandTag;		/* same here */
	int			command;		/* same here */
	const char *objectType;		/* From event trigger when possible, generated
								 * when not. */
	char	   *objectName;		/* Fully qualified object identification */
	const char *commandText;	/* sourceText / queryString */
	ParamListInfo paramList;	/* QueryDesc/ProcessUtility parameters */

	bool		logged;			/* Track if we have logged this event, used
								 * post-ProcessUtility to make sure we log */
	int64		rows;			/* Track rows processed by the statement */
	MemoryContext queryContext; /* Context for query tracking rows */
	Oid			auditOid;		/* Role running query tracking rows  */
	List	   *rangeTabls;		/* Tables in query tracking rows */
}			AuditEvent;

/*
 * A simple FIFO queue to keep track of the current stack of audit events.
 */
typedef struct AuditEventStackItem
{
	struct AuditEventStackItem *next;

	AuditEvent	auditEvent;

	int64		stackId;

	MemoryContext contextAudit;
	MemoryContextCallback contextCallback;
}			AuditEventStackItem;

static AuditEventStackItem * auditEventStack = NULL;

/*
 * Track running total for statements and substatements and whether or not
 * anything has been logged since the current statement began.
 */
static int64 stackTotal = 0;

/*
 * Respond to callbacks registered with MemoryContextRegisterResetCallback().
 * Removes the event(s) off the stack that have become obsolete once the
 * MemoryContext has been freed.  The callback should always be freeing the top
 * of the stack, but the code is tolerant of out-of-order callbacks.
 */
static void
stack_free(void *stackFree)
{
	AuditEventStackItem *nextItem = auditEventStack;

	/* Only process if the stack contains items */
	while (nextItem != NULL)
	{
		/* Check if this item matches the item to be freed */
		if (nextItem == (AuditEventStackItem *) stackFree)
		{
			/* Move top of stack to the item after the freed item */
			auditEventStack = nextItem->next;

			return;
		}

		nextItem = nextItem->next;
	}
}

/*
 * Push a new audit event onto the stack and create a new memory context to
 * store it.
 */
static AuditEventStackItem *
stack_push()
{
	MemoryContext contextAudit;
	MemoryContext contextOld;
	AuditEventStackItem *stackItem;

	/*
	 * Create a new memory context to contain the stack item.  This will be
	 * free'd on stack_pop, or by our callback when the parent context is
	 * destroyed.
	 */
	contextAudit = AllocSetContextCreate(CurrentMemoryContext,
										 "polar_audit stack context",
										 ALLOCSET_DEFAULT_SIZES);

	/* Save the old context to switch back to at the end */
	contextOld = MemoryContextSwitchTo(contextAudit);

	/* Create our new stack item in our context */
	stackItem = palloc0(sizeof(AuditEventStackItem));
	stackItem->contextAudit = contextAudit;
	stackItem->stackId = ++stackTotal;

	/*
	 * Setup a callback in case an error happens.  stack_free() will truncate
	 * the stack at this item.
	 */
	stackItem->contextCallback.func = stack_free;
	stackItem->contextCallback.arg = (void *) stackItem;
	MemoryContextRegisterResetCallback(contextAudit,
									   &stackItem->contextCallback);

	/* Push new item onto the stack */
	if (auditEventStack != NULL)
		stackItem->next = auditEventStack;
	else
		stackItem->next = NULL;

	auditEventStack = stackItem;

	MemoryContextSwitchTo(contextOld);

	return stackItem;
}

/*
 * Pop an audit event from the stack by deleting the memory context that
 * contains it.  The callback to stack_free() does the actual pop.
 */
static void
stack_pop(int64 stackId)
{
	/* Make sure what we want to delete is at the top of the stack */
	if (auditEventStack != NULL && auditEventStack->stackId == stackId)
		MemoryContextDelete(auditEventStack->contextAudit);
	else
		elog(ERROR, "polar_audit stack item " INT64_FORMAT " not found on top - cannot pop",
			 stackId);
}

/*
 * Hook ExecutorStart to get the query text and basic command type for queries
 * that do not contain a table and so can't be idenitified accurately in
 * ExecutorCheckPerms.
 */
static void
polar_audit_ExecutorStart_hook(QueryDesc *queryDesc, int eflags)
{
	return;
}

/*
 * Hook ExecutorCheckPerms to do session and object auditing for DML.
 */
static bool
polar_audit_ExecutorCheckPerms_hook(List *rangeTabls, bool abort)
{
	return false;
}


/*
 * Hook ProcessUtility to do session auditing for DDL and utility commands.
 */
static void
polar_audit_ProcessUtility_hook(PlannedStmt *pstmt,
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
polar_audit_object_access_hook(ObjectAccessType access,
						   Oid classId,
						   Oid objectId,
						   int subId,
						   void *arg)
{
	/* Temporary code to avoid compile warning. */
	stack_push();
	stack_pop(0);
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
						errmsg("polar_audit must be loaded via shared_preload_libraries")));

	/* Define polar_audit.log */
	DefineCustomStringVariable(
							   "polar_audit.log",

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

	/* Define polar_audit.log_catalog */
	DefineCustomBoolVariable(
							 "polar_audit.log_catalog",

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

	/* Define polar_audit.log_relation */
	DefineCustomBoolVariable(
							 "polar_audit.log_relation",

							 "Specifies whether audit logging should create a separate log entry "
							 "for each relation referenced in a SELECT or DML statement.",

							 NULL,
							 &auditLogRelation,
							 false,
							 PGC_SUSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);

	/* Define polar_audit.log_statement */
	DefineCustomBoolVariable(
							 "polar_audit.log_statement",

							 "Specifies whether logging will include the statement text and "
							 "parameters. Depending on settings, the full statement text might "
							 "not be required in the audit log.",

							 NULL,
							 &auditLogStatement,
							 true,
							 PGC_SUSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);

	/* Define polar_audit.log_parameter */
	DefineCustomBoolVariable(
							 "polar_audit.log_parameter",

							 "Specifies that audit logging should include the parameters that were "
							 "passed into the statement. When parameters are present they will be "
							 "included in CSV format after the statement text.",

							 NULL,
							 &auditLogParameter,
							 false,
							 PGC_SUSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);

	/* Define polar_audit.role */
	DefineCustomStringVariable(
							   "polar_audit.role",

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
	ExecutorStart_hook = polar_audit_ExecutorStart_hook;

	next_ExecutorCheckPerms_hook = ExecutorCheckPerms_hook;
	ExecutorCheckPerms_hook = polar_audit_ExecutorCheckPerms_hook;

	next_ProcessUtility_hook = ProcessUtility_hook;
	ProcessUtility_hook = polar_audit_ProcessUtility_hook;

	next_object_access_hook = object_access_hook;
	object_access_hook = polar_audit_object_access_hook;

#ifndef EXEC_BACKEND
	ereport(LOG, (errmsg("polar_audit extension initialized")));
#else
	ereport(DEBUG1, (errmsg("polar_audit extension initialized")));
#endif							/* EXEC_BACKEND */

	inited = true;
}