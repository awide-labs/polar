/*-------------------------------------------------------------------------
 *
 * faultinjector.c
 *
 * Fault injectors are used for fine control during testing. They allow a
 * developer to create deterministic tests for scenarios that are hard to
 * reproduce. This is done by programming actions at certain key areas to
 * suspend, skip, or even panic the process. Fault injectors are set in shared
 * memory so they are accessible to all segment processes.
 *
 * Portions Copyright (c) 2009-2010 Greenplum Inc
 * Portions Copyright (c) 2012-Present VMware, Inc. or its affiliates.
 *
 * IDENTIFICATION
 *		src/backend/utils/misc/faultinjector.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <signal.h>
#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#endif
#include "access/xact.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "postmaster/bgwriter.h"
#include "storage/spin.h"
#include "storage/shmem.h"
#include "utils/faultinjector.h"
#include "utils/hsearch.h"
#include "common/hashfn.h"
#include "miscadmin.h"


/*
 * gettext() can't be used in a static initializer... This breaks nls builds.
 * So, to work around this issue, I've made _() be a no-op.
 */
#undef _
#define _(x) x

#define FAULT_INJECTOR_LOG(elevel, format, args...) \
	InjectorLOG(elevel, __FILE__, __LINE__, format, ##args)

typedef struct FaultInjectorShmem_s
{
	slock_t		lock;
	int			faultInjectorSlots;
	HTAB	   *hash;
}			FaultInjectorShmem_s;

static FaultInjectorShmem_s * faultInjectorShmem = NULL;

static void FiLockAcquire(void);
static void FiLockRelease(void);

static FaultInjectorEntry_s * FaultInjector_LookupHashEntry(const char *faultName);

static FaultInjectorEntry_s * FaultInjector_InsertHashEntry(const char *faultName,
															bool *exists);

static int	FaultInjector_NewHashEntry(FaultInjectorEntry_s * entry);

static int	FaultInjector_MarkEntryAsResume(FaultInjectorEntry_s * entry);

static bool FaultInjector_RemoveHashEntry(const char *faultName);

static int	FaultInjector_SetFaultInjection(FaultInjectorEntry_s * entry);

static FaultInjectorType_e FaultInjectorTypeStringToEnum(const char *faultType);

static void
			InjectorLOG(int elevel, const char *filename, int lineno, const char *fmt,...)
			__attribute__((format(printf, 4, 5)));

/* Arrays to map between enum values and strings */
const char *FaultInjectorTypeEnumToString[] = {
#define FI_TYPE(id, str) str,
#include "utils/faultinjector_lists.h"
#undef FI_TYPE
};

const char *FaultInjectorStateEnumToString[] = {
#define FI_STATE(id, str) str,
#include "utils/faultinjector_lists.h"
#undef FI_STATE
};

static FaultInjectorType_e
FaultInjectorTypeStringToEnum(const char *faultTypeString)
{
	FaultInjectorType_e faultTypeEnum = FaultInjectorTypeMax;
	int			ii;

	for (ii = FaultInjectorTypeNotSpecified + 1; ii < FaultInjectorTypeMax; ii++)
	{
		if (strcmp(FaultInjectorTypeEnumToString[ii], faultTypeString) == 0)
		{
			faultTypeEnum = ii;
			break;
		}
	}
	return faultTypeEnum;
}

static void
FiLockAcquire(void)
{
	SpinLockAcquire(&faultInjectorShmem->lock);
}

static void
FiLockRelease(void)
{
	SpinLockRelease(&faultInjectorShmem->lock);
}

static void
InjectorLOG(int elevel, const char *filename, int lineno, const char *fmt,...)
{
#define FORMATTED_TS_LEN 128
	va_list		args;
	pg_time_t	stamp_time;
	struct timeval time_val;
	char		formatted_log_time[FORMATTED_TS_LEN];
	char		msbuf[13];
	FILE	   *outfile = elevel > ERROR ? stderr : stdout;

	gettimeofday(&time_val, NULL);
	stamp_time = (pg_time_t) time_val.tv_sec;

	pg_strftime(formatted_log_time, FORMATTED_TS_LEN,
				"%Y-%m-%d %H:%M:%S     %Z",
				pg_localtime(&stamp_time, log_timezone));
	sprintf(msbuf, ".%03d", (int) (time_val.tv_usec / 1000));
	memcpy(formatted_log_time + 19, msbuf, 4);
	fprintf(outfile, "%s", formatted_log_time);

	if (elevel >= FATAL)
		fprintf(outfile, "FATAL ");
	else if (elevel >= ERROR)
		fprintf(outfile, "ERROR ");
	else if (elevel >= WARNING)
		fprintf(outfile, "WARNING ");
	else if (elevel >= LOG)
		fprintf(outfile, "LOG ");
	else if (elevel == DEBUG1)
		fprintf(outfile, "DEBUG ");
	else if (elevel < DEBUG1 && elevel >= DEBUG5)
		fprintf(outfile, "DEBUG ");
	else
		fprintf(outfile, "WARNING ");

	fprintf(outfile, "Fault injector at %s:%d ", __FILE__, __LINE__);

	va_start(args, fmt);
	(void) vfprintf(outfile, fmt, args);
	va_end(args);
}

/****************************************************************
 * FAULT INJECTOR routines
 ****************************************************************/
Size
FaultInjector_ShmemSize(void)
{
	Size		size;

	size = hash_estimate_size(
							  (Size) FAULTINJECTOR_MAX_SLOTS,
							  sizeof(FaultInjectorEntry_s));

	size = add_size(size, sizeof(FaultInjectorShmem_s));

	return size;
}

/*
 * Hash table contains fault injection that are set on the system waiting to be injected.
 * FaultInjector identifier is the key in the hash table.
 * Hash table in shared memory is initialized only on primary and mirror segment.
 * It is not initialized on master host.
 */
void
FaultInjector_ShmemInit(void)
{
	HASHCTL		hash_ctl;
	bool		foundPtr;

	faultInjectorShmem = (FaultInjectorShmem_s *) ShmemInitStruct("fault injector",
																  sizeof(FaultInjectorShmem_s),
																  &foundPtr);

	if (faultInjectorShmem == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 (errmsg("not enough shared memory for fault injector"))));

	if (!foundPtr)
		MemSet(faultInjectorShmem, 0, sizeof(FaultInjectorShmem_s));

	SpinLockInit(&faultInjectorShmem->lock);

	faultInjectorShmem->faultInjectorSlots = 0;

	MemSet(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = FAULT_NAME_MAX_LENGTH;
	hash_ctl.entrysize = sizeof(FaultInjectorEntry_s);
	hash_ctl.hash = string_hash;

	faultInjectorShmem->hash = ShmemInitHash("fault injector hash",
											 FAULTINJECTOR_MAX_SLOTS,
											 FAULTINJECTOR_MAX_SLOTS,
											 &hash_ctl,
											 HASH_ELEM | HASH_FUNCTION);

	if (faultInjectorShmem->hash == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 (errmsg("not enough shared memory for fault injector"))));

	elog(LOG, "initialized faultinjector shmem");
	return;
}

FaultInjectorType_e
FaultInjector_TriggerFaultIfSet(const char *faultName,
								const char *databaseName,
								const char *tableName)
{

	FaultInjectorEntry_s *entryShared,
				localEntry,
			   *entryLocal = &localEntry;
	char		databaseNameLocal[NAMEDATALEN];
	char		tableNameLocal[NAMEDATALEN];
	int			ii = 0;
	int			cnt = 3600;

	if (strlen(faultName) >= FAULT_NAME_MAX_LENGTH)
	{
		FAULT_INJECTOR_LOG(ERROR, "fault name too long: '%s'", faultName);
		return FaultInjectorTypeNotSpecified;
	}
	if (strcmp(faultName, FaultInjectorNameAll) == 0)
	{
		FAULT_INJECTOR_LOG(ERROR, "Fault injector error: invalid fault name '%s'", faultName);
		return FaultInjectorTypeNotSpecified;
	}
	if (strlen(databaseName) >= NAMEDATALEN)
	{
		FAULT_INJECTOR_LOG(ERROR, "database name too long:'%s'", databaseName);
		return FaultInjectorTypeNotSpecified;
	}
	if (strlen(tableName) >= NAMEDATALEN)
	{
		FAULT_INJECTOR_LOG(ERROR, "table name too long: '%s'", tableName);
		return FaultInjectorTypeNotSpecified;
	}

	/*
	 * Return immediately if no fault has been injected ever.  It is important
	 * to not touch the spinlock, especially if this is the postmaster
	 * process.  If one of the backend processes dies while holding the spin
	 * lock, and postmaster comes here before resetting the shared memory, it
	 * waits without holder process and eventually goes into PANIC.  Also this
	 * saves a few cycles to acquire the spin lock and look into the shared
	 * hash table.
	 *
	 * Although this is a race condition without lock, a false negative is ok
	 * given this framework is purely for dev/testing.
	 */
	if (faultInjectorShmem->faultInjectorSlots == 0)
		return FaultInjectorTypeNotSpecified;

	snprintf(databaseNameLocal, sizeof(databaseNameLocal), "%s", databaseName);
	snprintf(tableNameLocal, sizeof(tableNameLocal), "%s", tableName);

	entryLocal->faultInjectorType = FaultInjectorTypeNotSpecified;

	FiLockAcquire();

	entryShared = FaultInjector_LookupHashEntry(faultName);

	do
	{
		if (entryShared == NULL)
			/* fault injection is not set */
			break;

		if (strcmp(entryShared->databaseName, databaseNameLocal) != 0)
			/* fault injection is not set for the specified database name */
			break;

		if (strcmp(entryShared->tableName, tableNameLocal) != 0)
			/* fault injection is not set for the specified table name */
			break;

		if (entryShared->faultInjectorState == FaultInjectorStateCompleted ||
			entryShared->faultInjectorState == FaultInjectorStateFailed)
		{
			/* fault injection was already executed */
			break;
		}

		entryShared->numTimesTriggered++;

		if (entryShared->numTimesTriggered < entryShared->startOccurrence)
		{
			break;
		}

		/* Update the injection fault entry in hash table */
		entryShared->faultInjectorState = FaultInjectorStateTriggered;

		/* Mark fault injector to completed */
		if (entryShared->endOccurrence != INFINITE_END_OCCURRENCE &&
			entryShared->numTimesTriggered >= entryShared->endOccurrence)
			entryShared->faultInjectorState = FaultInjectorStateCompleted;

		memcpy(entryLocal, entryShared, sizeof(FaultInjectorEntry_s));
	} while (0);

	FiLockRelease();

	/* Inject fault */
	switch (entryLocal->faultInjectorType)
	{
		case FaultInjectorTypeNotSpecified:
		case FaultInjectorTypeEnable:
			break;

		case FaultInjectorTypeSleep:
			/* Sleep for the specified amount of time. */

			FAULT_INJECTOR_LOG(LOG,
							   "fault triggered, fault name:'%s' fault type:'%s' ",
							   entryLocal->faultName,
							   FaultInjectorTypeEnumToString[entryLocal->faultInjectorType]);
			pg_usleep(entryLocal->extraArg * 1000000L);
			break;

		case FaultInjectorTypeFatal:
			ereport(FATAL,
					(errmsg("fault triggered, fault name:'%s' fault type:'%s' ",
							entryLocal->faultName,
							FaultInjectorTypeEnumToString[entryLocal->faultInjectorType])));
			break;

		case FaultInjectorTypePanic:

			/*
			 * Avoid core file generation for this PANIC. It helps to avoid
			 * filling up disks during tests and also saves time.
			 */
#if defined(HAVE_GETRLIMIT) && defined(RLIMIT_CORE)
			{
				struct rlimit lim;

				getrlimit(RLIMIT_CORE, &lim);
				lim.rlim_cur = 0;
				if (setrlimit(RLIMIT_CORE, &lim) != 0)
					elog(NOTICE,
						 "setrlimit failed for RLIMIT_CORE soft limit to zero (%m)");
			}
#endif
			ereport(PANIC,
					(errmsg("fault triggered, fault name:'%s' fault type:'%s' ",
							entryLocal->faultName,
							FaultInjectorTypeEnumToString[entryLocal->faultInjectorType])));
			break;

		case FaultInjectorTypeError:
			ereport(ERROR,
					(errmsg("fault triggered, fault name:'%s' fault type:'%s' ",
							entryLocal->faultName,
							FaultInjectorTypeEnumToString[entryLocal->faultInjectorType])));
			break;

		case FaultInjectorTypeInfiniteLoop:
			/* Loop until the fault is reset or an interrupt occurs. */
			ereport(LOG,
					(errmsg("fault triggered, fault name:'%s' fault type:'%s' ",
							entryLocal->faultName,
							FaultInjectorTypeEnumToString[entryLocal->faultInjectorType])));

			FiLockAcquire();
			for (ii = 0;
				 ii < cnt && FaultInjector_LookupHashEntry(entryLocal->faultName);
				 ii++)
			{
				FiLockRelease();
				pg_usleep(1000000L);
				CHECK_FOR_INTERRUPTS();
				FiLockAcquire();
			}
			FiLockRelease();
			break;

		case FaultInjectorTypeSuspend:
			{
				/* Suspend until the fault is resumed or reset */
				FaultInjectorEntry_s *entry;

				FAULT_INJECTOR_LOG(LOG,
								   "fault triggered, fault name:'%s' fault type:'%s' ",
								   entryLocal->faultName,
								   FaultInjectorTypeEnumToString[entryLocal->faultInjectorType]);

				FiLockAcquire();
				while ((entry = FaultInjector_LookupHashEntry(entryLocal->faultName)) != NULL &&
					   entry->faultInjectorType != FaultInjectorTypeResume)
				{
					FiLockRelease();
					pg_usleep(1000000L);
					/* 1 sec */
					FiLockAcquire();
				}
				FiLockRelease();

				if (entry != NULL)
				{
					FAULT_INJECTOR_LOG(LOG,
									   "fault triggered, fault name:'%s' fault type:'%s' ",
									   entryLocal->faultName,
									   FaultInjectorTypeEnumToString[entry->faultInjectorType]);
				}
				else
				{
					FAULT_INJECTOR_LOG(LOG,
									   "fault name:'%s' removed", entryLocal->faultName);

					/*
					 * Since the entry is gone already, we should NOT update
					 * the entry below.  (There could be other places in this
					 * function that are under the same situation, but I'm too
					 * tired to look for them...)
					 */
					return entryLocal->faultInjectorType;
				}
				break;
			}

		case FaultInjectorTypeSkip:
			/* Do nothing.  The caller is expected to take some action. */
			FAULT_INJECTOR_LOG(LOG,
							   "fault triggered, fault name:'%s' fault type:'%s' ",
							   entryLocal->faultName,
							   FaultInjectorTypeEnumToString[entryLocal->faultInjectorType]);
			break;

		case FaultInjectorTypeResume:

			/*
			 * This fault is resumed after suspension but has not been reset
			 * yet.  Ignore.
			 */
			break;

		case FaultInjectorTypeSegv:
			{
				/*
				 * Avoid core file generation for this PANIC. It helps to
				 * avoid filling up disks during tests and also saves time.
				 */
#if defined(HAVE_GETRLIMIT) && defined(RLIMIT_CORE)
				struct rlimit lim;

				getrlimit(RLIMIT_CORE, &lim);
				lim.rlim_cur = 0;
				if (setrlimit(RLIMIT_CORE, &lim) != 0)
					elog(NOTICE,
						 "setrlimit failed for RLIMIT_CORE soft limit to zero (%m)");
#endif

				*(volatile int *) 0 = 1234;
				break;
			}

		case FaultInjectorTypeInterrupt:

			/*
			 * XXX: check if the following comment is valid.
			 *
			 * The place where this type of fault is injected must have has
			 * HOLD_INTERRUPTS() .. RESUME_INTERRUPTS() around it, otherwise
			 * the interrupt could be handled inside the fault injector itself
			 */
			ereport(LOG,
					(errmsg("fault triggered, fault name:'%s' fault type:'%s' ",
							entryLocal->faultName,
							FaultInjectorTypeEnumToString[entryLocal->faultInjectorType])));
			InterruptPending = true;
			QueryCancelPending = true;
			break;

		default:
			ereport(ERROR,
					(errmsg("invalid fault type %d, fault name:'%s'",
							entryLocal->faultInjectorType, entryLocal->faultName)));
			break;
	}
	return (entryLocal->faultInjectorType);
}

/*
 * lookup if fault injection is set
 */
static FaultInjectorEntry_s *
FaultInjector_LookupHashEntry(const char *faultName)
{
	FaultInjectorEntry_s *entry;

	Assert(faultInjectorShmem->hash != NULL);
	entry = (FaultInjectorEntry_s *) hash_search(
												 faultInjectorShmem->hash,
												 (void *) faultName, //key
												 HASH_FIND,
												 NULL);

	if (entry == NULL)
	{
		ereport(DEBUG5,
				(errmsg("FaultInjector_LookupHashEntry() could not find fault injection hash entry:'%s' ",
						faultName)));
	}

	return entry;
}

/*
 * insert fault injection in hash table
 */
static FaultInjectorEntry_s *
FaultInjector_InsertHashEntry(const char *faultName,
							  bool *exists)
{

	bool		foundPtr;
	FaultInjectorEntry_s *entry;

	Assert(faultInjectorShmem->hash != NULL);
	entry = (FaultInjectorEntry_s *) hash_search(
												 faultInjectorShmem->hash,
												 (void *) faultName, //key
												 HASH_ENTER_NULL,
												 &foundPtr);

	if (entry == NULL)
	{
		*exists = false;
		return entry;
	}

	elog(DEBUG1, "FaultInjector_InsertHashEntry() entry_key:%s",
		 entry->faultName);

	if (foundPtr)
	{
		*exists = true;
	}
	else
	{
		*exists = false;
	}

	return entry;
}

static bool
FaultInjector_RemoveHashEntry(const char *faultName)
{

	FaultInjectorEntry_s *entry;
	bool		isRemoved = false;

	Assert(faultInjectorShmem->hash != NULL);
	entry = (FaultInjectorEntry_s *) hash_search(
												 faultInjectorShmem->hash,
												 (void *) faultName, //key
												 HASH_REMOVE,
												 NULL);

	if (entry)
	{
		ereport(LOG,
				(errmsg("fault removed, fault name:'%s' fault type:'%s' ",
						entry->faultName,
						FaultInjectorTypeEnumToString[entry->faultInjectorType])));

		isRemoved = true;
	}

	return isRemoved;
}

static int
FaultInjector_NewHashEntry(FaultInjectorEntry_s * entry)
{

	FaultInjectorEntry_s *entryLocal = NULL;
	bool		exists;
	int			status = STATUS_OK;

	FiLockAcquire();

	if ((faultInjectorShmem->faultInjectorSlots + 1) >= FAULTINJECTOR_MAX_SLOTS)
	{
		FiLockRelease();
		status = STATUS_ERROR;
		ereport(WARNING,
				(errmsg("cannot insert fault injection, no slots available"),
				 errdetail("Fault name:'%s' fault type:'%s'",
						   entry->faultName,
						   FaultInjectorTypeEnumToString[entry->faultInjectorType])));
		snprintf(entry->bufOutput, sizeof(entry->bufOutput),
				 "could not insert fault injection, max slots:'%d' reached",
				 FAULTINJECTOR_MAX_SLOTS);

		goto exit;
	}

	entryLocal = FaultInjector_InsertHashEntry(entry->faultName, &exists);

	if (entryLocal == NULL)
	{
		FiLockRelease();
		status = STATUS_ERROR;
		ereport(WARNING,
				(errmsg("cannot insert fault injection entry into table, no memory"),
				 errdetail("Fault name:'%s' fault type:'%s'",
						   entry->faultName,
						   FaultInjectorTypeEnumToString[entry->faultInjectorType])));
		snprintf(entry->bufOutput, sizeof(entry->bufOutput),
				 "could not insert fault injection, no memory");

		goto exit;
	}

	if (exists)
	{
		FiLockRelease();
		status = STATUS_ERROR;
		ereport(WARNING,
				(errmsg("cannot insert fault injection entry into table, entry already exists"),
				 errdetail("Fault name:'%s' fault type:'%s' ",
						   entry->faultName,
						   FaultInjectorTypeEnumToString[entry->faultInjectorType])));
		snprintf(entry->bufOutput, sizeof(entry->bufOutput),
				 "could not insert fault injection, entry already exists");

		goto exit;
	}

	entryLocal->faultInjectorType = entry->faultInjectorType;
	strlcpy(entryLocal->faultName, entry->faultName, sizeof(entryLocal->faultName));

	entryLocal->extraArg = entry->extraArg;

	entryLocal->startOccurrence = entry->startOccurrence;
	entryLocal->endOccurrence = entry->endOccurrence;

	entryLocal->numTimesTriggered = 0;
	strcpy(entryLocal->databaseName, entry->databaseName);
	strcpy(entryLocal->tableName, entry->tableName);

	entryLocal->faultInjectorState = FaultInjectorStateWaiting;

	faultInjectorShmem->faultInjectorSlots++;

	FiLockRelease();

	elog(DEBUG1, "FaultInjector_NewHashEntry(): '%s'", entry->faultName);

exit:

	return status;
}

/*
 * update hash entry with state
 */
static int
FaultInjector_MarkEntryAsResume(FaultInjectorEntry_s * entry)
{

	FaultInjectorEntry_s *entryLocal;
	int			status = STATUS_OK;

	Assert(entry->faultInjectorType == FaultInjectorTypeResume);

	FiLockAcquire();

	entryLocal = FaultInjector_LookupHashEntry(entry->faultName);

	if (entryLocal == NULL)
	{
		FiLockRelease();
		status = STATUS_ERROR;
		ereport(WARNING,
				(errmsg("cannot update fault injection hash entry with fault injection status, no entry found"),
				 errdetail("Fault name:'%s' fault type:'%s'",
						   entry->faultName,
						   FaultInjectorTypeEnumToString[entry->faultInjectorType])));
		goto exit;
	}

	if (entryLocal->faultInjectorType != FaultInjectorTypeSuspend)
		ereport(ERROR, (errmsg("only suspend fault can be resumed")));

	entryLocal->faultInjectorType = FaultInjectorTypeResume;

	FiLockRelease();

	ereport(DEBUG1,
			(errmsg("LOG(fault injector): update fault injection hash entry identifier:'%s' state:'%s'",
					entry->faultName,
					FaultInjectorStateEnumToString[entryLocal->faultInjectorState])));

exit:

	return status;
}

/*
 * Inject fault according to its type.
 */
static int
FaultInjector_SetFaultInjection(FaultInjectorEntry_s * entry)
{
	int			status = STATUS_OK;
	bool		isRemoved = false;

	switch (entry->faultInjectorType)
	{
		case FaultInjectorTypeReset:
			{
				HASH_SEQ_STATUS hash_status;
				FaultInjectorEntry_s *entryLocal;

				if (strcmp(entry->faultName, FaultInjectorNameAll) == 0)
				{
					hash_seq_init(&hash_status, faultInjectorShmem->hash);

					FiLockAcquire();

					while ((entryLocal = (FaultInjectorEntry_s *) hash_seq_search(&hash_status)) != NULL)
					{
						isRemoved = FaultInjector_RemoveHashEntry(entryLocal->faultName);
						if (isRemoved == true)
						{
							faultInjectorShmem->faultInjectorSlots--;
						}
					}
					FiLockRelease();
					Assert(faultInjectorShmem->faultInjectorSlots == 0);
				}
				else
				{
					FiLockAcquire();
					isRemoved = FaultInjector_RemoveHashEntry(entry->faultName);
					if (isRemoved == true)
					{
						faultInjectorShmem->faultInjectorSlots--;
					}
					FiLockRelease();
				}

				if (isRemoved == false)
					ereport(DEBUG1,
							(errmsg("LOG(fault injector): could not remove fault injection from hash identifier:'%s'",
									entry->faultName)));

				break;
			}

		case FaultInjectorTypeWaitUntilTriggered:
			{
				FaultInjectorEntry_s *entryLocal;
				int			retry_count = 600;	/* 10 minutes */

				FiLockAcquire();
				while ((entryLocal = FaultInjector_LookupHashEntry(entry->faultName)) != NULL &&
					   entryLocal->faultInjectorState != FaultInjectorStateCompleted &&
					   entryLocal->numTimesTriggered - entryLocal->startOccurrence < entry->extraArg - 1)
				{
					FiLockRelease();
					pg_usleep(1000000L);
					/* 1 sec */
					retry_count--;
					if (!retry_count)
					{
						ereport(ERROR,
								(errmsg("fault not triggered, fault name:'%s' fault type:'%s' ",
										entryLocal->faultName,
										FaultInjectorTypeEnumToString[entry->faultInjectorType]),
								 errdetail("Timed-out as 10 minutes max wait happens until triggered.")));
					}
					FiLockAcquire();
				}
				FiLockRelease();

				if (entryLocal != NULL)
				{
					ereport(LOG,
							(errmsg("fault triggered %d times, fault name:'%s' fault type:'%s' ",
									entryLocal->numTimesTriggered,
									entryLocal->faultName,
									FaultInjectorTypeEnumToString[entry->faultInjectorType])));
					status = STATUS_OK;
				}
				else
				{
					ereport(ERROR,
							(errmsg("fault not set, fault name:'%s'  ",
									entryLocal->faultName)));
				}
				break;
			}

		case FaultInjectorTypeStatus:
			{
				FaultInjectorEntry_s *entryLocal;
				int			length;

				if (faultInjectorShmem->hash == NULL)
				{
					status = STATUS_ERROR;
					break;
				}
				length = snprintf(entry->bufOutput, sizeof(entry->bufOutput), "Success: ");


				entryLocal = FaultInjector_LookupHashEntry(entry->faultName);
				if (entryLocal)
				{
					length = snprintf(
									  (entry->bufOutput + length),
									  sizeof(entry->bufOutput) - length,
									  "fault name:'%s' "
									  "fault type:'%s' "
									  "database name:'%s' "
									  "table name:'%s' "
									  "start occurrence:'%d' "
									  "end occurrence:'%d' "
									  "extra arg:'%d' "
									  "fault injection state:'%s' "
									  "num times hit:'%d' \n",
									  entryLocal->faultName,
									  FaultInjectorTypeEnumToString[entryLocal->faultInjectorType],
									  entryLocal->databaseName,
									  entryLocal->tableName,
									  entryLocal->startOccurrence,
									  entryLocal->endOccurrence,
									  entryLocal->extraArg,
									  FaultInjectorStateEnumToString[entryLocal->faultInjectorState],
									  entryLocal->numTimesTriggered);
				}
				else
				{
					length = snprintf(entry->bufOutput, sizeof(entry->bufOutput),
									  "Failure: fault name:'%s' not set",
									  entry->faultName);

				}
				elog(LOG, "%s", entry->bufOutput);
				if (length > sizeof(entry->bufOutput))
					elog(LOG, "fault status truncated from %d to %lu characters",
						 length, sizeof(entry->bufOutput));
				break;
			}
		case FaultInjectorTypeResume:
			{
				ereport(LOG,
						(errmsg("fault triggered, fault name:'%s' fault type:'%s' ",
								entry->faultName,
								FaultInjectorTypeEnumToString[entry->faultInjectorType])));

				FaultInjector_MarkEntryAsResume(entry);

				break;
			}
		default:

			status = FaultInjector_NewHashEntry(entry);
			break;
	}
	return status;
}

char *
InjectFault(char *faultName, char *type, char *databaseName, char *tableName,
			int startOccurrence, int endOccurrence, int extraArg)
{
	StringInfo	buf = makeStringInfo();
	FaultInjectorEntry_s faultEntry;

	elog(DEBUG1, "injecting fault: name %s, type %s, db %s, table %s, startOccurrence %d, endOccurrence %d, extraArg %d",
		 faultName, type, databaseName, tableName,
		 startOccurrence, endOccurrence, extraArg);

	if (strlcpy(faultEntry.faultName, faultName, sizeof(faultEntry.faultName)) >=
		sizeof(faultEntry.faultName))
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("fault name too long: '%s'", faultName),
				 errdetail("Fault name should be no more than %d characters.",
						   FAULT_NAME_MAX_LENGTH - 1)));

	faultEntry.faultInjectorType = FaultInjectorTypeStringToEnum(type);
	if (faultEntry.faultInjectorType == FaultInjectorTypeMax)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("could not recognize fault type '%s'", type)));

	/* Special fault name "all" is only used to reset all faults */
	if (faultEntry.faultInjectorType != FaultInjectorTypeReset &&
		strcmp(faultEntry.faultName, FaultInjectorNameAll) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid fault name '%s'", faultName)));

	faultEntry.extraArg = extraArg;
	if (faultEntry.faultInjectorType == FaultInjectorTypeSleep)
	{
		if (extraArg < 0 || extraArg > 7200)
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("invalid sleep time, allowed range [0, 7200 sec]")));
	}

	if (strlcpy(faultEntry.databaseName, databaseName,
				sizeof(faultEntry.databaseName)) >=
		sizeof(faultEntry.databaseName))
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("database name too long: '%s'", databaseName),
				 errdetail("Database name should be no more than %d characters.",
						   NAMEDATALEN - 1)));

	if (strlcpy(faultEntry.tableName, tableName, sizeof(faultEntry.tableName)) >=
		sizeof(faultEntry.tableName))
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("table name too long: '%s'", tableName),
				 errdetail("Table name should be no more than %d characters.",
						   NAMEDATALEN - 1)));

	if (startOccurrence < 1 || startOccurrence > 1000)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid start occurrence number, allowed range [1, 1000]")));


	if (endOccurrence != INFINITE_END_OCCURRENCE && endOccurrence < startOccurrence)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid end occurrence number, allowed range [startOccurrence, ] or -1")));

	faultEntry.startOccurrence = startOccurrence;
	faultEntry.endOccurrence = endOccurrence;

	if (FaultInjector_SetFaultInjection(&faultEntry) == STATUS_OK)
	{
		if (faultEntry.faultInjectorType == FaultInjectorTypeStatus)
			appendStringInfo(buf, "%s", faultEntry.bufOutput);
		else
		{
			appendStringInfo(buf, "Success:");
			elog(LOG, "injected fault '%s' type '%s'", faultName, type);
		}
	}
	else
		appendStringInfo(buf, "Failure: %s", faultEntry.bufOutput);

	return buf->data;
}
