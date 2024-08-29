/*-------------------------------------------------------------------------
 *
 * polar_nbtxlog_idx.c
 *   Implementation of parse btree records.
 *
 * Portions Copyright (c) 2021, Alibaba Group Holding limited
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *    src/backend/access/logindex/polar_nbtxlog_idx.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/bufmask.h"
#include "access/nbtxlog.h"
#include "access/nbtree.h"
#include "access/polar_logindex_redo.h"
#include "access/xlog.h"
#include "catalog/pg_control.h"
#include "miscadmin.h"
#include "storage/buf_internals.h"
#include "storage/standby.h"

static void
polar_btree_xlog_insert_save(polar_logindex_redo_ctl_t instance, bool isleaf, bool ismeta, XLogReaderState *record)
{
	if (!isleaf)
		polar_logindex_save_block(instance, record, 1);

	polar_logindex_save_block(instance, record, 0);

	if (ismeta)
		polar_logindex_save_block(instance, record, 2);
}

static void
polar_btree_xlog_insert_parse(polar_logindex_redo_ctl_t instance, bool isleaf, bool ismeta, XLogReaderState *record)
{
	if (!isleaf)
		polar_logindex_redo_parse(instance, record, 1);

	polar_logindex_redo_parse(instance, record, 0);

	if (ismeta)
		polar_logindex_redo_parse(instance, record, 2);
}

static void
polar_btree_xlog_split_save(polar_logindex_redo_ctl_t instance, XLogReaderState *record)
{
	if (XLogRecHasBlockRef(record, 3))
		polar_logindex_save_block(instance, record, 3);

	polar_logindex_save_block(instance, record, 1);
	polar_logindex_save_block(instance, record, 0);

	if (XLogRecHasBlockRef(record, 2))
		polar_logindex_save_block(instance, record, 2);
}

static void
polar_btree_xlog_split_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *record)
{
	BufferTag	rtag,
				tag;
	Buffer		rbuf,
				buf;
	polar_page_lock_t rpage_lock,
				page_lock;

	/* block id 3 is set only when isleaf is false */
	if (XLogRecHasBlockRef(record, 3))
		polar_logindex_redo_parse(instance, record, 3);

	POLAR_MINI_TRANS_REDO_PARSE(instance, record, 1, rtag, rpage_lock, rbuf);

	POLAR_MINI_TRANS_REDO_PARSE(instance, record, 0, tag, page_lock, buf);

	if (XLogRecHasBlockRef(record, 2))
		polar_logindex_redo_parse(instance, record, 2);

	if (BufferIsValid(rbuf))
		UnlockReleaseBuffer(rbuf);

	polar_logindex_mini_trans_unlock(instance->mini_trans, rpage_lock);

	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);

	polar_logindex_mini_trans_unlock(instance->mini_trans, page_lock);
}

static void
polar_btree_xlog_delete_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *record)
{
	/*
	 * If we have any conflict processing to do, it must happen before we
	 * update the page.
	 *
	 * Btree delete records can conflict with standby queries.  You might
	 * think that vacuum records would conflict as well, but we've handled
	 * that already.  XLOG_HEAP2_VACUUM records provide the highest xid
	 * cleaned by the vacuum of the heap and so we can resolve any conflicts
	 * just once when that arrives.  After that we know that no conflicts
	 * exist from individual btree vacuum records on that index.
	 */
	if (polar_enable_resolve_conflict && reachedConsistency && InHotStandby)
	{
		xl_btree_delete *xlrec = (xl_btree_delete *) XLogRecGetData(record);
		RelFileNode rnode;

		XLogRecGetBlockTag(record, 0, &rnode, NULL, NULL);

		ResolveRecoveryConflictWithSnapshot(xlrec->latestRemovedXid, rnode);
	}

	polar_logindex_redo_parse(instance, record, 0);
}

static void
polar_btree_xlog_reuse_page_parse(XLogReaderState *record)
{
	/*
	 * Btree reuse_page records exist to provide a conflict point when we
	 * reuse pages in the index via the FSM.  That's all they do though.
	 *
	 * latestRemovedXid was the page's btpo.xact.  The btpo.xact <
	 * RecentGlobalXmin test in _bt_page_recyclable() conceptually mirrors the
	 * pgxact->xmin > limitXmin test in GetConflictingVirtualXIDs().
	 * Consequently, one XID value achieves the same exclusion effect on
	 * primary and standby.
	 */
	if (polar_enable_resolve_conflict && reachedConsistency && InHotStandby)
	{
		xl_btree_reuse_page *xlrec = (xl_btree_reuse_page *) XLogRecGetData(record);

		ResolveRecoveryConflictWithSnapshot(XidFromFullTransactionId(xlrec->latestRemovedFullXid),
											xlrec->node);
	}
}

static void
polar_btree_xlog_unlink_page_save(polar_logindex_redo_ctl_t instance, uint8 info, XLogReaderState *record)
{
	if (XLogRecHasBlockRef(record, 1))
		polar_logindex_save_block(instance, record, 1);

	polar_logindex_save_block(instance, record, 0);
	polar_logindex_save_block(instance, record, 2);

	if (XLogRecHasBlockRef(record, 3))
		polar_logindex_save_block(instance, record, 3);

	if (info == XLOG_BTREE_UNLINK_PAGE_META)
		polar_logindex_save_block(instance, record, 4);
}

static void
polar_btree_xlog_unlink_page_parse(polar_logindex_redo_ctl_t instance, uint8 info, XLogReaderState *record)
{
	Buffer		leftbuf = InvalidBuffer,
				target = InvalidBuffer,
				rightbuf = InvalidBuffer;
	BufferTag	leftbuf_tag,
				target_tag,
				rightbuf_tag;
	polar_page_lock_t leftbuf_lock,
				target_lock,
				rightbuf_lock;

	/*
	 * Fix right-link of left sibling, if any. block 1 is set when leftsib is
	 * not P_NONE.
	 */
	if (XLogRecHasBlockRef(record, 1))
		POLAR_MINI_TRANS_REDO_PARSE(instance, record, 1, leftbuf_tag, leftbuf_lock, leftbuf);

	/* Rewrite target page as empty deleted page */
	POLAR_MINI_TRANS_REDO_PARSE(instance, record, 0, target_tag, target_lock, target);

	/* Fix left-link of right sibling */
	POLAR_MINI_TRANS_REDO_PARSE(instance, record, 2, rightbuf_tag, rightbuf_lock, rightbuf);

	/* Release siblings */
	if (XLogRecHasBlockRef(record, 1))
	{
		if (BufferIsValid(leftbuf))
			UnlockReleaseBuffer(leftbuf);
		polar_logindex_mini_trans_unlock(instance->mini_trans, leftbuf_lock);
	}
	if (BufferIsValid(rightbuf))
		UnlockReleaseBuffer(rightbuf);
	polar_logindex_mini_trans_unlock(instance->mini_trans, rightbuf_lock);

	/* Release target */
	if (BufferIsValid(target))
		UnlockReleaseBuffer(target);
	polar_logindex_mini_trans_unlock(instance->mini_trans, target_lock);

	if (XLogRecHasBlockRef(record, 3))
		polar_logindex_redo_parse(instance, record, 3);

	/* Update metapage if needed */
	if (info == XLOG_BTREE_UNLINK_PAGE_META)
		polar_logindex_redo_parse(instance, record, 4);
}

static void
polar_btree_xlog_newroot_save(polar_logindex_redo_ctl_t instance, XLogReaderState *record)
{
	polar_logindex_save_block(instance, record, 0);

	if (XLogRecHasBlockRef(record, 1))
		polar_logindex_save_block(instance, record, 1);

	polar_logindex_save_block(instance, record, 2);
}

static void
polar_btree_xlog_newroot_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *record)
{
	BufferTag	root_tag;
	Buffer		root_buf;
	polar_page_lock_t root_lock;

	POLAR_MINI_TRANS_REDO_PARSE(instance, record, 0, root_tag, root_lock, root_buf);

	/* block 1 is set when xlrec->level > 0 */
	if (XLogRecHasBlockRef(record, 1))
		polar_logindex_redo_parse(instance, record, 1);

	if (BufferIsValid(root_buf))
		UnlockReleaseBuffer(root_buf);

	polar_logindex_mini_trans_unlock(instance->mini_trans, root_lock);

	polar_logindex_redo_parse(instance, record, 2);
}

/*
 * _bt_clear_incomplete_split -- clear INCOMPLETE_SPLIT flag on a page
 *
 * This is a common subroutine of the redo functions of all the WAL record
 * types that can insert a downlink: insert, split, and newroot.
 */
static XLogRedoAction
polar_bt_clear_incomplete_split(XLogReaderState *record, uint8 block_id, Buffer *buffer)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	XLogRedoAction action = BLK_NOTFOUND;

	action = POLAR_READ_BUFFER_FOR_REDO(record, block_id, buffer);

	if (action == BLK_NEEDS_REDO)
	{
		Page		page = (Page) BufferGetPage(*buffer);
		BTPageOpaque pageop = BTPageGetOpaque(page);

		POLAR_ASSERT_PANIC(P_INCOMPLETE_SPLIT(pageop));
		pageop->btpo_flags &= ~BTP_INCOMPLETE_SPLIT;
		PageSetLSN(page, lsn);
	}

	return action;
}

static XLogRedoAction
polar_bt_restore_meta(XLogReaderState *record, uint8 block_id, Buffer *metabuf)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	Page		metapg;
	BTMetaPageData *md;
	BTPageOpaque pageop;
	xl_btree_metadata *xlrec;
	char	   *ptr;
	Size		len;

	POLAR_INIT_BUFFER_FOR_REDO(record, block_id, metabuf);

	ptr = XLogRecGetBlockData(record, block_id, &len);

	POLAR_ASSERT_PANIC(len == sizeof(xl_btree_metadata));
	Assert(BufferGetBlockNumber(*metabuf) == BTREE_METAPAGE);
	xlrec = (xl_btree_metadata *) ptr;
	metapg = BufferGetPage(*metabuf);

	_bt_pageinit(metapg, BufferGetPageSize(*metabuf));

	md = BTPageGetMeta(metapg);
	md->btm_magic = BTREE_MAGIC;
	md->btm_version = xlrec->version;
	md->btm_root = xlrec->root;
	md->btm_level = xlrec->level;
	md->btm_fastroot = xlrec->fastroot;
	md->btm_fastlevel = xlrec->fastlevel;
	/* Cannot log BTREE_MIN_VERSION index metapage without upgrade */
	POLAR_ASSERT_PANIC(md->btm_version >= BTREE_NOVAC_VERSION);
	md->btm_last_cleanup_num_delpages = xlrec->last_cleanup_num_delpages;
	md->btm_last_cleanup_num_heap_tuples = -1.0;
	md->btm_allequalimage = xlrec->allequalimage;

	pageop = BTPageGetOpaque(metapg);
	pageop->btpo_flags = BTP_META;

	/*
	 * Set pd_lower just past the end of the metadata.  This is essential,
	 * because without doing so, metadata will be lost if xlog.c compresses
	 * the page.
	 */
	((PageHeader) metapg)->pd_lower =
		((char *) md + sizeof(BTMetaPageData)) - (char *) metapg;

	PageSetLSN(metapg, lsn);

	return BLK_NEEDS_REDO;
}

static XLogRedoAction
polar_btree_xlog_insert(bool isleaf, bool ismeta, bool posting, XLogReaderState *record,
						BufferTag *tag, Buffer *buffer)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_btree_insert *xlrec = (xl_btree_insert *) XLogRecGetData(record);
	XLogRedoAction action = BLK_NOTFOUND;
	Page		page;
	BufferTag	tags[3];

	if (!isleaf)
	{
		POLAR_GET_LOG_TAG(record, tags[1], 1);

		/*
		 * Insertion to an internal page finishes an incomplete split at the
		 * child level.  Clear the incomplete-split flag in the child.  Note:
		 * during normal operation, the child and parent pages are locked at
		 * the same time, so that clearing the flag and inserting the downlink
		 * appear atomic to other backends.  We don't bother with that during
		 * replay, because readers don't care about the incomplete-split flag
		 * and there cannot be updates happening.
		 */
		if (BUFFERTAGS_EQUAL(*tag, tags[1]))
			return polar_bt_clear_incomplete_split(record, 1, buffer);
	}

	POLAR_GET_LOG_TAG(record, tags[0], 0);

	if (BUFFERTAGS_EQUAL(*tag, tags[0]))
	{
		action = POLAR_READ_BUFFER_FOR_REDO(record, 0, buffer);

		if (action == BLK_NEEDS_REDO)
		{
			Size		datalen;
			char	   *datapos = XLogRecGetBlockData(record, 0, &datalen);

			page = BufferGetPage(*buffer);

			if (!posting)
			{
				/* Simple retail insertion */
				if (PageAddItem(page, (Item) datapos, datalen, xlrec->offnum,
								false, false) == InvalidOffsetNumber)
				{
					POLAR_LOG_REDO_INFO(page, record);
					elog(PANIC, "btree_insert_redo: failed to add item");
				}
			}
			else
			{
				ItemId		itemid;
				IndexTuple	oposting,
							newitem,
							nposting;
				uint16		postingoff;

				/*
				 * A posting list split occurred during leaf page insertion.
				 * WAL record data will start with an offset number
				 * representing the point in an existing posting list that a
				 * split occurs at.
				 *
				 * Use _bt_swap_posting() to repeat posting list split steps
				 * from primary.  Note that newitem from WAL record is
				 * 'orignewitem', not the final version of newitem that is
				 * actually inserted on page.
				 */
				postingoff = *((uint16 *) datapos);
				datapos += sizeof(uint16);
				datalen -= sizeof(uint16);

				itemid = PageGetItemId(page, OffsetNumberPrev(xlrec->offnum));
				oposting = (IndexTuple) PageGetItem(page, itemid);

				/* Use mutable, aligned newitem copy in _bt_swap_posting() */
				POLAR_ASSERT_PANIC(isleaf && postingoff > 0);
				newitem = CopyIndexTuple((IndexTuple) datapos);
				nposting = _bt_swap_posting(newitem, oposting, postingoff);

				/* Replace existing posting list with post-split version */
				memcpy(oposting, nposting, MAXALIGN(IndexTupleSize(nposting)));

				/* Insert "final" new item (not orignewitem from WAL stream) */
				POLAR_ASSERT_PANIC(IndexTupleSize(newitem) == datalen);
				if (PageAddItem(page, (Item) newitem, datalen, xlrec->offnum,
								false, false) == InvalidOffsetNumber)
					elog(PANIC, "failed to add posting split new item");
			}

			PageSetLSN(page, lsn);
		}

		return action;
	}

	if (ismeta)
	{
		POLAR_GET_LOG_TAG(record, tags[2], 2);

		if (BUFFERTAGS_EQUAL(*tag, tags[2]))
		{
			/*
			 * Note: in normal operation, we'd update the metapage while still
			 * holding lock on the page we inserted into.  But during replay
			 * it's not necessary to hold that lock, since no other index
			 * updates can be happening concurrently, and readers will cope
			 * fine with following an obsolete link from the metapage.
			 */
			return polar_bt_restore_meta(record, 2, buffer);
		}
	}

	return action;
}

static XLogRedoAction
polar_btree_xlog_vacuum(XLogReaderState *record, BufferTag *tag, Buffer *buffer)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_btree_vacuum *xlrec = (xl_btree_vacuum *) XLogRecGetData(record);
	Page		page;
	BTPageOpaque opaque;
	BufferTag	vacuum_tag;
	XLogRedoAction action = BLK_NOTFOUND;

	POLAR_GET_LOG_TAG(record, vacuum_tag, 0);

	if (!BUFFERTAGS_EQUAL(*tag, vacuum_tag))
		return action;

	/*
	 * We need to take a cleanup lock here, just like btvacuumpage(). However,
	 * it isn't necessary to exhaustively get a cleanup lock on every block in
	 * the index during recovery (just getting a cleanup lock on pages with
	 * items to kill suffices).  See nbtree/README for details.
	 */
	action = XLogReadBufferForRedoExtended(record, 0, POLAR_READ_MODE(*buffer), true, buffer);
	if (action == BLK_NEEDS_REDO)
	{
		char	   *ptr = XLogRecGetBlockData(record, 0, NULL);

		page = (Page) BufferGetPage(*buffer);

		if (xlrec->nupdated > 0)
		{
			OffsetNumber *updatedoffsets;
			xl_btree_update *updates;

			updatedoffsets = (OffsetNumber *)
				(ptr + xlrec->ndeleted * sizeof(OffsetNumber));
			updates = (xl_btree_update *) ((char *) updatedoffsets +
										   xlrec->nupdated *
										   sizeof(OffsetNumber));

			btree_xlog_updates(page, updatedoffsets, updates, xlrec->nupdated);
		}

		if (xlrec->ndeleted > 0)
			PageIndexMultiDelete(page, (OffsetNumber *) ptr, xlrec->ndeleted);

		/*
		 * Mark the page as not containing any LP_DEAD items --- see comments
		 * in _bt_delitems_vacuum().
		 */
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);
		opaque->btpo_flags &= ~BTP_HAS_GARBAGE;

		PageSetLSN(page, lsn);
	}

	return action;
}

static XLogRedoAction
polar_btree_xlog_split(bool newitemonleft, XLogReaderState *record,
					   BufferTag *tag, Buffer *buffer)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_btree_split *xlrec = (xl_btree_split *) XLogRecGetData(record);
	bool		isleaf = (xlrec->level == 0);
	Page		rpage;
	BTPageOpaque ropaque;
	char	   *datapos;
	Size		datalen;
	BlockNumber spagenumber;
	BufferTag	tags[4];
	XLogRedoAction action = BLK_NOTFOUND;

	/* leftsib */
	POLAR_GET_LOG_TAG(record, tags[0], 0);
	/* rightsib */
	POLAR_GET_LOG_TAG(record, tags[1], 1);

	if (!XLogRecGetBlockTagExtended(record, 2, NULL, NULL, &spagenumber, NULL))
		spagenumber = P_NONE;

	/*
	 * Clear the incomplete split flag on the appropriate child page one level
	 * down when origpage/buf is an internal page (there must have been
	 * cascading page splits during original execution in the event of an
	 * internal page split).  This is like the corresponding btree_xlog_insert
	 * call for internal pages.  We're not clearing the incomplete split flag
	 * for the current page split here (you can think of this as part of the
	 * insert of newitem that the page split action needs to perform in
	 * passing).
	 *
	 * Like in btree_xlog_insert, this can be done before locking other pages.
	 * We never need to couple cross-level locks in REDO routines.
	 */
	if (!isleaf)
	{
		POLAR_GET_LOG_TAG(record, tags[3], 3);
		if (BUFFERTAGS_EQUAL(tags[3], *tag))
			action = polar_bt_clear_incomplete_split(record, 3, buffer);
	}

	/* Reconstruct right (new) sibling page from scratch */
	if (BUFFERTAGS_EQUAL(*tag, tags[1]))
	{
		POLAR_INIT_BUFFER_FOR_REDO(record, 1, buffer);
		datapos = XLogRecGetBlockData(record, 1, &datalen);
		rpage = (Page) BufferGetPage(*buffer);

		_bt_pageinit(rpage, BufferGetPageSize(*buffer));
		ropaque = (BTPageOpaque) PageGetSpecialPointer(rpage);

		ropaque->btpo_prev = tags[0].blockNum;
		ropaque->btpo_next = spagenumber;
		ropaque->btpo_level = xlrec->level;
		ropaque->btpo_flags = isleaf ? BTP_LEAF : 0;
		ropaque->btpo_cycleid = 0;

		_bt_restore_page(rpage, datapos, datalen);

		PageSetLSN(rpage, lsn);
		action = BLK_NEEDS_REDO;
	}

	if (BUFFERTAGS_EQUAL(*tag, tags[0]))
	{
		action = POLAR_READ_BUFFER_FOR_REDO(record, 0, buffer);
		/* Now reconstruct original page (left half of split) */
		if (action == BLK_NEEDS_REDO)
		{
			/*
			 * To retain the same physical order of the tuples that they had,
			 * we initialize a temporary empty page for the left page and add
			 * all the items to that in item number order.  This mirrors how
			 * _bt_split() works.  Retaining the same physical order makes WAL
			 * consistency checking possible.  See also _bt_restore_page(),
			 * which does the same for the right page.
			 */
			Page		origpage = (Page) BufferGetPage(*buffer);
			BTPageOpaque oopaque = (BTPageOpaque) PageGetSpecialPointer(origpage);
			OffsetNumber off;
			IndexTuple	newitem = NULL,
						left_hikey = NULL,
						nposting = NULL;
			Size		newitemsz = 0,
						left_hikeysz = 0;
			Page		leftpage;
			OffsetNumber leftoff,
						replacepostingoff = InvalidOffsetNumber;

			datapos = XLogRecGetBlockData(record, 0, &datalen);

			if (newitemonleft || xlrec->postingoff != 0)
			{
				newitem = (IndexTuple) datapos;
				newitemsz = MAXALIGN(IndexTupleSize(newitem));
				datapos += newitemsz;
				datalen -= newitemsz;

				if (xlrec->postingoff != 0)
				{
					ItemId		itemid;
					IndexTuple	oposting;

					/* Posting list must be at offset number before new item's */
					replacepostingoff = OffsetNumberPrev(xlrec->newitemoff);

					/* Use mutable, aligned newitem copy in _bt_swap_posting() */
					newitem = CopyIndexTuple(newitem);
					itemid = PageGetItemId(origpage, replacepostingoff);
					oposting = (IndexTuple) PageGetItem(origpage, itemid);
					nposting = _bt_swap_posting(newitem, oposting,
												xlrec->postingoff);
				}
			}

			/*
			 * Extract left hikey and its size.  We assume that 16-bit
			 * alignment is enough to apply IndexTupleSize (since it's
			 * fetching from a uint16 field).
			 */
			left_hikey = (IndexTuple) datapos;
			left_hikeysz = MAXALIGN(IndexTupleSize(left_hikey));
			datapos += left_hikeysz;
			datalen -= left_hikeysz;

			POLAR_ASSERT_PANIC(datalen == 0);

			leftpage = PageGetTempPageCopySpecial(origpage);

			/* Add high key tuple from WAL record to temp page */
			leftoff = P_HIKEY;
			if (PageAddItem(leftpage, (Item) left_hikey, left_hikeysz, P_HIKEY,
							false, false) == InvalidOffsetNumber)
			{
				POLAR_LOG_REDO_INFO(leftpage, record);
				elog(ERROR, "failed to add high key to left page after split");
			}
			leftoff = OffsetNumberNext(leftoff);

			for (off = P_FIRSTDATAKEY(oopaque); off < xlrec->firstrightoff; off++)
			{
				ItemId		itemid;
				Size		itemsz;
				IndexTuple	item;

				/* Add replacement posting list when required */
				if (off == replacepostingoff)
				{
					POLAR_ASSERT_PANIC(newitemonleft ||
									   xlrec->firstrightoff == xlrec->newitemoff);
					if (PageAddItem(leftpage, (Item) nposting,
									MAXALIGN(IndexTupleSize(nposting)), leftoff,
									false, false) == InvalidOffsetNumber)
						elog(ERROR, "failed to add new posting list item to left page after split");
					leftoff = OffsetNumberNext(leftoff);
					continue;	/* don't insert oposting */
				}

				/* add the new item if it was inserted on left page */
				else if (newitemonleft && off == xlrec->newitemoff)
				{
					if (PageAddItem(leftpage, (Item) newitem, newitemsz, leftoff,
									false, false) == InvalidOffsetNumber)
						elog(ERROR, "failed to add new item to left page after split");
					leftoff = OffsetNumberNext(leftoff);
				}

				itemid = PageGetItemId(origpage, off);
				itemsz = ItemIdGetLength(itemid);
				item = (IndexTuple) PageGetItem(origpage, itemid);
				if (PageAddItem(leftpage, (Item) item, itemsz, leftoff,
								false, false) == InvalidOffsetNumber)
					elog(ERROR, "failed to add old item to left page after split");
				leftoff = OffsetNumberNext(leftoff);
			}

			/* cope with possibility that newitem goes at the end */
			if (newitemonleft && off == xlrec->newitemoff)
			{
				if (PageAddItem(leftpage, (Item) newitem, newitemsz, leftoff,
								false, false) == InvalidOffsetNumber)
					elog(ERROR, "failed to add new item to left page after split");
				leftoff = OffsetNumberNext(leftoff);
			}

			PageRestoreTempPage(leftpage, origpage);

			/* Fix opaque fields */
			oopaque->btpo_flags = BTP_INCOMPLETE_SPLIT;
			if (isleaf)
				oopaque->btpo_flags |= BTP_LEAF;
			oopaque->btpo_next = tags[1].blockNum;
			oopaque->btpo_cycleid = 0;

			PageSetLSN(origpage, lsn);
		}
	}

	/* Fix left-link of the page to the right of the new right sibling */
	if (spagenumber != P_NONE)
	{
		POLAR_GET_LOG_TAG(record, tags[2], 2);

		if (BUFFERTAGS_EQUAL(*tag, tags[2]))
		{
			action = POLAR_READ_BUFFER_FOR_REDO(record, 2, buffer);

			if (action == BLK_NEEDS_REDO)
			{
				Page		spage = (Page) BufferGetPage(*buffer);
				BTPageOpaque spageop = (BTPageOpaque) PageGetSpecialPointer(spage);

				spageop->btpo_prev = tags[1].blockNum;

				PageSetLSN(spage, lsn);
			}
		}
	}

	return action;
}

static XLogRedoAction
polar_btree_xlog_delete(XLogReaderState *record, BufferTag *tag, Buffer *buffer)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_btree_delete *xlrec = (xl_btree_delete *) XLogRecGetData(record);
	Page		page;
	BTPageOpaque opaque;
	XLogRedoAction action = BLK_NOTFOUND;
	BufferTag	del_tag;

	POLAR_GET_LOG_TAG(record, del_tag, 0);

	if (!BUFFERTAGS_EQUAL(*tag, del_tag))
		return action;

	/*
	 * We don't need to take a cleanup lock to apply these changes. See
	 * nbtree/README for details.
	 */
	action = POLAR_READ_BUFFER_FOR_REDO(record, 0, buffer);
	if (action == BLK_NEEDS_REDO)
	{
		char	   *ptr = XLogRecGetBlockData(record, 0, NULL);

		page = (Page) BufferGetPage(*buffer);

		if (xlrec->nupdated > 0)
		{
			OffsetNumber *updatedoffsets;
			xl_btree_update *updates;

			updatedoffsets = (OffsetNumber *)
				(ptr + xlrec->ndeleted * sizeof(OffsetNumber));
			updates = (xl_btree_update *) ((char *) updatedoffsets +
										   xlrec->nupdated *
										   sizeof(OffsetNumber));

			btree_xlog_updates(page, updatedoffsets, updates, xlrec->nupdated);
		}

		if (xlrec->ndeleted > 0)
			PageIndexMultiDelete(page, (OffsetNumber *) ptr, xlrec->ndeleted);

		/* Mark the page as not containing any LP_DEAD items */
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);
		opaque->btpo_flags &= ~BTP_HAS_GARBAGE;

		PageSetLSN(page, lsn);
	}

	return action;
}

static XLogRedoAction
polar_btree_xlog_mark_page_halfdead(uint8 info, XLogReaderState *record, BufferTag *tag, Buffer *buffer)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_btree_mark_page_halfdead *xlrec = (xl_btree_mark_page_halfdead *) XLogRecGetData(record);
	Page		page;
	BTPageOpaque pageop;
	IndexTupleData trunctuple;
	BufferTag	tags[2];
	XLogRedoAction action = BLK_NOTFOUND;

	POLAR_GET_LOG_TAG(record, tags[0], 0);
	POLAR_GET_LOG_TAG(record, tags[1], 1);

	if (BUFFERTAGS_EQUAL(*tag, tags[1]))
	{
		/*
		 * In normal operation, we would lock all the pages this WAL record
		 * touches before changing any of them.  In WAL replay, it should be
		 * okay to lock just one page at a time, since no concurrent index
		 * updates can be happening, and readers should not care whether they
		 * arrive at the target page or not (since it's surely empty).
		 */

		/* to-be-deleted subtree's parent page */
		action = POLAR_READ_BUFFER_FOR_REDO(record, 1, buffer);
		if (action == BLK_NEEDS_REDO)
		{
			OffsetNumber poffset;
			ItemId		itemid;
			IndexTuple	itup;
			OffsetNumber nextoffset;
			BlockNumber rightsib;

			page = (Page) BufferGetPage(*buffer);
			pageop = (BTPageOpaque) PageGetSpecialPointer(page);

			poffset = xlrec->poffset;

			nextoffset = OffsetNumberNext(poffset);
			itemid = PageGetItemId(page, nextoffset);
			itup = (IndexTuple) PageGetItem(page, itemid);
			rightsib = BTreeTupleGetDownLink(itup);

			itemid = PageGetItemId(page, poffset);
			itup = (IndexTuple) PageGetItem(page, itemid);
			BTreeTupleSetDownLink(itup, rightsib);
			nextoffset = OffsetNumberNext(poffset);
			PageIndexTupleDelete(page, nextoffset);

			PageSetLSN(page, lsn);
		}
	}

	if (BUFFERTAGS_EQUAL(*tag, tags[0]))
	{
		/* Rewrite the leaf page as a halfdead page */
		POLAR_INIT_BUFFER_FOR_REDO(record, 0, buffer);
		page = (Page) BufferGetPage(*buffer);

		_bt_pageinit(page, BufferGetPageSize(*buffer));
		pageop = (BTPageOpaque) PageGetSpecialPointer(page);

		pageop->btpo_prev = xlrec->leftblk;
		pageop->btpo_next = xlrec->rightblk;
		pageop->btpo_level = 0;
		pageop->btpo_flags = BTP_HALF_DEAD | BTP_LEAF;
		pageop->btpo_cycleid = 0;

		/*
		 * Construct a dummy high key item that points to top parent page
		 * (value is InvalidBlockNumber when the top parent page is the leaf
		 * page itself)
		 */
		MemSet(&trunctuple, 0, sizeof(IndexTupleData));
		trunctuple.t_info = sizeof(IndexTupleData);
		BTreeTupleSetTopParent(&trunctuple, xlrec->topparent);

		if (PageAddItem(page, (Item) &trunctuple, sizeof(IndexTupleData), P_HIKEY,
						false, false) == InvalidOffsetNumber)
			elog(ERROR, "could not add dummy high key to half-dead page");

		PageSetLSN(page, lsn);
		action = BLK_NEEDS_REDO;
	}

	return action;
}

static XLogRedoAction
polar_btree_xlog_unlink_page(uint8 info, XLogReaderState *record, BufferTag *tag, Buffer *buffer)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_btree_unlink_page *xlrec = (xl_btree_unlink_page *) XLogRecGetData(record);
	BlockNumber leftsib;
	BlockNumber rightsib;
	uint32		level;
	bool		isleaf;
	FullTransactionId safexid;
	Page		page;
	BTPageOpaque pageop;
	BufferTag	tags[5];
	XLogRedoAction action = BLK_NOTFOUND;

	leftsib = xlrec->leftsib;
	rightsib = xlrec->rightsib;
	level = xlrec->level;
	isleaf = (level == 0);
	safexid = xlrec->safexid;

	/* No leaftopparent for level 0 (leaf page) or level 1 target */
	POLAR_ASSERT_PANIC(!BlockNumberIsValid(xlrec->leaftopparent) || level > 1);

	/*
	 * In normal operation, we would lock all the pages this WAL record
	 * touches before changing any of them.  In WAL replay, we at least lock
	 * the pages in the same standard left-to-right order (leftsib, target,
	 * rightsib), and don't release the sibling locks until the target is
	 * marked deleted.
	 */

	/* Fix right-link of left sibling, if any */
	if (leftsib != P_NONE)
	{
		POLAR_GET_LOG_TAG(record, tags[1], 1);
		if (BUFFERTAGS_EQUAL(*tag, tags[1]))
		{
			action = POLAR_READ_BUFFER_FOR_REDO(record, 1, buffer);
			if (action == BLK_NEEDS_REDO)
			{
				page = (Page) BufferGetPage(*buffer);
				pageop = BTPageGetOpaque(page);
				pageop->btpo_next = rightsib;

				PageSetLSN(page, lsn);
			}
		}
	}

	POLAR_GET_LOG_TAG(record, tags[0], 0);
	if (BUFFERTAGS_EQUAL(*tag, tags[0]))
	{
		/* Rewrite target page as empty deleted page */
		POLAR_INIT_BUFFER_FOR_REDO(record, 0, buffer);
		page = (Page) BufferGetPage(*buffer);

		_bt_pageinit(page, BufferGetPageSize(*buffer));
		pageop = BTPageGetOpaque(page);

		pageop->btpo_prev = leftsib;
		pageop->btpo_next = rightsib;
		pageop->btpo_level = level;
		BTPageSetDeleted(page, safexid);
		if (isleaf)
			pageop->btpo_flags |= BTP_LEAF;
		pageop->btpo_cycleid = 0;

		PageSetLSN(page, lsn);
		action = BLK_NEEDS_REDO;
	}

	POLAR_GET_LOG_TAG(record, tags[2], 2);
	if (BUFFERTAGS_EQUAL(*tag, tags[2]))
	{
		/* Fix left-link of right sibling */
		action = POLAR_READ_BUFFER_FOR_REDO(record, 2, buffer);
		if (action == BLK_NEEDS_REDO)
		{
			page = (Page) BufferGetPage(*buffer);
			pageop = BTPageGetOpaque(page);
			pageop->btpo_prev = leftsib;

			PageSetLSN(page, lsn);
		}
	}

	/*
	 * If we deleted a parent of the targeted leaf page, instead of the leaf
	 * itself, update the leaf to point to the next remaining child in the
	 * to-be-deleted subtree
	 */
	if (XLogRecHasBlockRef(record, 3))
	{
		POLAR_GET_LOG_TAG(record, tags[3], 3);
		if (BUFFERTAGS_EQUAL(*tag, tags[3]))
		{
			/*
			 * There is no real data on the page, so we just re-create it from
			 * scratch using the information from the WAL record.
			 *
			 * Note that we don't end up here when the target page is also the
			 * leafbuf page.  There is no need to add a dummy hikey item with
			 * a top parent link when deleting leafbuf because it's the last
			 * page we'll delete in the subtree undergoing deletion.
			 */
			IndexTupleData trunctuple;

			POLAR_ASSERT_PANIC(!isleaf);

			POLAR_INIT_BUFFER_FOR_REDO(record, 3, buffer);
			page = (Page) BufferGetPage(*buffer);

			_bt_pageinit(page, BufferGetPageSize(*buffer));
			pageop = BTPageGetOpaque(page);

			pageop->btpo_flags = BTP_HALF_DEAD | BTP_LEAF;
			pageop->btpo_prev = xlrec->leafleftsib;
			pageop->btpo_next = xlrec->leafrightsib;
			pageop->btpo_level = 0;
			pageop->btpo_cycleid = 0;

			/* Add a dummy hikey item */
			MemSet(&trunctuple, 0, sizeof(IndexTupleData));
			trunctuple.t_info = sizeof(IndexTupleData);
			BTreeTupleSetTopParent(&trunctuple, xlrec->leaftopparent);

			if (PageAddItem(page, (Item) &trunctuple, sizeof(IndexTupleData), P_HIKEY,
							false, false) == InvalidOffsetNumber)
				elog(ERROR, "could not add dummy high key to half-dead page");

			PageSetLSN(page, lsn);
			action = BLK_NEEDS_REDO;
		}
	}

	/* Update metapage if needed */
	if (info == XLOG_BTREE_UNLINK_PAGE_META)
	{
		POLAR_GET_LOG_TAG(record, tags[4], 4);
		if (BUFFERTAGS_EQUAL(*tag, tags[4]))
			action = polar_bt_restore_meta(record, 4, buffer);
	}

	return action;
}

static XLogRedoAction
polar_btree_xlog_newroot(XLogReaderState *record, BufferTag *tag, Buffer *buffer)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_btree_newroot *xlrec = (xl_btree_newroot *) XLogRecGetData(record);
	Page		page;
	BTPageOpaque pageop;
	char	   *ptr;
	Size		len;
	BufferTag	tags[3];
	XLogRedoAction action = BLK_NOTFOUND;

	POLAR_GET_LOG_TAG(record, tags[0], 0);
	if (BUFFERTAGS_EQUAL(*tag, tags[0]))
	{
		POLAR_INIT_BUFFER_FOR_REDO(record, 0, buffer);
		page = (Page) BufferGetPage(*buffer);

		_bt_pageinit(page, BufferGetPageSize(*buffer));
		pageop = BTPageGetOpaque(page);

		pageop->btpo_flags = BTP_ROOT;
		pageop->btpo_prev = pageop->btpo_next = P_NONE;
		pageop->btpo_level = xlrec->level;
		if (xlrec->level == 0)
			pageop->btpo_flags |= BTP_LEAF;
		pageop->btpo_cycleid = 0;

		if (xlrec->level > 0)
		{
			ptr = XLogRecGetBlockData(record, 0, &len);
			_bt_restore_page(page, ptr, len);
		}

		PageSetLSN(page, lsn);
		action = BLK_NEEDS_REDO;
	}

	if (xlrec->level > 0)
	{
		POLAR_GET_LOG_TAG(record, tags[1], 1);
		if (BUFFERTAGS_EQUAL(*tag, tags[1]))
		{
			/* Clear the incomplete-split flag in left child */
			action = polar_bt_clear_incomplete_split(record, 1, buffer);
		}
	}

	POLAR_GET_LOG_TAG(record, tags[2], 2);
	if (BUFFERTAGS_EQUAL(*tag, tags[2]))
		action = polar_bt_restore_meta(record, 2, buffer);

	return action;
}

static XLogRedoAction
polar_btree_xlog_dedup(XLogReaderState *record, BufferTag *tag, Buffer *buffer)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_btree_dedup *xlrec = (xl_btree_dedup *) XLogRecGetData(record);
	XLogRedoAction action = BLK_NOTFOUND;
	BufferTag	dedup_tag;

	POLAR_GET_LOG_TAG(record, dedup_tag, 0);
	if (BUFFERTAGS_EQUAL(*tag, dedup_tag))
	{
		char	   *ptr = XLogRecGetBlockData(record, 0, NULL);
		Page		page;
		BTPageOpaque opaque;
		OffsetNumber offnum,
					minoff,
					maxoff;
		BTDedupState state;
		BTDedupInterval *intervals;
		Page		newpage;

		action = POLAR_READ_BUFFER_FOR_REDO(record, 0, buffer);
		if (action == BLK_NEEDS_REDO)
		{
			page = (Page) BufferGetPage(*buffer);
			opaque = (BTPageOpaque) PageGetSpecialPointer(page);

			state = (BTDedupState) palloc(sizeof(BTDedupStateData));
			state->deduplicate = true;	/* unused */
			state->nmaxitems = 0;	/* unused */
			/* Conservatively use larger maxpostingsize than primary */
			state->maxpostingsize = BTMaxItemSize(page);
			state->base = NULL;
			state->baseoff = InvalidOffsetNumber;
			state->basetupsize = 0;
			state->htids = palloc(state->maxpostingsize);
			state->nhtids = 0;
			state->nitems = 0;
			state->phystupsize = 0;
			state->nintervals = 0;

			minoff = P_FIRSTDATAKEY(opaque);
			maxoff = PageGetMaxOffsetNumber(page);
			newpage = PageGetTempPageCopySpecial(page);

			if (!P_RIGHTMOST(opaque))
			{
				ItemId		itemid = PageGetItemId(page, P_HIKEY);
				Size		itemsz = ItemIdGetLength(itemid);
				IndexTuple	item = (IndexTuple) PageGetItem(page, itemid);

				if (PageAddItem(newpage, (Item) item, itemsz, P_HIKEY,
								false, false) == InvalidOffsetNumber)
					elog(ERROR, "deduplication failed to add highkey");
			}

			intervals = (BTDedupInterval *) ptr;
			for (offnum = minoff;
				 offnum <= maxoff;
				 offnum = OffsetNumberNext(offnum))
			{
				ItemId		itemid = PageGetItemId(page, offnum);
				IndexTuple	itup = (IndexTuple) PageGetItem(page, itemid);

				if (offnum == minoff)
					_bt_dedup_start_pending(state, itup, offnum);
				else if (state->nintervals < xlrec->nintervals &&
						 state->baseoff == intervals[state->nintervals].baseoff &&
						 state->nitems < intervals[state->nintervals].nitems)
				{
					if (!_bt_dedup_save_htid(state, itup))
						elog(ERROR, "deduplication failed to add heap tid to pending posting list");
				}
				else
				{
					_bt_dedup_finish_pending(newpage, state);
					_bt_dedup_start_pending(state, itup, offnum);
				}
			}

			_bt_dedup_finish_pending(newpage, state);
			POLAR_ASSERT_PANIC(state->nintervals == xlrec->nintervals);
			Assert(memcmp(state->intervals, intervals,
						  state->nintervals * sizeof(BTDedupInterval)) == 0);

			if (P_HAS_GARBAGE(opaque))
			{
				BTPageOpaque nopaque = (BTPageOpaque) PageGetSpecialPointer(newpage);

				nopaque->btpo_flags &= ~BTP_HAS_GARBAGE;
			}

			PageRestoreTempPage(newpage, page);
			PageSetLSN(page, lsn);
		}
	}

	return action;
}

void
polar_btree_idx_save(polar_logindex_redo_ctl_t instance, XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_BTREE_INSERT_LEAF:
			polar_btree_xlog_insert_save(instance, true, false, record);
			break;

		case XLOG_BTREE_INSERT_UPPER:
			polar_btree_xlog_insert_save(instance, false, false, record);
			break;

		case XLOG_BTREE_INSERT_META:
			polar_btree_xlog_insert_save(instance, false, true, record);
			break;

		case XLOG_BTREE_INSERT_POST:
			polar_btree_xlog_insert_save(instance, true, false, record);
			break;

		case XLOG_BTREE_DEDUP:
			polar_logindex_save_block(instance, record, 0);
			break;

		case XLOG_BTREE_SPLIT_L:
		case XLOG_BTREE_SPLIT_R:
			polar_btree_xlog_split_save(instance, record);
			break;

		case XLOG_BTREE_VACUUM:
			polar_logindex_save_block(instance, record, 0);
			break;

		case XLOG_BTREE_DELETE:
			polar_logindex_save_block(instance, record, 0);
			break;

		case XLOG_BTREE_MARK_PAGE_HALFDEAD:
			polar_logindex_save_block(instance, record, 1);
			polar_logindex_save_block(instance, record, 0);
			break;

		case XLOG_BTREE_UNLINK_PAGE:
		case XLOG_BTREE_UNLINK_PAGE_META:
			polar_btree_xlog_unlink_page_save(instance, info, record);
			break;

		case XLOG_BTREE_NEWROOT:
			polar_btree_xlog_newroot_save(instance, record);
			break;

		case XLOG_BTREE_REUSE_PAGE:
			break;

		case XLOG_BTREE_META_CLEANUP:
			polar_logindex_save_block(instance, record, 0);
			break;

		default:
			elog(PANIC, "polar_btree_idx_save: unknown op code %u", info);
			break;
	}
}

bool
polar_btree_idx_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_BTREE_INSERT_LEAF:
			polar_btree_xlog_insert_parse(instance, true, false, record);
			break;

		case XLOG_BTREE_INSERT_UPPER:
			polar_btree_xlog_insert_parse(instance, false, false, record);
			break;

		case XLOG_BTREE_INSERT_META:
			polar_btree_xlog_insert_parse(instance, false, true, record);
			break;

		case XLOG_BTREE_INSERT_POST:
			polar_btree_xlog_insert_parse(instance, true, false, record);
			break;

		case XLOG_BTREE_DEDUP:
			polar_logindex_redo_parse(instance, record, 0);
			break;

		case XLOG_BTREE_SPLIT_L:
		case XLOG_BTREE_SPLIT_R:
			polar_btree_xlog_split_parse(instance, record);
			break;

		case XLOG_BTREE_VACUUM:
			polar_logindex_cleanup_parse(instance, record, 0);
			break;

		case XLOG_BTREE_DELETE:
			polar_btree_xlog_delete_parse(instance, record);
			break;

		case XLOG_BTREE_MARK_PAGE_HALFDEAD:
			polar_logindex_redo_parse(instance, record, 1);
			polar_logindex_redo_parse(instance, record, 0);
			break;

		case XLOG_BTREE_UNLINK_PAGE:
		case XLOG_BTREE_UNLINK_PAGE_META:
			polar_btree_xlog_unlink_page_parse(instance, info, record);
			break;

		case XLOG_BTREE_NEWROOT:
			polar_btree_xlog_newroot_parse(instance, record);
			break;

		case XLOG_BTREE_REUSE_PAGE:
			polar_btree_xlog_reuse_page_parse(record);
			break;

		case XLOG_BTREE_META_CLEANUP:
			polar_logindex_redo_parse(instance, record, 0);
			break;

		default:
			elog(PANIC, "polar_btree_idx_parse: unknown op code %u", info);
			break;
	}

	return true;
}

XLogRedoAction
polar_btree_idx_redo(polar_logindex_redo_ctl_t instance, XLogReaderState *record, BufferTag *tag, Buffer *buffer)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	XLogRedoAction action = BLK_NOTFOUND;

	switch (info)
	{
		case XLOG_BTREE_INSERT_LEAF:
			action = polar_btree_xlog_insert(true, false, false, record, tag, buffer);
			break;

		case XLOG_BTREE_INSERT_UPPER:
			action = polar_btree_xlog_insert(false, false, false, record, tag, buffer);
			break;

		case XLOG_BTREE_INSERT_META:
			action = polar_btree_xlog_insert(false, true, false, record, tag, buffer);
			break;

		case XLOG_BTREE_INSERT_POST:
			action = polar_btree_xlog_insert(true, false, true, record, tag, buffer);
			break;

		case XLOG_BTREE_SPLIT_L:
			action = polar_btree_xlog_split(true, record, tag, buffer);
			break;

		case XLOG_BTREE_SPLIT_R:
			action = polar_btree_xlog_split(false, record, tag, buffer);
			break;

		case XLOG_BTREE_DEDUP:
			action = polar_btree_xlog_dedup(record, tag, buffer);
			break;

		case XLOG_BTREE_VACUUM:
			action = polar_btree_xlog_vacuum(record, tag, buffer);
			break;

		case XLOG_BTREE_DELETE:
			action = polar_btree_xlog_delete(record, tag, buffer);
			break;

		case XLOG_BTREE_MARK_PAGE_HALFDEAD:
			action = polar_btree_xlog_mark_page_halfdead(info, record, tag, buffer);
			break;

		case XLOG_BTREE_UNLINK_PAGE:
		case XLOG_BTREE_UNLINK_PAGE_META:
			action = polar_btree_xlog_unlink_page(info, record, tag, buffer);
			break;

		case XLOG_BTREE_NEWROOT:
			action = polar_btree_xlog_newroot(record, tag, buffer);
			break;

		case XLOG_BTREE_REUSE_PAGE:
			/* Never go to here */
			POLAR_ASSERT_PANIC(0);
			break;

		case XLOG_BTREE_META_CLEANUP:
			action = polar_bt_restore_meta(record, 0, buffer);
			break;

		default:
			elog(PANIC, "polar_btree_idx_redo: unknown op code %u", info);
			break;
	}

	return action;
}
