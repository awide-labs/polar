/*
 * pg_bulkload: bin/recovery.c
 *
 *	  Copyright (c) 2007-2026, NTT, Inc.
 */

/**
 *	@file
 *	@brief Recovery routine for pg_bulkload.
 */
#include "postgres_fe.h"

#include <signal.h>
#include <time.h>
#ifndef WIN32
#include <sys/shm.h>
#endif

#include "polar_vfs/polar_vfs_fe.h"
#include "pg_loadstatus.h"
#include "pgut/pgut.h"
#include "pgut/pgut-recovery.h"

#include "storage/pg_shmem.h"

/*
 * Global variables
 */

extern char  *DataDir;

/*
 * Prototypes
 */

/* Entry point for the recovery. */
extern int	LoaderRecoveryMain(void);

/* Deletes a lock file. */
static void LoaderUnlinkLockFile(const char *filename);

/* Creates a lock file. */
static void LoaderCreateLockFile(const char *filename,
								 bool amPostmaster,
								 bool isDDLock, const char *refName);


/**
 * @brief Entry point for recovery process
 *
 * @param  none
 * @return exitcode
 */
int
LoaderRecoveryMain(void)
{
	if (chdir(DataDir) < 0)
		elog(ERROR, "could not change directory to \"%s\"", DataDir);

	LoaderCreateLockFile("postmaster.pid", true, true, DataDir);
	StartLoaderRecovery(DataDir);
	LoaderUnlinkLockFile("postmaster.pid");
	return 0;
}

/*------------------------------------------------------------------------
 *	 The following codes are copied from PostgreSQL source code with some changes.
 *	   - UnlinkLockFile()		 Change the argument. Remove free(fname)
 *		  + backend/utils/init/miscinit.c:466
 *	   - CreateLockFile()		 Replace ereport with the message report function of bulk loader.
 *		  + backend/utils/init/miscinit.c:488
 *	   - PGSharedMemoryIsInUse() No change
 *		  + backend/port/pg_shmem.c:196
 *
 *------------------------------------------------------------------------*/

/*-------------------------------------------------------------------------
 *				Interlock-file support
 *
 * These routines are used to create both a data-directory lockfile
 * ($DATADIR/postmaster.pid) and a Unix-socket-file lockfile ($SOCKFILE.lock).
 * Both kinds of files contain the same info:
 *
 *		Owning process' PID
 *		Data directory path
 *
 * By convention, the owning process' PID is negated if it is a standalone
 * backend rather than a postmaster.  This is just for informational purposes.
 * The path is also just for informational purposes (so that a socket lockfile
 * can be more easily traced to the associated postmaster).
 *
 * A data-directory lockfile can optionally contain a third line, containing
 * the key and ID for the shared memory block used by this postmaster.
 *
 * On successful lockfile creation, a proc_exit callback to remove the
 * lockfile is automatically created.
 *-------------------------------------------------------------------------
 */


/**
 * @brief proc_exit callback to remove a lockfile.
 *
 * Changes with the original source code.
 * <ol>
 *	 <li> Simplify the argument of the function. </li>
 *	 <li> Remove free(fname). </li>
 * </ol>
 */

/*
 * proc_exit callback to remove a lockfile.
 */
static void
LoaderUnlinkLockFile(const char *fname)
{
	if (fname != NULL)
	{
		if (unlink(fname) != 0)
		{
			/*
			 * Should we complain if the unlink fails?
			 */
		}
	}
}


/**
 * @brief  Create a lockfile.
 *
 * Changes from the original source code.
 * <ol>
 *	 <li> Use elog() to replace ereport(). </li>
 * </ol>
 */

/*
 * Create a lockfile.
 *
 * filename is the name of the lockfile to create.
 * amPostmaster is used to determine how to encode the output PID.
 * isDDLock and refName are used to determine what error message to produce.
 */
static void
LoaderCreateLockFile(const char *filename, bool amPostmaster,
					 bool isDDLock, const char *refName)
{
	int			fd;
	char		buffer[MAXPGPATH + 100];
	int			ntries;
	int			len;
	int			encoded_pid;
	pid_t		other_pid;
	pid_t		my_pid = getpid();

	/*
	 * We need a loop here because of race conditions.	But don't loop forever
	 * (for example, a non-writable $PGDATA directory might cause a failure
	 * that won't go away).  100 tries seems like plenty.
	 */
	for (ntries = 0;; ntries++)
	{
		/*
		 * Try to create the lock file --- O_EXCL makes this atomic.
		 *
		 * Think not to make the file protection weaker than 0600.	See
		 * comments below.
		 */
		fd = open(filename, O_RDWR | O_CREAT | O_EXCL | PG_BINARY, 0600);
		if (fd >= 0)
			break;				/* Success; exit the retry loop */

		/*
		 * Couldn't create the pid file. Probably it already exists.
		 */
		if ((errno != EEXIST && errno != EACCES) || ntries > 100)
			elog(FATAL,
				 "could not create lock file \"%s\": %m",
				 filename);

		/*
		 * Read the file to get the old owner's PID.  Note race condition
		 * here: file might have been deleted since we tried to create it.
		 */
		fd = open(filename, O_RDONLY | PG_BINARY, 0600);
		if (fd < 0)
		{
			if (errno == ENOENT)
				continue;		/* race condition; try again */
			elog(FATAL,
				 "could not open lock file \"%s\": %m",
				 filename);
		}
		if ((len = read(fd, buffer, sizeof(buffer) - 1)) < 0)
			elog(FATAL,
				 "could not read lock file \"%s\": %m",
				 filename);
		close(fd);

		buffer[len] = '\0';
		encoded_pid = atoi(buffer);

		/*
		 * if pid < 0, the pid is for postgres, not postmaster
		 */
		other_pid = (pid_t) (encoded_pid < 0 ? -encoded_pid : encoded_pid);

		if (other_pid <= 0)
			elog(FATAL, "bogus data in lock file \"%s\": %s", filename, buffer);

		/*
		 * Check to see if the other process still exists
		 *
		 * If the PID in the lockfile is our own PID or our parent's PID, then
		 * the file must be stale (probably left over from a previous system
		 * boot cycle).  We need this test because of the likelihood that a
		 * reboot will assign exactly the same PID as we had in the previous
		 * reboot.	Also, if there is just one more process launch in this
		 * reboot than in the previous one, the lockfile might mention our
		 * parent's PID.  We can reject that since we'd never be launched
		 * directly by a competing postmaster.	We can't detect grandparent
		 * processes unfortunately, but if the init script is written
		 * carefully then all but the immediate parent shell will be
		 * root-owned processes and so the kill test will fail with EPERM.
		 *
		 * We can treat the EPERM-error case as okay because that error
		 * implies that the existing process has a different userid than we
		 * do, which means it cannot be a competing postmaster.  A postmaster
		 * cannot successfully attach to a data directory owned by a userid
		 * other than its own.	(This is now checked directly in
		 * checkDataDir(), but has been true for a long time because of the
		 * restriction that the data directory isn't group- or
		 * world-accessible.)  Also, since we create the lockfiles mode 600,
		 * we'd have failed above if the lockfile belonged to another userid
		 * --- which means that whatever process kill() is reporting about
		 * isn't the one that made the lockfile.  (NOTE: this last
		 * consideration is the only one that keeps us from blowing away a
		 * Unix socket file belonging to an instance of Postgres being run by
		 * someone else, at least on machines where /tmp hasn't got a
		 * stickybit.)
		 *
		 * Windows hasn't got getppid(), but doesn't need it since it's not
		 * using real kill() either...
		 *
		 * Normally kill() will fail with ESRCH if the given PID doesn't
		 * exist. BeOS returns EINVAL for some silly reason, however.
		 */
		if (other_pid != my_pid
#ifndef WIN32
			&& other_pid != getppid()
#endif
			)
		{
			if (kill(other_pid, 0) == 0 || (errno != ESRCH && errno != EPERM))
				/*
				 * lockfile belongs to a live process
				 */
				elog(FATAL, "lock file \"%s\" already exists.\n"
					"Is another postmaster (PID %d) running in data directory \"%s\"?",
					filename, (int) other_pid, refName);
		}

		/*
		 * No, the creating process did not exist.	However, it could be that
		 * the postmaster crashed (or more likely was kill -9'd by a clueless
		 * admin) but has left orphan backends behind.	Check for this by
		 * looking to see if there is an associated shmem segment that is
		 * still in use.
		 */
		if (isDDLock)
		{
			char	   *ptr;
			unsigned long id1,
						id2;

			ptr = strchr(buffer, '\n');
			if (ptr != NULL && (ptr = strchr(ptr + 1, '\n')) != NULL)
			{
				ptr++;
				if (sscanf(ptr, "%lu %lu", &id1, &id2) == 2)
				{
					if (PGSharedMemoryIsInUse(id1, id2))
						elog(FATAL,
							 "pre-existing shared memory block "
							 "(key %lu, ID %lu) is still in use "
							 "If you're sure there are no old "
							 "server processes still running, remove "
							 "the shared memory block with the command "
							 "\"ipcrm\", or just delete the file \"%s\".",
							 id1, id2, filename);
				}
			}
		}

		/*
		 * Looks like nobody's home.  Unlink the file and try again to create
		 * it.	Need a loop because of possible race condition against other
		 * would-be creators.
		 */
		if (unlink(filename) < 0)
			elog(FATAL,
				 "could not remove old lock file \"%s\": %m "
				 "The file seems accidentally left over, but "
				 "it could not be removed. Please remove the file "
				 "by hand and try again.",
				 filename);
	}

	/*
	 * Successfully created the file, now fill it.
	 */
	snprintf(buffer, sizeof(buffer), "%d\n%s\n",
			 amPostmaster ? (int) my_pid : -((int) my_pid), DataDir);
	errno = 0;
	if (write(fd, buffer, strlen(buffer)) != strlen(buffer))
	{
		int			save_errno = errno;

		close(fd);
		unlink(filename);
		/*
		 * if write didn't set errno, assume problem is no disk space 
		 */
		errno = save_errno ? save_errno : ENOSPC;
		elog(FATAL,
			 "could not write lock file \"%s\": %m",
			 filename);
	}
	if (close(fd))
	{
		int			save_errno = errno;

		unlink(filename);
		errno = save_errno;
		elog(FATAL,
			 "could not write lock file \"%s\": %m",
			 filename);
	}
}

/*
 * PGSharedMemoryIsInUse
 *
 * Is a previously-existing shmem segment still existing and in use?
 *
 * The point of this exercise is to detect the case where a prior postmaster
 * crashed, but it left child backends that are still running.	Therefore
 * we only care about shmem segments that are associated with the intended
 * DataDir.  This is an important consideration since accidental matches of
 * shmem segment IDs are reasonably common.
 */
#ifndef WIN32

/**
 * @brief Shared memory ID returned by shmget(2).
 * See the line 40, backend/port/pg_shmem.c.
 */
typedef int IpcMemoryId;

#ifdef SHM_SHARE_MMU
#define PG_SHMAT_FLAGS SHM_SHARE_MMU
#else
#define PG_SHMAT_FLAGS 0
#endif

bool
PGSharedMemoryIsInUse(unsigned long id1, unsigned long id2)
{
	IpcMemoryId shmId = (IpcMemoryId) id2;
	struct shmid_ds shmStat;

	struct stat statbuf;
	PGShmemHeader *hdr;

	/*
	 * We detect whether a shared memory segment is in use by seeing whether
	 * it (a) exists and (b) has any processes are attached to it.
	 */
	if (shmctl(shmId, IPC_STAT, &shmStat) < 0)
	{
		/*
		 * EINVAL actually has multiple possible causes documented in the
		 * shmctl man page, but we assume it must mean the segment no longer
		 * exists.
		 */
		if (errno == EINVAL)
			return false;

		/*
		 * EACCES implies that the segment belongs to some other userid, which
		 * means it is not a Postgres shmem segment (or at least, not one that
		 * is relevant to our data directory).
		 */
		if (errno == EACCES)
			return false;

		/*
		 * Otherwise, we had better assume that the segment is in use. The
		 * only likely case is EIDRM, which implies that the segment has been
		 * IPC_RMID'd but there are still processes attached to it.
		 */
		return true;
	}

	/*
	 * If it has no attached processes, it's not in use 
	 */
	if (shmStat.shm_nattch == 0)
		return false;

	/*
	 * Try to attach to the segment and see if it matches our data directory.
	 * This avoids shmid-conflict problems on machines that are running
	 * several postmasters under the same userid.  On Windows, which doesn't
	 * have useful inode numbers, we can't do this so we punt and assume there
	 * is a conflict.
	 */
	if (stat(DataDir, &statbuf) < 0)
		return true;			/* if can't stat, be conservative */

	hdr = (PGShmemHeader *) shmat(shmId, NULL, PG_SHMAT_FLAGS);

	if (hdr == (PGShmemHeader *) -1)
		return true;			/* if can't attach, be conservative */

	if (hdr->magic != PGShmemMagic ||
		hdr->device != statbuf.st_dev || hdr->inode != statbuf.st_ino)
	{
		/*
		 * It's either not a Postgres segment, or not one for my data
		 * directory.  In either case it poses no threat.
		 */
		shmdt((void *) hdr);
		return false;
	}

	/*
	 * Trouble --- looks a lot like there's still live backends 
	 */
	shmdt((void *) hdr);

	return true;
}

#else	/* WIN32 */
#include "pg_bulkload_win32.c"
#endif
