/*-------------------------------------------------------------------------
 *
 * polar_login_history.c
 *
 * IDENTIFICATION
 *    external/polar_login_history/polar_login_history.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/xact.h"
#include "commands/user.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "replication/walsender.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/acl.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

PG_MODULE_MAGIC;


#define POLAR_LOGIN_HISTORY_FILE "polar_login_history"

/* GUC variable */
static bool polar_login_history_enable;
static int	polar_login_history_maxnum;

/**************************************************************************************************/

#define POLAR_LOGIN_HISTORY_STRING_LENGTH 256

#define POLAR_LOGIN_HISTORY_VALID 1

typedef struct polar_login_history_info
{
	Oid			useroid;		/* user oid */
	TimestampTz logintime;		/* last login time */
	char		ip[POLAR_LOGIN_HISTORY_STRING_LENGTH];	/* last login ip */
	char		application[POLAR_LOGIN_HISTORY_STRING_LENGTH]; /* the application used
																 * for the last login */
	uint8		failcount;		/* the number of failed login attempts since
								 * the last successful login */
}			polar_login_history_info;

typedef struct polar_login_history_array_struct
{
	slock_t		spinlock;
	uint8		state;			/* identifies whether the element is valid */
	int			freenext;		/* if in the free list, points to the index of
								 * the next free element */
	polar_login_history_info logininfo; /* login information of the user */
}			polar_login_history_array_struct;

/* Use a fixed-length array to record the login information of all users */
static polar_login_history_array_struct * polar_login_history_array = NULL;

/**************************************************************************************************/

#define POLAR_LOGIN_HISTORY_LOCK_NUM (polar_login_history_maxnum / 64 * 2)

typedef struct polar_login_history_lock_struct
{
	LWLock	   *lwlock;
}			polar_login_history_lock_struct;

/* The hash table is partitioned, and different areas are protected with different locks */
static polar_login_history_lock_struct * polar_login_history_lock = NULL;

typedef struct polar_login_history_hash_entry
{
	Oid			useroid;
	int			index;			/* index in the array */
}			polar_login_history_hash_entry;

/* The mapping between user oids and array subscripts is maintained through a hash table */
static HTAB *polar_login_history_hash = NULL;

/**************************************************************************************************/

#define POLAR_LOGIN_HISTORY_FREENEXT_END_OF_LIST (-1)
#define POLAR_LOGIN_HISTORY_FREENEXT_NOT_IN_LIST (-2)

typedef struct polar_login_history_freelist_struct
{
	slock_t		spinlock;
	int			firstfree;		/* head of the free list */
	int			lastfree;		/* tail of the free list */
}			polar_login_history_freelist_struct;

/* A list of free array elements that can be used directly when generating new user login information */
static polar_login_history_freelist_struct * polar_login_history_freelist = NULL;

/**************************************************************************************************/

typedef struct polar_login_history_callback_arg
{
	TransactionId xid;			/* current transaction id */
	List	   *useroid;		/* a list of users to be dropped */
}			polar_login_history_callback_arg;

typedef struct polar_login_history_xact_callback_item
{
	struct polar_login_history_xact_callback_item *next;
	XactCallback callback;
	void	   *arg;
}			polar_login_history_xact_callback_item;

/*
 * Records the function that requires a callback when a transaction is committed,
 * mainly to clean up the login information of the dropped user.
 */
static polar_login_history_xact_callback_item * polar_login_history_xact_callback = NULL;

/**************************************************************************************************/

static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static shmem_request_hook_type prev_shmem_request_hook = NULL;

void		_PG_init(void);


/*
 * Estimate shared memory space needed.
 */
static Size
polar_login_history_memsize(void)
{
	Size		size;

	size = MAXALIGN(mul_size(POLAR_LOGIN_HISTORY_LOCK_NUM, sizeof(polar_login_history_lock_struct)));
	size = add_size(size, hash_estimate_size(polar_login_history_maxnum, sizeof(polar_login_history_hash_entry)));
	size = add_size(size, MAXALIGN(mul_size(polar_login_history_maxnum, sizeof(polar_login_history_array_struct))));
	size = add_size(size, MAXALIGN(sizeof(polar_login_history_freelist_struct)));

	return size;
}

/*
 * shmem_request hook: request additional shared resources.  We'll allocate or
 * attach to the shared resources in polar_login_history_shmem_startup().
 */
static void
polar_login_history_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	if (!polar_login_history_enable)
		return;

	RequestAddinShmemSpace(polar_login_history_memsize());
	RequestNamedLWLockTranche("polar_login_history", POLAR_LOGIN_HISTORY_LOCK_NUM);
}

static void
polar_save_information(void)
{
	FILE	   *file = NULL;

	/* Safety check ... shouldn't get here unless shmem is set up */
	if (!polar_login_history_hash || !polar_login_history_array)
		return;

	file = AllocateFile(POLAR_LOGIN_HISTORY_FILE ".tmp", PG_BINARY_W);
	if (file == NULL)
		goto error;

	/* Serialize to disk */
	for (int i = 0; i < polar_login_history_maxnum; i++)
	{
		polar_login_history_array_struct *elem = &polar_login_history_array[i];

		if (elem->state & POLAR_LOGIN_HISTORY_VALID)
		{
			if (fwrite(&elem->logininfo, sizeof(polar_login_history_info), 1, file) != 1)
				goto error;
		}
	}

	FreeFile(file);
	/* Rename file into place, so we atomically replace any old one */
	(void) durable_rename(POLAR_LOGIN_HISTORY_FILE ".tmp", POLAR_LOGIN_HISTORY_FILE, LOG);
	return;

error:
	ereport(WARNING,
			(errcode_for_file_access(),
			 errmsg("could not write file \"%s\": %m",
					POLAR_LOGIN_HISTORY_FILE ".tmp")));
	if (file)
		FreeFile(file);
	unlink(POLAR_LOGIN_HISTORY_FILE ".tmp");
}

static void
polar_load_information(void)
{
	FILE	   *file = NULL;
	int			num = 0;

	/* Attempt to load old statistics from the dump file */
	file = AllocateFile(POLAR_LOGIN_HISTORY_FILE, PG_BINARY_R);
	if (file == NULL)
	{
		if (errno != ENOENT)
			goto read_error;
		return;
	}

	/* Check whether the file is empty */
	fseek(file, 0, SEEK_END);
	if (ftell(file) == 0)
	{
		FreeFile(file);
		unlink(POLAR_LOGIN_HISTORY_FILE);
		return;
	}
	rewind(file);

	do
	{
		polar_login_history_info info;
		polar_login_history_hash_entry *entry;

		if (num >= polar_login_history_maxnum)
		{
			/*
			 * The login information saved in the file exceeds the allocated
			 * shared memory size, so the user needs to reallocate it. Setting
			 * NULL prevents files from being updated when the database is
			 * stopped.
			 */
			FreeFile(file);
			polar_login_history_hash = NULL;
			polar_login_history_array = NULL;
			ereport(FATAL, (errmsg("Out of memory, please adjust polar_login_history.maxnum")));
		}

		if (fread(&info, sizeof(polar_login_history_info), 1, file) != 1)
			goto read_error;

		polar_login_history_freelist->firstfree = polar_login_history_array[num].freenext;

		polar_login_history_array[num].state = POLAR_LOGIN_HISTORY_VALID;
		polar_login_history_array[num].freenext = POLAR_LOGIN_HISTORY_FREENEXT_NOT_IN_LIST;
		polar_login_history_array[num].logininfo = info;

		/* Create an entry with desired hash code */
		entry = (polar_login_history_hash_entry *)
			hash_search(polar_login_history_hash, &info.useroid, HASH_ENTER, NULL);
		entry->useroid = info.useroid;
		entry->index = num;

		num++;
	} while (!feof(file));

	FreeFile(file);

	/*
	 * Remove the persisted stats file so it's not included in
	 * backups/replication standbys, etc
	 */
	unlink(POLAR_LOGIN_HISTORY_FILE);
	return;

read_error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not read file \"%s\": %m",
					POLAR_LOGIN_HISTORY_FILE)));
	if (file)
		FreeFile(file);
	/* If possible, throw away the bogus file; ignore any error */
	unlink(POLAR_LOGIN_HISTORY_FILE);
}

/*
 * shmem_shutdown hook: Dump login information into file.
 *
 * Note: we don't bother with acquiring lock, because there should be no
 * other processes running when this is called.
 */
static void
polar_login_history_shmem_shutdown(int code, Datum arg)
{
	/* Don't try to dump during a crash. */
	if (code)
		return;

	polar_save_information();
}

/*
 * shmem_startup hook: allocate or attach to shared memory,
 * then load any pre-existing login information from file.
 */
static void
polar_login_history_shmem_startup(void)
{
	bool		found_lock;
	bool		found_array;
	bool		found_freelist;
	HASHCTL		info;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	if (!polar_login_history_enable)
		return;

	/*
	 * Create or attach to the shared memory state, including hash table.
	 */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	polar_login_history_lock = (polar_login_history_lock_struct *)
		ShmemInitStruct("polar_login_history_lock",
						POLAR_LOGIN_HISTORY_LOCK_NUM * sizeof(polar_login_history_lock_struct),
						&found_lock);
	polar_login_history_array = (polar_login_history_array_struct *)
		ShmemInitStruct("polar_login_history_array",
						polar_login_history_maxnum * sizeof(polar_login_history_array_struct),
						&found_array);
	polar_login_history_freelist = (polar_login_history_freelist_struct *)
		ShmemInitStruct("polar_login_history_freelist",
						sizeof(polar_login_history_freelist_struct),
						&found_freelist);

	if (found_lock || found_array || found_freelist)
	{
		/* should find all of these, or none of them */
		if (!(found_lock && found_array && found_freelist))
		{
			polar_login_history_lock = NULL;
			polar_login_history_array = NULL;
			polar_login_history_freelist = NULL;
			ereport(WARNING, (errmsg("The login history function fails to allocate shared memory")));
			return;
		}
	}
	else
	{
		/* First time through ... */
		LWLockPadded *lwlock = GetNamedLWLockTranche("polar_login_history");

		for (int i = 0; i < POLAR_LOGIN_HISTORY_LOCK_NUM; i++)
			polar_login_history_lock[i].lwlock = &(lwlock[i].lock);

		for (int i = 0; i < polar_login_history_maxnum; i++)
		{
			SpinLockInit(&polar_login_history_array[i].spinlock);
			polar_login_history_array[i].state = 0;
			/* Initially link all array elements together as unused */
			polar_login_history_array[i].freenext = i + 1;
		}
		/* Correct last entry of linked list */
		polar_login_history_array[polar_login_history_maxnum - 1].freenext = POLAR_LOGIN_HISTORY_FREENEXT_END_OF_LIST;

		SpinLockInit(&polar_login_history_freelist->spinlock);
		polar_login_history_freelist->firstfree = 0;
		polar_login_history_freelist->lastfree = polar_login_history_maxnum - 1;
	}

	info.keysize = sizeof(Oid);
	info.entrysize = sizeof(polar_login_history_hash_entry);
	info.num_partitions = POLAR_LOGIN_HISTORY_LOCK_NUM;
	polar_login_history_hash = ShmemInitHash("polar_login_history_hash",
											 polar_login_history_maxnum,
											 polar_login_history_maxnum,
											 &info,
											 HASH_ELEM | HASH_BLOBS | HASH_PARTITION);

	LWLockRelease(AddinShmemInitLock);

	/*
	 * If we're in the postmaster (or a standalone backend...), set up a shmem
	 * exit hook to dump the statistics to disk.
	 */
	if (!IsUnderPostmaster)
		on_shmem_exit(polar_login_history_shmem_shutdown, (Datum) 0);

	/*
	 * Done if some other process already completed our initialization.
	 */
	if (found_lock && found_array && found_freelist)
		return;

	/*
	 * Note: we don't bother with locks here, because there should be no other
	 * processes running when this code is reached.
	 */
	polar_load_information();
}

static bool
polar_login_history_check(void)
{
	if (!polar_login_history_enable
		|| !polar_login_history_lock
		|| !polar_login_history_hash
		|| !polar_login_history_array)
		return false;

	return true;
}

/*
 * Show the user the last login information and update the current login information.
 */
static void
polar_login_history_show_info(polar_login_history_info * info, Oid useroid, bool login_success, bool first_login)
{
	/* Print login information */
	if (login_success && !first_login)
		ereport(INFO,
				(errmsg("\n" \
						"Last login: %s from %s using %s\n" \
						"The number of failures since the last successful login is %d",
						timestamptz_to_str(info->logintime),
						info->ip,
						info->application,
						info->failcount)));

	/* Update login information */
	info->useroid = useroid;
	info->logintime = MyStartTimestamp ? MyStartTimestamp : GetCurrentTimestamp();
	strncpy(info->ip,
			MyProcPort->remote_host ? MyProcPort->remote_host : "unrecognized",
			POLAR_LOGIN_HISTORY_STRING_LENGTH - 1);
	info->ip[POLAR_LOGIN_HISTORY_STRING_LENGTH - 1] = '\0';
	strncpy(info->application,
			MyProcPort->application_name ? MyProcPort->application_name : "unrecognized",
			POLAR_LOGIN_HISTORY_STRING_LENGTH - 1);
	info->application[POLAR_LOGIN_HISTORY_STRING_LENGTH - 1] = '\0';
	info->failcount = login_success ? 0 : (first_login ? 1 : info->failcount + 1);

	/* Print login information */
	if (login_success && first_login)
		ereport(INFO,
				(errmsg("\n" \
						"First login: %s from %s using %s",
						timestamptz_to_str(info->logintime),
						info->ip,
						info->application)));
}

/*
 * Gets an array element from the free list.
 */
static int
polar_login_history_get_element(void)
{
	polar_login_history_array_struct *elem;
	int			result = -1;

	SpinLockAcquire(&polar_login_history_freelist->spinlock);

	/* There is no element in the free list */
	if (polar_login_history_freelist->firstfree < 0)
	{
		SpinLockRelease(&polar_login_history_freelist->spinlock);
		ereport(WARNING,
				(errmsg("The login information of the user cannot be recorded. " \
						"Please adjust polar_login_history.maxnum to increase the memory space.")));
		return result;
	}

	result = polar_login_history_freelist->firstfree;

	elem = &polar_login_history_array[polar_login_history_freelist->firstfree];
	polar_login_history_freelist->firstfree = elem->freenext;
	elem->freenext = POLAR_LOGIN_HISTORY_FREENEXT_NOT_IN_LIST;

	SpinLockRelease(&polar_login_history_freelist->spinlock);
	return result;
}

/*
 * An array element is invalid, add it to the free list header.
 */
static void
polar_login_history_free_element(int id)
{
	polar_login_history_array_struct *elem = &polar_login_history_array[id];

	SpinLockAcquire(&polar_login_history_freelist->spinlock);

	/*
	 * It is possible that we are told to put something in the freelist that
	 * is already in it; don't screw up the list if so.
	 */
	if (elem->freenext == POLAR_LOGIN_HISTORY_FREENEXT_NOT_IN_LIST)
	{
		elem->freenext = polar_login_history_freelist->firstfree;
		if (elem->freenext < 0)
			polar_login_history_freelist->lastfree = id;
		polar_login_history_freelist->firstfree = id;
	}

	SpinLockRelease(&polar_login_history_freelist->spinlock);
}

/*
 * When a user logs in to the database, the login information is recorded regardless
 * of whether the login succeeds or fails. However, only after a successful login,
 * the database will show the user the last login information.
 */
static void
polar_update_login_history(bool login_success)
{
	bool		intrans;
	Oid			useroid;
	uint32		hashcode;
	LWLock	   *partitionlock;
	polar_login_history_hash_entry *entry;
	bool		found;
	polar_login_history_array_struct *elem;
	bool		first_login = false;

	if (!polar_login_history_check())
		return;

	/* Only the postgres process calls this function during login */
	if (am_walsender || !polar_login_flag)
		return;

	/*
	 * To avoid repeated records of login information, that is, to ensure that
	 * the function is called only once during login, set the flag to false
	 * immediately to mark the end of the login.
	 */
	polar_login_flag = false;

	if (!MyProcPort || !MyProcPort->user_name)
		return;

	/* User information needs to be queried in a transaction */
	intrans = IsTransactionState();
	if (intrans)
		useroid = get_role_oid(MyProcPort->user_name, true);
	else
	{
		StartTransactionCommand();
		useroid = get_role_oid(MyProcPort->user_name, true);
	}
	/* If the user does not exist, return instead of reporting an error */
	if (!OidIsValid(useroid))
		return;

	hashcode = get_hash_value(polar_login_history_hash, &useroid);
	partitionlock = polar_login_history_lock[hashcode % POLAR_LOGIN_HISTORY_LOCK_NUM].lwlock;

	/* Check whether the hash table contains information about the user */
	LWLockAcquire(partitionlock, LW_SHARED);
	entry = (polar_login_history_hash_entry *)
		hash_search_with_hash_value(polar_login_history_hash, &useroid, hashcode, HASH_FIND, &found);
	if (!found)
	{
		/* New user information needs to be inserted */
		LWLockRelease(partitionlock);
		LWLockAcquire(partitionlock, LW_EXCLUSIVE);

		/* Find again to prevent having been inserted by another process */
		entry = (polar_login_history_hash_entry *)
			hash_search_with_hash_value(polar_login_history_hash, &useroid, hashcode, HASH_FIND, &found);
		if (!found)
		{
			uint32		hash_value;
			bool		user_exist;
			int			new_id;

			/*
			 * Check whether the user exists to prevent it from being dropped
			 * by other processes. Invalidates cached data first.
			 */
			hash_value = GetSysCacheHashValue1(AUTHOID, ObjectIdGetDatum(useroid));
			SysCacheInvalidate(AUTHOID, hash_value);
			user_exist = SearchSysCacheExists1(AUTHOID, ObjectIdGetDatum(useroid));
			if (!intrans)
				CommitTransactionCommand();
			if (!user_exist)
			{
				LWLockRelease(partitionlock);
				ereport(WARNING, (errmsg("role %u was concurrently dropped", useroid)));
				return;
			}

			/* Looks for an empty array element */
			new_id = polar_login_history_get_element();
			if (new_id < 0)
			{
				/*
				 * There is not enough memory space and the error has been
				 * printed
				 */
				LWLockRelease(partitionlock);
				return;
			}

			entry = (polar_login_history_hash_entry *)
				hash_search_with_hash_value(polar_login_history_hash, &useroid, hashcode, HASH_ENTER, NULL);
			entry->useroid = useroid;
			entry->index = new_id;

			first_login = true;
		}
		else
		{
			/* Just update the user information in the array */
			if (!intrans)
				CommitTransactionCommand();
		}
	}
	else
	{
		/* Just update the user information in the array */
		if (!intrans)
			CommitTransactionCommand();
	}

	elem = &polar_login_history_array[entry->index];
	SpinLockAcquire(&elem->spinlock);

	LWLockRelease(partitionlock);

	polar_login_history_show_info(&elem->logininfo, useroid, login_success, first_login);
	elem->state |= POLAR_LOGIN_HISTORY_VALID;
	SpinLockRelease(&elem->spinlock);
}

/*
 * When dropping a user, the login information is also deleted.
 */
static void
polar_delete_login_history(Oid useroid)
{
	uint32		hashcode;
	LWLock	   *partitionlock;
	polar_login_history_hash_entry *entry;
	bool		found;

	if (!OidIsValid(useroid))
		return;

	hashcode = get_hash_value(polar_login_history_hash, &useroid);
	partitionlock = polar_login_history_lock[hashcode % POLAR_LOGIN_HISTORY_LOCK_NUM].lwlock;

	/* Check whether the hash table contains information about the user */
	LWLockAcquire(partitionlock, LW_SHARED);
	entry = (polar_login_history_hash_entry *)
		hash_search_with_hash_value(polar_login_history_hash, &useroid, hashcode, HASH_FIND, &found);
	if (!found)
	{
		/* If not found, no processing is done */
		LWLockRelease(partitionlock);
	}
	else
	{
		LWLockRelease(partitionlock);

		/* Preparing to delete login information */
		LWLockAcquire(partitionlock, LW_EXCLUSIVE);
		if (hash_search_with_hash_value(polar_login_history_hash, &useroid, hashcode, HASH_REMOVE, NULL) == NULL)
		{
			/* It has been deleted by another process */
			LWLockRelease(partitionlock);
		}
		else
		{
			polar_login_history_array_struct *elem = &polar_login_history_array[entry->index];

			SpinLockAcquire(&elem->spinlock);

			LWLockRelease(partitionlock);

			/* Set the array element to invalid and add it to the free list */
			elem->state &= ~POLAR_LOGIN_HISTORY_VALID;
			SpinLockRelease(&elem->spinlock);
			polar_login_history_free_element(entry->index);
		}
	}
}

/*
 * When the top-level transaction is committed, the login information of the user
 * associated with the committed transaction (including sub-transactions) is deleted.
 */
static void
polar_delete_login_history_callback(XactEvent event, void *arg)
{
	polar_login_history_callback_arg *content = (polar_login_history_callback_arg *) arg;

	if (event == XACT_EVENT_COMMIT)
	{
		/* Submitted by the top-level transaction */
		if (GetCurrentTransactionId() == content->xid)
		{
			ListCell   *item;

			foreach(item, content->useroid)
				polar_delete_login_history(lfirst_oid(item));
		}
		else
		{
			TransactionId *children;
			int			nchildren = xactGetCommittedChildren(&children);

			for (int i = 0; i < nchildren; i++)
			{
				/*
				 * Indicates that the transaction has committed and the login
				 * information can be removed
				 */
				if (children[i] == content->xid)
				{
					ListCell   *item;

					foreach(item, content->useroid)
						polar_delete_login_history(lfirst_oid(item));
					break;
				}
			}
		}
	}
}

/*
 * Register a callback function when dropping users so that the associated login information
 * is not deleted until the transaction is committed.
 */
static void
polar_register_delete_login_history(List *userlist)
{
	MemoryContext oldContext;
	polar_login_history_callback_arg *arg;
	polar_login_history_xact_callback_item *item;

	if (!polar_login_history_check() || userlist == NIL)
		return;

	oldContext = MemoryContextSwitchTo(TopMemoryContext);

	arg = (polar_login_history_callback_arg *) palloc0(sizeof(polar_login_history_callback_arg));
	arg->xid = GetCurrentTransactionId();
	arg->useroid = list_copy(userlist);

	item = (polar_login_history_xact_callback_item *) palloc0(sizeof(polar_login_history_xact_callback_item));
	item->callback = polar_delete_login_history_callback;
	item->arg = arg;
	item->next = polar_login_history_xact_callback;
	polar_login_history_xact_callback = item;

	MemoryContextSwitchTo(oldContext);
}

/*
 * When a transaction is committed or dropped, this function is called to clean up the login information.
 */
static void
polar_callback_delete_login_history(XactEvent event)
{
	if (!polar_login_history_check() || polar_login_history_xact_callback == NULL)
		return;

	while (polar_login_history_xact_callback)
	{
		polar_login_history_xact_callback_item *item = polar_login_history_xact_callback;

		item->callback(event, item->arg);

		polar_login_history_xact_callback = item->next;
		list_free(((polar_login_history_callback_arg *) item->arg)->useroid);
		pfree(item->arg);
		pfree(item);
	}
}

/*
 * User login information is periodically written to a file by the polar_worker process.
 */
static void
polar_flush_login_history(void)
{
	FILE	   *file = NULL;

	if (!polar_login_history_check())
		return;

	file = AllocateFile(POLAR_LOGIN_HISTORY_FILE ".tmp", PG_BINARY_W);
	if (file == NULL)
		goto error;

	for (int i = 0; i < polar_login_history_maxnum; i++)
	{
		polar_login_history_array_struct *elem = &polar_login_history_array[i];

		SpinLockAcquire(&elem->spinlock);
		if (elem->state & POLAR_LOGIN_HISTORY_VALID)
		{
			polar_login_history_info info = elem->logininfo;

			SpinLockRelease(&elem->spinlock);
			if (fwrite(&info, sizeof(polar_login_history_info), 1, file) != 1)
				goto error;
		}
		else
			SpinLockRelease(&elem->spinlock);
	}

	FreeFile(file);
	/* Rename file into place, so we atomically replace any old one */
	(void) durable_rename(POLAR_LOGIN_HISTORY_FILE ".tmp", POLAR_LOGIN_HISTORY_FILE, LOG);
	return;

error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not write file \"%s\": %m",
					POLAR_LOGIN_HISTORY_FILE ".tmp")));
	if (file)
		FreeFile(file);
	unlink(POLAR_LOGIN_HISTORY_FILE ".tmp");
}

void
_PG_init(void)
{
	/* Define custom GUC variables */
	DefineCustomBoolVariable("polar_login_history.enable",
							 "Whether to enable the login history function",
							 "After the function is enabled, historical login information is displayed to users",
							 &polar_login_history_enable,
							 false,
							 PGC_POSTMASTER,
							 POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomIntVariable("polar_login_history.maxnum",
							"The maximum number of users that this function can record",
							NULL,
							&polar_login_history_maxnum,
							512,
							64,
							INT_MAX / 2,
							PGC_POSTMASTER,
							POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE,
							NULL,
							NULL,
							NULL);

	/* Install hooks */
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = polar_login_history_shmem_request;

	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = polar_login_history_shmem_startup;

	polar_update_login_history_hook = polar_update_login_history;
	polar_register_delete_login_history_hook = polar_register_delete_login_history;
	polar_callback_delete_login_history_hook = polar_callback_delete_login_history;
	polar_flush_login_history_hook = polar_flush_login_history;
}
