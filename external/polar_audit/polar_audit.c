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

/* Bits within auditLogBitmap */
#define LOG_DDL         (1 << 0)	/* CREATE/DROP/ALTER */
#define LOG_FUNCTION    (1 << 1)	/* Functions and DO blocks */
#define LOG_MISC        (1 << 2)	/* Statements which not covered */
#define LOG_READ        (1 << 3)	/* SELECT */
#define LOG_ROLE        (1 << 4)	/* GRANT/REVOKE, CREATE/DROP/ALTER ROLE */
#define LOG_WRITE       (1 << 5)	/* INSERT, DELETE, UPDATE, TRUNCATE */
#define LOG_MISC_SET    (1 << 6)	/* SET ... */

#define LOG_NONE        0		/* empty */
#define LOG_ALL         (0xFFFFFFFF)	/* All above */

/* String tokens constants for log classes */
#define CLASS_DDL       "DDL"
#define CLASS_FUNCTION  "FUNCTION"
#define CLASS_MISC      "MISC"
#define CLASS_MISC_SET  "MISC_SET"
#define CLASS_READ      "READ"
#define CLASS_ROLE      "ROLE"
#define CLASS_WRITE     "WRITE"

#define CLASS_NONE      "NONE"
#define CLASS_ALL       "ALL"

/*
 * Object type, used for SELECT/DML statements and function calls.
 */
#define OBJECT_TYPE_TABLE           "TABLE"
#define OBJECT_TYPE_INDEX           "INDEX"
#define OBJECT_TYPE_SEQUENCE        "SEQUENCE"
#define OBJECT_TYPE_TOASTVALUE      "TOAST TABLE"
#define OBJECT_TYPE_VIEW            "VIEW"
#define OBJECT_TYPE_MATVIEW         "MATERIALIZED VIEW"
#define OBJECT_TYPE_COMPOSITE_TYPE  "COMPOSITE TYPE"
#define OBJECT_TYPE_FOREIGN_TABLE   "FOREIGN TABLE"
#define OBJECT_TYPE_FUNCTION        "FUNCTION"

#define OBJECT_TYPE_UNKNOWN         "UNKNOWN"

/* GUC variable for polar_audit.log, which defines the classes to log. */
static char *auditLog = NULL;

static int	auditLogBitmap = LOG_NONE;

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
 * Check that an item is on the stack.  If not, an error will be raised since
 * this is a bad state to be in and it might mean audit records are being lost.
 */
static void
stack_valid(int64 stackId)
{
	AuditEventStackItem *nextItem = auditEventStack;

	/* Look through the stack for the stack entry */
	while (nextItem != NULL && nextItem->stackId != stackId)
		nextItem = nextItem->next;

	/* If we didn't find it, something went wrong. */
	if (nextItem == NULL)
		elog(ERROR, "plaudit stack item " INT64_FORMAT
			 " not found - top of stack is " INT64_FORMAT "",
			 stackId,
			 auditEventStack == NULL ? (int64) -1 : auditEventStack->stackId);
}

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
 * Gets an AuditEvent, classifies it, then logs it if appropriate.
 */
static void
log_audit_event(AuditEventStackItem * stackItem)
{
	/* avoid warning */
	return;
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
 * Create AuditEvents for SELECT/DML operations via executor permissions checks.
 */
static void
log_select_dml(Oid auditOid, List *rangeTabls)
{
	ListCell   *lr;
	bool		first = true;
	bool		found = false;

	foreach(lr, rangeTabls)
	{
		Oid			relOid;
		Oid			relNamespaceOid;
		RangeTblEntry *rte = lfirst(lr);

		/*
		 * We only care about tables, and can ignore subqueries etc. Also
		 * detect and skip partitions by checking for missing requiredPerms.
		 */
		if (rte->rtekind != RTE_RELATION || rte->requiredPerms == 0)
			continue;

		found = true;

		/*
		 * Don't log if the session user is not a member of the current role.
		 * This prevents contents of security definer functions from being
		 * logged and suppresses foreign key queries unless the session user
		 * is the owner of the referenced table.
		 */
		if (!is_member_of_role(GetSessionUserId(), GetUserId()))
			return;

		/*
		 * If we are not logging all-catalog queries (auditLogCatalog is
		 * false) then filter out any system relations here.
		 */
		relOid = rte->relid;
		relNamespaceOid = get_rel_namespace(relOid);

		if (!auditLogCatalog && IsCatalogNamespace(relNamespaceOid))
			continue;

		/*
		 * If this is the first RTE then session log unless auditLogRelation
		 * is set.
		 */
		if (first && !auditLogRelation)
		{
			log_audit_event(auditEventStack);

			first = false;
		}

		/*
		 * We don't have access to the parse tree here, so we have to generate
		 * the node type, object type, and command tag by decoding
		 * rte->requiredPerms and rte->relkind. For updates we also check
		 * rellockmode so that only true UPDATE commands (not SELECT FOR
		 * UPDATE, etc.) are logged as UPDATE.
		 */
		if (rte->requiredPerms & ACL_INSERT)
		{
			auditEventStack->auditEvent.logStmtLevel = LOGSTMT_MOD;
			auditEventStack->auditEvent.commandTag = T_InsertStmt;
			auditEventStack->auditEvent.command = CMDTAG_INSERT;
		}
		else if (rte->requiredPerms & ACL_UPDATE &&
				 rte->rellockmode >= RowExclusiveLock)
		{
			auditEventStack->auditEvent.logStmtLevel = LOGSTMT_MOD;
			auditEventStack->auditEvent.commandTag = T_UpdateStmt;
			auditEventStack->auditEvent.command = CMDTAG_UPDATE;
		}
		else if (rte->requiredPerms & ACL_DELETE)
		{
			auditEventStack->auditEvent.logStmtLevel = LOGSTMT_MOD;
			auditEventStack->auditEvent.commandTag = T_DeleteStmt;
			auditEventStack->auditEvent.command = CMDTAG_DELETE;
		}
		else if (rte->requiredPerms & ACL_SELECT)
		{
			auditEventStack->auditEvent.logStmtLevel = LOGSTMT_ALL;
			auditEventStack->auditEvent.commandTag = T_SelectStmt;
			auditEventStack->auditEvent.command = CMDTAG_SELECT;
		}
		else
		{
			auditEventStack->auditEvent.logStmtLevel = LOGSTMT_ALL;
			auditEventStack->auditEvent.commandTag = T_Invalid;
			auditEventStack->auditEvent.command = CMDTAG_UNKNOWN;
		}

		/* Use the relation type to assign object type */
		switch (rte->relkind)
		{
			case RELKIND_RELATION:
			case RELKIND_PARTITIONED_TABLE:
				auditEventStack->auditEvent.objectType = OBJECT_TYPE_TABLE;
				break;

			case RELKIND_INDEX:
			case RELKIND_PARTITIONED_INDEX:
				auditEventStack->auditEvent.objectType = OBJECT_TYPE_INDEX;
				break;

			case RELKIND_SEQUENCE:
				auditEventStack->auditEvent.objectType = OBJECT_TYPE_SEQUENCE;
				break;

			case RELKIND_TOASTVALUE:
				auditEventStack->auditEvent.objectType = OBJECT_TYPE_TOASTVALUE;
				break;

			case RELKIND_VIEW:
				auditEventStack->auditEvent.objectType = OBJECT_TYPE_VIEW;
				break;

			case RELKIND_COMPOSITE_TYPE:
				auditEventStack->auditEvent.objectType = OBJECT_TYPE_COMPOSITE_TYPE;
				break;

			case RELKIND_FOREIGN_TABLE:
				auditEventStack->auditEvent.objectType = OBJECT_TYPE_FOREIGN_TABLE;
				break;

			case RELKIND_MATVIEW:
				auditEventStack->auditEvent.objectType = OBJECT_TYPE_MATVIEW;
				break;

			default:
				auditEventStack->auditEvent.objectType = OBJECT_TYPE_UNKNOWN;
				break;
		}

		/* Get a copy of the relation name and assign it to object name */
		auditEventStack->auditEvent.objectName =
			quote_qualified_identifier(
									   get_namespace_name(relNamespaceOid), get_rel_name(relOid));

		/* Do relation level logging if auditLogRelation is set */
		if (auditLogRelation)
		{
			auditEventStack->auditEvent.logged = false;
			log_audit_event(auditEventStack);
		}

		pfree(auditEventStack->auditEvent.objectName);
	}

	/*
	 * If no tables were found that means that RangeTbls was empty or all
	 * relations were in the system schema.  In that case still log a record.
	 */
	if (!found)
	{
		auditEventStack->auditEvent.logged = false;

		log_audit_event(auditEventStack);
	}
}

/*
 * Hook ExecutorCheckPerms to do auditing for DML.
 */
static bool
polar_audit_ExecutorCheckPerms_hook(List *rangeTabls, bool abort)
{
	Oid			auditOid;

	/* Get the audit oid if the role exists */
	auditOid = get_role_oid(auditRole, true);
	auditEventStack->auditEvent.auditOid = auditOid;

	/* Log DML if the audit role is valid or logging is enabled */
	if ((auditOid != InvalidOid || auditLogBitmap != 0) &&
		!IsAbortedTransactionBlockState())
		log_select_dml(auditOid, rangeTabls);

	/* Call the next hook function */
	if (next_ExecutorCheckPerms_hook &&
		!(*next_ExecutorCheckPerms_hook) (rangeTabls, abort))
		return false;

	return true;
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
	int64		stackId = 0;
	AuditEventStackItem *stackItem = NULL;

	/*
	 * Don't audit substatements.  All the substatements we care about should
	 * be covered by the event triggers.
	 */
	if (context <= PROCESS_UTILITY_QUERY && !IsAbortedTransactionBlockState())
	{
		/* Process top level utility statement */
		if (context == PROCESS_UTILITY_TOPLEVEL)
		{
			/*
			 * If the stack is not empty then the only allowed entries are
			 * call statements or open, select, show, and explain cursors
			 */
			if (auditEventStack != NULL)
			{
				AuditEventStackItem *nextItem = auditEventStack;

				do
				{
					if (nextItem->auditEvent.commandTag != T_SelectStmt &&
						nextItem->auditEvent.commandTag != T_VariableShowStmt &&
						nextItem->auditEvent.commandTag != T_ExplainStmt &&
						nextItem->auditEvent.commandTag != T_CallStmt)
					{
						elog(ERROR, "plaudit stack is not empty");
					}

					nextItem = nextItem->next;
				}
				while (nextItem != NULL);
			}

			stackItem = stack_push();
			stackItem->auditEvent.paramList = copyParamList(params);
		}
		else
			stackItem = stack_push();

		stackId = stackItem->stackId;
		stackItem->auditEvent.logStmtLevel = GetCommandLogLevel(pstmt->utilityStmt);
		stackItem->auditEvent.commandTag = nodeTag(pstmt->utilityStmt);
		stackItem->auditEvent.command = CreateCommandTag(pstmt->utilityStmt);
		stackItem->auditEvent.commandText = queryString;
		stackItem->auditEvent.auditOid = get_role_oid(auditRole, true);

		/*
		 * If this is a DO block log it before calling the next ProcessUtility
		 * hook.
		 */
		if (auditLogBitmap & LOG_FUNCTION &&
			stackItem->auditEvent.commandTag == T_DoStmt &&
			!IsAbortedTransactionBlockState())
			log_audit_event(stackItem);

		/*
		 * If this is a create/alter extension command log it before calling
		 * the next ProcessUtility hook. Otherwise, any warnings will be
		 * emitted before the create/alter is logged and errors will prevent
		 * it from being logged at all.
		 */
		if (auditLogBitmap & LOG_DDL &&
			(stackItem->auditEvent.commandTag == T_CreateExtensionStmt ||
			 stackItem->auditEvent.commandTag == T_AlterExtensionStmt) &&
			!IsAbortedTransactionBlockState())
			log_audit_event(stackItem);

		/*
		 * A close will free the open cursor which will also free the close
		 * audit entry. Immediately log the close and set stackItem to NULL so
		 * it won't be logged later.
		 */
		if (stackItem->auditEvent.commandTag == T_ClosePortalStmt)
		{
			if (auditLogBitmap & LOG_MISC && !IsAbortedTransactionBlockState())
				log_audit_event(stackItem);

			stackItem = NULL;
		}
	}

	/* Call the standard process utility chain. */
	if (next_ProcessUtility_hook)
		(*next_ProcessUtility_hook) (pstmt, queryString, readOnlyTree, context,
									 params, queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);

	/*
	 * Process the audit event if there is one.  Also check that this event
	 * was not popped off the stack by a memory context being free'd
	 * elsewhere.
	 */
	if (stackItem && !IsAbortedTransactionBlockState())
	{
		/*
		 * Make sure the item we want to log is still on the stack - if not
		 * then something has gone wrong and an error will be raised.
		 */
		stack_valid(stackId);

		/*
		 * Log the utility command if logging is on, the command has not
		 * already been logged by another hook, and the transaction is not
		 * aborted.
		 */
		if (auditLogBitmap != 0 && !stackItem->auditEvent.logged)
			log_audit_event(stackItem);
	}
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
 * GUC check functions.
 * Take a polar_audit.log value such as "read, write, dml", verify that each of the
 * comma-separated tokens corresponds to a LogClass value, and convert them into
 * a bitmap that log_audit_event can check.
 */
static bool
check_polar_audit_log(char **newVal, void **extra, GucSource source)
{
	List	   *flagRawList;
	char	   *rawVal;
	ListCell   *lt;
	int		   *flags;

	rawVal = pstrdup(*newVal);
	if (!SplitIdentifierString(rawVal, ',', &flagRawList))
	{
		GUC_check_errdetail("List syntax is invalid");
		list_free(flagRawList);
		pfree(rawVal);
		return false;
	}

	if (!(flags = (int *) malloc(sizeof(int))))
		return false;

	*flags = 0;

	foreach(lt, flagRawList)
	{
		char	   *token = (char *) lfirst(lt);
		bool		subtract = false;
		int			class;

		/* If token is preceded by -, then the token is subtractive */
		if (token[0] == '-')
		{
			token++;
			subtract = true;
		}

		/* Test each token */
		if (pg_strcasecmp(token, CLASS_NONE) == 0)
			class = LOG_NONE;
		else if (pg_strcasecmp(token, CLASS_ALL) == 0)
			class = LOG_ALL;
		else if (pg_strcasecmp(token, CLASS_DDL) == 0)
			class = LOG_DDL;
		else if (pg_strcasecmp(token, CLASS_FUNCTION) == 0)
			class = LOG_FUNCTION;
		else if (pg_strcasecmp(token, CLASS_MISC) == 0)
			class = LOG_MISC | LOG_MISC_SET;
		else if (pg_strcasecmp(token, CLASS_MISC_SET) == 0)
			class = LOG_MISC_SET;
		else if (pg_strcasecmp(token, CLASS_READ) == 0)
			class = LOG_READ;
		else if (pg_strcasecmp(token, CLASS_ROLE) == 0)
			class = LOG_ROLE;
		else if (pg_strcasecmp(token, CLASS_WRITE) == 0)
			class = LOG_WRITE;
		else
		{
			free(flags);
			pfree(rawVal);
			list_free(flagRawList);
			return false;
		}

		/* Add or subtract class bits from the bitmap */
		if (subtract)
			*flags &= ~class;
		else
			*flags |= class;
	}

	pfree(rawVal);
	list_free(flagRawList);

	*extra = flags;

	return true;
}

/*
 * GUC assign functions.
 */
static void
assign_polar_audit_log(const char *newVal, void *extra)
{
	if (extra)
		auditLogBitmap = *(int *) extra;
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
							   check_polar_audit_log,
							   assign_polar_audit_log,
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