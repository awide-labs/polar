/*-------------------------------------------------------------------------
 *
 * smgr.c
 *	  public interface routines to storage manager switch.
 *
 *	  All file system operations in POSTGRES dispatch through these
 *	  routines.
 *
 * Portions Copyright (c) 2024, Alibaba Group Holding Limited
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/smgr/smgr.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xlogutils.h"
#include "lib/ilist.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/md.h"
#include "storage/smgr.h"
#include "utils/hsearch.h"
#include "utils/inval.h"

/* POLAR */
#include "access/polar_logindex_redo.h"
#include "access/xlog.h"
#include "storage/polar_rsc.h"

/*
 * This struct of function pointers defines the API between smgr.c and
 * any individual storage manager module.  Note that smgr subfunctions are
 * generally expected to report problems via elog(ERROR).  An exception is
 * that smgr_unlink should use elog(WARNING), rather than erroring out,
 * because we normally unlink relations during post-commit/abort cleanup,
 * and so it's too late to raise an error.  Also, various conditions that
 * would normally be errors should be allowed during bootstrap and/or WAL
 * recovery --- see comments in md.c for details.
 */
typedef struct f_smgr
{
	void		(*smgr_init) (void);	/* may be NULL */
	void		(*smgr_shutdown) (void);	/* may be NULL */
	void		(*smgr_open) (SMgrRelation reln);
	void		(*smgr_close) (SMgrRelation reln, ForkNumber forknum);
	void		(*smgr_create) (SMgrRelation reln, ForkNumber forknum,
								bool isRedo);
	bool		(*smgr_exists) (SMgrRelation reln, ForkNumber forknum);
	void		(*smgr_unlink) (RelFileNodeBackend rnode, ForkNumber forknum,
								bool isRedo);
	void		(*smgr_extend) (SMgrRelation reln, ForkNumber forknum,
								BlockNumber blocknum, const void *buffer, bool skipFsync);
	void		(*smgr_zeroextend) (SMgrRelation reln, ForkNumber forknum,
									BlockNumber blocknum, int nblocks, bool skipFsync);
	bool		(*smgr_prefetch) (SMgrRelation reln, ForkNumber forknum,
								  BlockNumber blocknum);
	void		(*smgr_read) (SMgrRelation reln, ForkNumber forknum,
							  BlockNumber blocknum, void *buffer);
	void		(*smgr_write) (SMgrRelation reln, ForkNumber forknum,
							   BlockNumber blocknum, const void *buffer, bool skipFsync);
	void		(*smgr_writeback) (SMgrRelation reln, ForkNumber forknum,
								   BlockNumber blocknum, BlockNumber nblocks);
	BlockNumber (*smgr_nblocks) (SMgrRelation reln, ForkNumber forknum);
	void		(*smgr_truncate) (SMgrRelation reln, ForkNumber forknum,
								  BlockNumber old_blocks, BlockNumber nblocks);
	void		(*smgr_immedsync) (SMgrRelation reln, ForkNumber forknum);
	void		(*smgr_registersync) (SMgrRelation reln, ForkNumber forknum);
	/* POLAR: bulk read */
	void		(*polar_smgr_bulkread) (SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
										int nblocks, void *buffer);
	/* POLAR: bulk write */
	void		(*polar_smgr_bulkwrite) (SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
										 int nblocks, const void *buffer, bool skipFsync);
	/* POLAR: bulk extend */
	void		(*polar_smgr_bulkextend) (SMgrRelation reln, ForkNumber forknum,
										  BlockNumber blocknum, int nblocks, const void *buffer, bool skipFsync);
	/* POLAR end */
} f_smgr;

static const f_smgr smgrsw[] = {
	/* magnetic disk */
	{
		.smgr_init = mdinit,
		.smgr_shutdown = NULL,
		.smgr_open = mdopen,
		.smgr_close = mdclose,
		.smgr_create = mdcreate,
		.smgr_exists = mdexists,
		.smgr_unlink = mdunlink,
		.smgr_extend = mdextend,
		.smgr_zeroextend = mdzeroextend,
		.smgr_prefetch = mdprefetch,
		.smgr_read = mdread,
		.smgr_write = mdwrite,
		.smgr_writeback = mdwriteback,
		.smgr_nblocks = mdnblocks,
		.smgr_truncate = mdtruncate,
		.smgr_immedsync = mdimmedsync,
		.smgr_registersync = mdregistersync,
		/* POLAR: bulk read */
		.polar_smgr_bulkread = polar_mdbulkread,
		/* POLAR: bulk write */
		.polar_smgr_bulkwrite = polar_mdbulkwrite,
		/* POLAR: extend batch */
		.polar_smgr_bulkextend = polar_mdbulkextend,
		/* POLAR end */
	}
};

static const int NSmgr = lengthof(smgrsw);

/*
 * Each backend has a hashtable that stores all extant SMgrRelation objects.
 * In addition, "unowned" SMgrRelation objects are chained together in a list.
 */
static HTAB *SMgrRelationHash = NULL;

static dlist_head unowned_relns;

/* local function prototypes */
static void smgrshutdown(int code, Datum arg);


/*
 *	smgrinit(), smgrshutdown() -- Initialize or shut down storage
 *								  managers.
 *
 * Note: smgrinit is called during backend startup (normal or standalone
 * case), *not* during postmaster start.  Therefore, any resources created
 * here or destroyed in smgrshutdown are backend-local.
 */
void
smgrinit(void)
{
	int			i;

	for (i = 0; i < NSmgr; i++)
	{
		if (smgrsw[i].smgr_init)
			smgrsw[i].smgr_init();
	}

	/* register the shutdown proc */
	on_proc_exit(smgrshutdown, 0);
}

/*
 * on_proc_exit hook for smgr cleanup during backend shutdown
 */
static void
smgrshutdown(int code, Datum arg)
{
	int			i;

	for (i = 0; i < NSmgr; i++)
	{
		if (smgrsw[i].smgr_shutdown)
			smgrsw[i].smgr_shutdown();
	}
}

/*
 *	smgropen() -- Return an SMgrRelation object, creating it if need be.
 *
 *		This does not attempt to actually open the underlying file.
 */
SMgrRelation
smgropen(RelFileNode rnode, BackendId backend)
{
	RelFileNodeBackend brnode;
	SMgrRelation reln;
	bool		found;

	if (SMgrRelationHash == NULL)
	{
		/* First time through: initialize the hash table */
		HASHCTL		ctl;

		ctl.keysize = sizeof(RelFileNodeBackend);
		ctl.entrysize = sizeof(SMgrRelationData);
		SMgrRelationHash = hash_create("smgr relation table", 400,
									   &ctl, HASH_ELEM | HASH_BLOBS);
		dlist_init(&unowned_relns);
	}

	/* Look up or create an entry */
	brnode.node = rnode;
	brnode.backend = backend;
	reln = (SMgrRelation) hash_search(SMgrRelationHash,
									  (void *) &brnode,
									  HASH_ENTER, &found);

	/* Initialize it if not present before */
	if (!found)
	{
		int			forknum;

		/* hash_search already filled in the lookup key */
		reln->smgr_owner = NULL;
		reln->smgr_targblock = InvalidBlockNumber;
		for (int i = 0; i <= MAX_FORKNUM; ++i)
			reln->smgr_cached_nblocks[i] = InvalidBlockNumber;
		reln->smgr_which = 0;	/* we only have md.c at present */

		/*
		 * POLAR RSC: initialization shared entry reference and generation.
		 */
		reln->rsc_ref = NULL;
		reln->rsc_generation = 0;
		/* POLAR end */

		/* implementation-specific initialization */
		smgrsw[reln->smgr_which].smgr_open(reln);

		/* POLAR: bulk extend status */
		for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
			polar_smgr_clear_bulk_extend(reln, forknum);
		/* POLAR end */

		/* it has no owner yet */
		dlist_push_tail(&unowned_relns, &reln->node);
	}

	return reln;
}

/*
 * smgrsetowner() -- Establish a long-lived reference to an SMgrRelation object
 *
 * There can be only one owner at a time; this is sufficient since currently
 * the only such owners exist in the relcache.
 */
void
smgrsetowner(SMgrRelation *owner, SMgrRelation reln)
{
	/* We don't support "disowning" an SMgrRelation here, use smgrclearowner */
	Assert(owner != NULL);

	/*
	 * First, unhook any old owner.  (Normally there shouldn't be any, but it
	 * seems possible that this can happen during swap_relation_files()
	 * depending on the order of processing.  It's ok to close the old
	 * relcache entry early in that case.)
	 *
	 * If there isn't an old owner, then the reln should be in the unowned
	 * list, and we need to remove it.
	 */
	if (reln->smgr_owner)
		*(reln->smgr_owner) = NULL;
	else
		dlist_delete(&reln->node);

	/* Now establish the ownership relationship. */
	reln->smgr_owner = owner;
	*owner = reln;
}

/*
 * smgrclearowner() -- Remove long-lived reference to an SMgrRelation object
 *					   if one exists
 */
void
smgrclearowner(SMgrRelation *owner, SMgrRelation reln)
{
	/* Do nothing if the SMgrRelation object is not owned by the owner */
	if (reln->smgr_owner != owner)
		return;

	/* unset the owner's reference */
	*owner = NULL;

	/* unset our reference to the owner */
	reln->smgr_owner = NULL;

	/* add to list of unowned relations */
	dlist_push_tail(&unowned_relns, &reln->node);
}

/*
 *	smgrexists() -- Does the underlying file for a fork exist?
 */
bool
smgrexists(SMgrRelation reln, ForkNumber forknum)
{
	return smgrsw[reln->smgr_which].smgr_exists(reln, forknum);
}

/*
 *	smgrclose() -- Close and delete an SMgrRelation object.
 */
void
smgrclose(SMgrRelation reln)
{
	SMgrRelation *owner;
	ForkNumber	forknum;

	for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
		smgrsw[reln->smgr_which].smgr_close(reln, forknum);

	owner = reln->smgr_owner;

	if (!owner)
		dlist_delete(&reln->node);

	if (hash_search(SMgrRelationHash,
					(void *) &(reln->smgr_rnode),
					HASH_REMOVE, NULL) == NULL)
		elog(ERROR, "SMgrRelation hashtable corrupted");

	/*
	 * Unhook the owner pointer, if any.  We do this last since in the remote
	 * possibility of failure above, the SMgrRelation object will still exist.
	 */
	if (owner)
		*owner = NULL;
}

/*
 *	smgrrelease() -- Release all resources used by this object.
 *
 *	The object remains valid.
 */
void
smgrrelease(SMgrRelation reln)
{
	for (ForkNumber forknum = 0; forknum <= MAX_FORKNUM; forknum++)
	{
		smgrsw[reln->smgr_which].smgr_close(reln, forknum);
		reln->smgr_cached_nblocks[forknum] = InvalidBlockNumber;
	}
	reln->smgr_targblock = InvalidBlockNumber;
}

/*
 *	smgrreleaseall() -- Release resources used by all objects.
 *
 *	This is called for PROCSIGNAL_BARRIER_SMGRRELEASE.
 */
void
smgrreleaseall(void)
{
	HASH_SEQ_STATUS status;
	SMgrRelation reln;

	/* Nothing to do if hashtable not set up */
	if (SMgrRelationHash == NULL)
		return;

	hash_seq_init(&status, SMgrRelationHash);

	while ((reln = (SMgrRelation) hash_seq_search(&status)) != NULL)
		smgrrelease(reln);
}

/*
 *	smgrcloseall() -- Close all existing SMgrRelation objects.
 */
void
smgrcloseall(void)
{
	HASH_SEQ_STATUS status;
	SMgrRelation reln;

	/* Nothing to do if hashtable not set up */
	if (SMgrRelationHash == NULL)
		return;

	hash_seq_init(&status, SMgrRelationHash);

	while ((reln = (SMgrRelation) hash_seq_search(&status)) != NULL)
		smgrclose(reln);
}

/*
 *	smgrclosenode() -- Close SMgrRelation object for given RelFileNode,
 *					   if one exists.
 *
 * This has the same effects as smgrclose(smgropen(rnode)), but it avoids
 * uselessly creating a hashtable entry only to drop it again when no
 * such entry exists already.
 */
void
smgrclosenode(RelFileNodeBackend rnode)
{
	SMgrRelation reln;

	/* Nothing to do if hashtable not set up */
	if (SMgrRelationHash == NULL)
		return;

	reln = (SMgrRelation) hash_search(SMgrRelationHash,
									  (void *) &rnode,
									  HASH_FIND, NULL);
	if (reln != NULL)
		smgrclose(reln);
}

/*
 *	smgrcreate() -- Create a new relation.
 *
 *		Given an already-created (but presumably unused) SMgrRelation,
 *		cause the underlying disk file or other storage for the fork
 *		to be created.
 */
void
smgrcreate(SMgrRelation reln, ForkNumber forknum, bool isRedo)
{
	smgrsw[reln->smgr_which].smgr_create(reln, forknum, isRedo);
}

/*
 *	smgrdosyncall() -- Immediately sync all forks of all given relations
 *
 *		All forks of all given relations are synced out to the store.
 *
 *		This is equivalent to FlushRelationBuffers() for each smgr relation,
 *		then calling smgrimmedsync() for all forks of each relation, but it's
 *		significantly quicker so should be preferred when possible.
 */
void
smgrdosyncall(SMgrRelation *rels, int nrels)
{
	int			i = 0;
	ForkNumber	forknum;

	if (nrels == 0)
		return;

	FlushRelationsAllBuffers(rels, nrels);

	/*
	 * Sync the physical file(s).
	 */
	for (i = 0; i < nrels; i++)
	{
		int			which = rels[i]->smgr_which;

		for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
		{
			if (smgrsw[which].smgr_exists(rels[i], forknum))
				smgrsw[which].smgr_immedsync(rels[i], forknum);
		}
	}
}

/*
 *	smgrdounlinkall() -- Immediately unlink all forks of all given relations
 *
 *		All forks of all given relations are removed from the store.  This
 *		should not be used during transactional operations, since it can't be
 *		undone.
 *
 *		If isRedo is true, it is okay for the underlying file(s) to be gone
 *		already.
 */
void
smgrdounlinkall(SMgrRelation *rels, int nrels, bool isRedo)
{
	int			i = 0;
	RelFileNodeBackend *rnodes;
	ForkNumber	forknum;

	if (nrels == 0)
		return;

	/*
	 * POLAR: Record relation size change infomation before doing the real
	 * work
	 */
	if (polar_is_replica() || polar_bg_redo_state_is_parallel(polar_logindex_redo_instance) ||
		polar_should_launch_standby_instant_recovery())
	{
		ForkNumber	forks[MAX_FORKNUM + 1] = {MAIN_FORKNUM, FSM_FORKNUM, VISIBILITYMAP_FORKNUM, INIT_FORKNUM};
		BlockNumber nblocks[MAX_FORKNUM + 1] = {0, 0, 0, 0};

		for (i = 0; i < nrels; i++)
			POLAR_RECORD_REL_SIZE(&(rels[i]->smgr_rnode.node), MAX_FORKNUM + 1, forks, nblocks);
	}

	/*
	 * Get rid of any remaining buffers for the relations.  bufmgr will just
	 * drop them without bothering to write the contents.
	 */
	DropRelFileNodesAllBuffers(rels, nrels);

	/*
	 * create an array which contains all relations to be dropped, and close
	 * each relation's forks at the smgr level while at it
	 */
	rnodes = palloc(sizeof(RelFileNodeBackend) * nrels);
	for (i = 0; i < nrels; i++)
	{
		RelFileNodeBackend rnode = rels[i]->smgr_rnode;
		int			which = rels[i]->smgr_which;

		rnodes[i] = rnode;

		/* Close the forks at smgr level */
		for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
			smgrsw[which].smgr_close(rels[i], forknum);
	}

	/*
	 * POLAR RSC: clean up for the relation to be unlinked.
	 */
	if (POLAR_RSC_ENABLED())
		for (i = 0; i < nrels; i++)
			polar_rsc_drop_entry(&rnodes[i].node);
	/* POLAR end */

	/*
	 * Send a shared-inval message to force other backends to close any
	 * dangling smgr references they may have for these rels.  We should do
	 * this before starting the actual unlinking, in case we fail partway
	 * through that step.  Note that the sinval messages will eventually come
	 * back to this backend, too, and thereby provide a backstop that we
	 * closed our own smgr rel.
	 */
	for (i = 0; i < nrels; i++)
		CacheInvalidateSmgr(rnodes[i]);

	/*
	 * Delete the physical file(s).
	 *
	 * Note: smgr_unlink must treat deletion failure as a WARNING, not an
	 * ERROR, because we've already decided to commit or abort the current
	 * xact.
	 */
	if (!polar_is_replica())
	{
		for (i = 0; i < nrels; i++)
		{
			int			which = rels[i]->smgr_which;

			for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
				smgrsw[which].smgr_unlink(rnodes[i], forknum, isRedo);
		}
	}

	pfree(rnodes);
}


/*
 *	smgrextend() -- Add a new block to a file.
 *
 *		The semantics are nearly the same as smgrwrite(): write at the
 *		specified position.  However, this is to be used for the case of
 *		extending a relation (i.e., blocknum is at or beyond the current
 *		EOF).  Note that we assume writing a block beyond current EOF
 *		causes intervening file space to become filled with zeroes.
 */
void
smgrextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		   const void *buffer, bool skipFsync)
{
	smgrsw[reln->smgr_which].smgr_extend(reln, forknum, blocknum,
										 buffer, skipFsync);

	/*
	 * Normally we expect this to increase nblocks by one, but if the cached
	 * value isn't as expected, just invalidate it so the next call asks the
	 * kernel.
	 */

	/*
	 * POLAR: because smgr_cached_nblocks is for backend not shared, fake
	 * nblocks can be updated. recovery should use fake nblocks to alloc
	 * extend zero buffer.
	 */
	if (reln->smgr_cached_nblocks[forknum] == blocknum)
		reln->smgr_cached_nblocks[forknum] = blocknum + 1;
	else
		reln->smgr_cached_nblocks[forknum] = InvalidBlockNumber;

	/*
	 * POLAR RSC: update new blocknum into entry.
	 */
	if (POLAR_RSC_SHOULD_UPDATE(reln, forknum))
		polar_rsc_update_entry(reln, forknum, blocknum + 1);
	/* POLAR end */
}

/*
 * smgrzeroextend() -- Add new zeroed out blocks to a file.
 *
 * Similar to smgrextend(), except the relation can be extended by
 * multiple blocks at once and the added blocks will be filled with
 * zeroes.
 */
void
smgrzeroextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			   int nblocks, bool skipFsync)
{
	smgrsw[reln->smgr_which].smgr_zeroextend(reln, forknum, blocknum,
											 nblocks, skipFsync);

	/*
	 * Normally we expect this to increase the fork size by nblocks, but if
	 * the cached value isn't as expected, just invalidate it so the next call
	 * asks the kernel.
	 */
	if (reln->smgr_cached_nblocks[forknum] == blocknum)
		reln->smgr_cached_nblocks[forknum] = blocknum + nblocks;
	else
		reln->smgr_cached_nblocks[forknum] = InvalidBlockNumber;

	/*
	 * POLAR RSC: update new blocknum into entry.
	 */
	if (POLAR_RSC_SHOULD_UPDATE(reln, forknum))
		polar_rsc_update_entry(reln, forknum, blocknum + nblocks);
	/* POLAR end */
}

/*
 * polar_smgrbulkextend() -- Add new blocks to a file.
 *
 * The semantics are nearly the same as smgrwrite(): write at the
 * specified position.  However, this is to be used for the case of
 * extending a relation (i.e., blocknum is at or beyond the current
 * EOF).  Note that we assume writing nblocks beyond current EOF
 * causes intervening file space to become filled with zeroes.
 */
void
polar_smgrbulkextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
					 int nblocks, const void *buffer, bool skipFsync)
{
	smgrsw[reln->smgr_which].polar_smgr_bulkextend(reln, forknum, blocknum, nblocks, buffer, skipFsync);

	/*
	 * Normally we expect this to increase the fork size by nblocks, but if
	 * the cached value isn't as expected, just invalidate it so the next call
	 * asks the kernel.
	 */
	if (reln->smgr_cached_nblocks[forknum] == blocknum)
		reln->smgr_cached_nblocks[forknum] = blocknum + nblocks;
	else
		reln->smgr_cached_nblocks[forknum] = InvalidBlockNumber;

	/*
	 * POLAR RSC: update new blocknum into entry.
	 */
	if (POLAR_RSC_SHOULD_UPDATE(reln, forknum))
		polar_rsc_update_entry(reln, forknum, blocknum + nblocks);
	/* POLAR end */
}

/*
 *	smgrprefetch() -- Initiate asynchronous read of the specified block of a relation.
 *
 *		In recovery only, this can return false to indicate that a file
 *		doesn't	exist (presumably it has been dropped by a later WAL
 *		record).
 */
bool
smgrprefetch(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum)
{
	return smgrsw[reln->smgr_which].smgr_prefetch(reln, forknum, blocknum);
}

/*
 *	smgrread() -- read a particular block from a relation into the supplied
 *				  buffer.
 *
 *		This routine is called from the buffer manager in order to
 *		instantiate pages in the shared buffer cache.  All storage managers
 *		return pages in the format that POSTGRES expects.
 */
void
smgrread(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		 void *buffer)
{
	smgrsw[reln->smgr_which].smgr_read(reln, forknum, blocknum, buffer);
}

/*
 * polar_smgrbulkread() -- read multi particular blocks from a relation into
 *						   the supplied buffers.
 *
 * This routine is called from the buffer manager in order to
 * instantiate pages in the shared buffer cache.  All storage managers
 * return pages in the format that POSTGRES expects.
 */
void
polar_smgrbulkread(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
				   int nblocks, void *buffer)
{
	smgrsw[reln->smgr_which].polar_smgr_bulkread(reln, forknum, blocknum, nblocks, buffer);
}

/*
 *	smgrwrite() -- Write the supplied buffer out.
 *
 *		This is to be used only for updating already-existing blocks of a
 *		relation (ie, those before the current EOF).  To extend a relation,
 *		use smgrextend().
 *
 *		This is not a synchronous write -- the block is not necessarily
 *		on disk at return, only dumped out to the kernel.  However,
 *		provisions will be made to fsync the write before the next checkpoint.
 *
 *		skipFsync indicates that the caller will make other provisions to
 *		fsync the relation, so we needn't bother.  Temporary relations also
 *		do not require fsync.
 */
void
smgrwrite(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		  const void *buffer, bool skipFsync)
{
	smgrsw[reln->smgr_which].smgr_write(reln, forknum, blocknum,
										buffer, skipFsync);
}

/*
 * polar_smgrbulkwrite() -- Write the supplied buffers out.
 *
 * This is to be used only for updating already-existing blocks of a
 * relation (ie, those before the current EOF).  To extend a relation,
 * use polar_smgrbulkextend().
 *
 * This is not a synchronous write -- the block is not necessarily
 * on disk at return, only dumped out to the kernel.  However,
 * provisions will be made to fsync the write before the next checkpoint.
 *
 * NB: The mechanism to ensure fsync at next checkpoint assumes that there is
 * something that prevents a concurrent checkpoint from "racing ahead" of the
 * write.  One way to prevent that is by holding a lock on the buffer; the
 * buffer manager's writes are protected by that.  The bulk writer facility
 * in bulk_write.c checks the redo pointer and calls smgrimmedsync() if a
 * checkpoint happened; that relies on the fact that no other backend can be
 * concurrently modifying the page.
 *
 * skipFsync indicates that the caller will make other provisions to
 * fsync the relation, so we needn't bother.  Temporary relations also
 * do not require fsync.
 */
void
polar_smgrbulkwrite(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
					int nblocks, const void *buffer, bool skipFsync)
{
	smgrsw[reln->smgr_which].polar_smgr_bulkwrite(reln, forknum, blocknum, nblocks,
												  buffer, skipFsync);
}

/*
 *	smgrwriteback() -- Trigger kernel writeback for the supplied range of
 *					   blocks.
 */
void
smgrwriteback(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			  BlockNumber nblocks)
{
	smgrsw[reln->smgr_which].smgr_writeback(reln, forknum, blocknum,
											nblocks);
}

/*
 * POLAR: original call of f_smgr level API.
 */
inline BlockNumber
smgrnblocks_real(SMgrRelation reln, ForkNumber forknum)
{
	return smgrsw[reln->smgr_which].smgr_nblocks(reln, forknum);
}

/* POLAR end */

/*
 *	smgrnblocks() -- Calculate the number of blocks in the
 *					 supplied relation.
 */
BlockNumber
smgrnblocks(SMgrRelation reln, ForkNumber forknum)
{
	BlockNumber result;

	/* Check and return if we get the cached value for the number of blocks. */
	result = smgrnblocks_cached(reln, forknum);
	if (result != InvalidBlockNumber)
		return result;

	/*
	 * POLAR RSC: search for nblocks value, evict and update entry if
	 * necessary.
	 */
	if (POLAR_RSC_SHOULD_UPDATE(reln, forknum))
		result = polar_rsc_search_entry(reln, forknum, true);
	else
		result = smgrnblocks_real(reln, forknum);
	/* POLAR end */

	reln->smgr_cached_nblocks[forknum] = result;

	return result;
}

/*
 * polar_smgr_init_bulk_extend() -- Init polar bulk extend backend-local-variable.
 */
void
polar_smgr_init_bulk_extend(SMgrRelation reln, ForkNumber forknum)
{
	Assert(!reln->polar_flag_for_bulk_extend[forknum]);
	reln->polar_nblocks_faked_for_bulk_extend[forknum] = smgrnblocks(reln, forknum);

	/*
	 * polar_flag_for_bulk_extend must be set after
	 * polar_nblocks_faked_for_bulk_extend, as polar_flag_for_bulk_extend have
	 * an effort on result of smgrnblocks().
	 */
	reln->polar_flag_for_bulk_extend[forknum] = true;
}

/*
 * polar_smgr_clear_bulk_extend() -- Clear polar bulk extend backend-local-variable.
 */
void
polar_smgr_clear_bulk_extend(SMgrRelation reln, ForkNumber forknum)
{
	reln->polar_flag_for_bulk_extend[forknum] = false;
}

/*
 *	smgrnblocks_cached() -- Get the cached number of blocks in the supplied
 *							relation.
 *
 * Returns an InvalidBlockNumber when not in recovery and when the relation
 * fork size is not cached.
 */
BlockNumber
smgrnblocks_cached(SMgrRelation reln, ForkNumber forknum)
{
	/*
	 * POLAR RSC: make sure all processes will use the shared-memory-based
	 * search/update mechanism of RSC to get accurate shared nblocks number.
	 */
	if (polar_enable_rel_size_cache)
		return InvalidBlockNumber;
	/* POLAR end */

	/*
	 * For now, this function uses cached values only in recovery due to lack
	 * of a shared invalidation mechanism for changes in file size.  Code
	 * elsewhere reads smgr_cached_nblocks and copes with stale data.
	 *
	 * POLAR: The startup process is just parsing xlog to build logindex
	 * without doing the real replaying work. So there's no guarantee that all
	 * blocks of every xlog will be accessed by startup. In other words, the
	 * cache of smgr's nblock may be not right in startup. However, in standby
	 * parallel replaying mode, it's safe to use this smgr nblocks cache
	 * because only startup process could extend file.
	 */
	if (InRecovery && reln->smgr_cached_nblocks[forknum] != InvalidBlockNumber &&
		!polar_is_replica())
		return reln->smgr_cached_nblocks[forknum];

	return InvalidBlockNumber;
}

/*
 *	smgrtruncate() -- Truncate the given forks of supplied relation to
 *					  each specified numbers of blocks
 *
 * Backward-compatible version of smgrtruncate2() for the benefit of external
 * callers.  This version isn't used in PostgreSQL core code, and can't be
 * used in a critical section.
 */
void
smgrtruncate(SMgrRelation reln, ForkNumber *forknum, int nforks,
			 BlockNumber *nblocks)
{
	BlockNumber old_nblocks[MAX_FORKNUM + 1];

	for (int i = 0; i < nforks; ++i)
		old_nblocks[i] = smgrnblocks(reln, forknum[i]);

	smgrtruncate2(reln, forknum, nforks, old_nblocks, nblocks);
}

/*
 * smgrtruncate2() -- Truncate the given forks of supplied relation to
 *					  each specified numbers of blocks
 *
 * The truncation is done immediately, so this can't be rolled back.
 *
 * The caller must hold AccessExclusiveLock on the relation, to ensure that
 * other backends receive the smgr invalidation event that this function sends
 * before they access any forks of the relation again.  The current size of
 * the forks should be provided in old_nblocks.  This function should normally
 * be called in a critical section, but the current size must be checked
 * outside the critical section, and no interrupts or smgr functions relating
 * to this relation should be called in between.
 */
void
smgrtruncate2(SMgrRelation reln, ForkNumber *forknum, int nforks,
			  BlockNumber *old_nblocks, BlockNumber *nblocks)
{
	int			i;

	/* POLAR :record the new relation size to the cache */
	POLAR_RECORD_REL_SIZE(&(reln->smgr_rnode.node), nforks, forknum, nblocks);
	/* POLAR end */

	/*
	 * Get rid of any buffers for the about-to-be-deleted blocks. bufmgr will
	 * just drop them without bothering to write the contents.
	 */
	DropRelFileNodeBuffers(reln, forknum, nforks, nblocks);

	/*
	 * Send a shared-inval message to force other backends to close any smgr
	 * references they may have for this rel.  This is useful because they
	 * might have open file pointers to segments that got removed, and/or
	 * smgr_targblock variables pointing past the new rel end.  (The inval
	 * message will come back to our backend, too, causing a
	 * probably-unnecessary local smgr flush.  But we don't expect that this
	 * is a performance-critical path.)  As in the unlink code, we want to be
	 * sure the message is sent before we start changing things on-disk.
	 */
	CacheInvalidateSmgr(reln->smgr_rnode);

	/* Do the truncation */
	for (i = 0; i < nforks; i++)
	{
		/* Make the cached size is invalid if we encounter an error. */
		reln->smgr_cached_nblocks[forknum[i]] = InvalidBlockNumber;

		smgrsw[reln->smgr_which].smgr_truncate(reln, forknum[i],
											   old_nblocks[i], nblocks[i]);

		/*
		 * We might as well update the local smgr_cached_nblocks values. The
		 * smgr cache inval message that this function sent will cause other
		 * backends to invalidate their copies of smgr_fsm_nblocks and
		 * smgr_vm_nblocks, and these ones too at the next command boundary.
		 * But these ensure they aren't outright wrong until then.
		 */
		reln->smgr_cached_nblocks[forknum[i]] = nblocks[i];

		/*
		 * POLAR RSC: update blocknum after truncate.
		 */
		if (POLAR_RSC_SHOULD_UPDATE(reln, forknum[i]))
			polar_rsc_update_entry(reln, forknum[i], nblocks[i]);
		/* POLAR end */
	}
}

/*
 * smgrregistersync() -- Request a relation to be sync'd at next checkpoint
 *
 * This can be used after calling smgrwrite() or smgrextend() with skipFsync =
 * true, to register the fsyncs that were skipped earlier.
 *
 * Note: be mindful that a checkpoint could already have happened between the
 * smgrwrite or smgrextend calls and this!  In that case, the checkpoint
 * already missed fsyncing this relation, and you should use smgrimmedsync
 * instead.  Most callers should use the bulk loading facility in bulk_write.c
 * which handles all that.
 */
void
smgrregistersync(SMgrRelation reln, ForkNumber forknum)
{
	smgrsw[reln->smgr_which].smgr_registersync(reln, forknum);
}

/*
 *	smgrimmedsync() -- Force the specified relation to stable storage.
 *
 *		Synchronously force all previous writes to the specified relation
 *		down to disk.
 *
 *		This is useful for building completely new relations (eg, new
 *		indexes).  Instead of incrementally WAL-logging the index build
 *		steps, we can just write completed index pages to disk with smgrwrite
 *		or smgrextend, and then fsync the completed index file before
 *		committing the transaction.  (This is sufficient for purposes of
 *		crash recovery, since it effectively duplicates forcing a checkpoint
 *		for the completed index.  But it is *not* sufficient if one wishes
 *		to use the WAL log for PITR or replication purposes: in that case
 *		we have to make WAL entries as well.)
 *
 *		The preceding writes should specify skipFsync = true to avoid
 *		duplicative fsyncs.
 *
 *		Note that you need to do FlushRelationBuffers() first if there is
 *		any possibility that there are dirty buffers for the relation;
 *		otherwise the sync is not very meaningful.
 */
void
smgrimmedsync(SMgrRelation reln, ForkNumber forknum)
{
	smgrsw[reln->smgr_which].smgr_immedsync(reln, forknum);
}

/*
 * AtEOXact_SMgr
 *
 * This routine is called during transaction commit or abort (it doesn't
 * particularly care which).  All transient SMgrRelation objects are closed.
 *
 * We do this as a compromise between wanting transient SMgrRelations to
 * live awhile (to amortize the costs of blind writes of multiple blocks)
 * and needing them to not live forever (since we're probably holding open
 * a kernel file descriptor for the underlying file, and we need to ensure
 * that gets closed reasonably soon if the file gets deleted).
 */
void
AtEOXact_SMgr(void)
{
	dlist_mutable_iter iter;

	/* POLAR RSC: release the holding reference on transaction ends */
	polar_rsc_release_holding_ref();
	/* POLAR end */

	/*
	 * Zap all unowned SMgrRelations.  We rely on smgrclose() to remove each
	 * one from the list.
	 */
	dlist_foreach_modify(iter, &unowned_relns)
	{
		SMgrRelation rel = dlist_container(SMgrRelationData, node,
										   iter.cur);

		Assert(rel->smgr_owner == NULL);

		smgrclose(rel);
	}
}

/*
 * This routine is called when we are ordered to release all open files by a
 * ProcSignalBarrier.
 */
bool
ProcessBarrierSmgrRelease(void)
{
	smgrreleaseall();
	return true;
}
