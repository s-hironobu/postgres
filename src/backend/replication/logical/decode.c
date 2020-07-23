/* -------------------------------------------------------------------------
 *
 * decode.c
 *		This module decodes WAL records read using xlogreader.h's APIs for the
 *		purpose of logical decoding by passing information to the
 *		reorderbuffer module (containing the actual changes) and to the
 *		snapbuild module to build a fitting catalog snapshot (to be able to
 *		properly decode the changes in the reorderbuffer).
 *
 * NOTE:
 *		This basically tries to handle all low level xlog stuff for
 *		reorderbuffer.c and snapbuild.c. There's some minor leakage where a
 *		specific record's struct is used to pass data along, but those just
 *		happen to contain the right amount of data in a convenient
 *		format. There isn't and shouldn't be much intelligence about the
 *		contents of records in here except turning them into a more usable
 *		format.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/decode.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/heapam_xlog.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "access/xlogutils.h"
#include "access/xlogreader.h"
#include "access/xlogrecord.h"
#include "access/zheapam_xlog.h"

#include "catalog/pg_control.h"

#include "replication/decode.h"
#include "replication/logical.h"
#include "replication/message.h"
#include "replication/reorderbuffer.h"
#include "replication/origin.h"
#include "replication/snapbuild.h"

#include "storage/standby.h"

typedef struct XLogRecordBuffer
{
	XLogRecPtr	origptr;
	XLogRecPtr	endptr;
	XLogReaderState *record;
} XLogRecordBuffer;

/* RMGR Handlers */
static void DecodeXLogOp(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeHeapOp(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeHeap2Op(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeZHeapOp(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeZHeap2Op(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeXactOp(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeStandbyOp(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeLogicalMsgOp(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);

/* individual record(group)'s handlers */
static void DecodeInsert(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeZHeapInsert(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeUpdate(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeZHeapUpdate(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeDelete(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeZHeapDelete(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeTruncate(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeMultiInsert(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeZHeapMultiInsert(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeSpecConfirm(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeZHeapSpecConfirm(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);

static void DecodeCommit(LogicalDecodingContext *ctx, XLogRecordBuffer *buf,
						 xl_xact_parsed_commit *parsed, TransactionId xid);
static void DecodeAbort(LogicalDecodingContext *ctx, XLogRecordBuffer *buf,
						xl_xact_parsed_abort *parsed, TransactionId xid);

/* common functions to decode tuples */
static void DecodeXLogTuple(char *data, Size len, ReorderBufferTupleBuf *tup);
static void DecodeXLogZHeapTuple(char *data, Size len,
								 ReorderBufferTupleBuf *tuple);

/*
 * Take every XLogReadRecord()ed record and perform the actions required to
 * decode it using the output plugin already setup in the logical decoding
 * context.
 *
 * NB: Note that every record's xid needs to be processed by reorderbuffer
 * (xids contained in the content of records are not relevant for this rule).
 * That means that for records which'd otherwise not go through the
 * reorderbuffer ReorderBufferProcessXid() has to be called. We don't want to
 * call ReorderBufferProcessXid for each record type by default, because
 * e.g. empty xacts can be handled more efficiently if there's no previous
 * state for them.
 *
 * We also support the ability to fast forward thru records, skipping some
 * record types completely - see individual record types for details.
 */
void
LogicalDecodingProcessRecord(LogicalDecodingContext *ctx, XLogReaderState *record)
{
	XLogRecordBuffer buf;

	buf.origptr = ctx->reader->ReadRecPtr;
	buf.endptr = ctx->reader->EndRecPtr;
	buf.record = record;

	/* cast so we get a warning when new rmgrs are added */
	switch ((RmgrIds) XLogRecGetRmid(record))
	{
			/*
			 * Rmgrs we care about for logical decoding. Add new rmgrs in
			 * rmgrlist.h's order.
			 */
		case RM_XLOG_ID:
			DecodeXLogOp(ctx, &buf);
			break;

		case RM_XACT_ID:
			DecodeXactOp(ctx, &buf);
			break;

		case RM_STANDBY_ID:
			DecodeStandbyOp(ctx, &buf);
			break;

		case RM_HEAP2_ID:
			DecodeHeap2Op(ctx, &buf);
			break;

		case RM_HEAP_ID:
			DecodeHeapOp(ctx, &buf);
			break;

		case RM_LOGICALMSG_ID:
			DecodeLogicalMsgOp(ctx, &buf);
			break;

			/*
			 * Rmgrs irrelevant for logical decoding; they describe stuff not
			 * represented in logical decoding. Add new rmgrs in rmgrlist.h's
			 * order.
			 */
		case RM_SMGR_ID:
		case RM_CLOG_ID:
		case RM_DBASE_ID:
		case RM_TBLSPC_ID:
		case RM_MULTIXACT_ID:
		case RM_RELMAP_ID:
		case RM_BTREE_ID:
		case RM_HASH_ID:
		case RM_GIN_ID:
		case RM_GIST_ID:
		case RM_SEQ_ID:
		case RM_SPGIST_ID:
		case RM_BRIN_ID:
		case RM_COMMIT_TS_ID:
		case RM_REPLORIGIN_ID:
		case RM_GENERIC_ID:
		case RM_UNDOLOG_ID:
		case RM_UNDOACTION_ID:
			/* just deal with xid, and done */
			ReorderBufferProcessXid(ctx->reorder, XLogRecGetXid(record),
									buf.origptr);
			break;
		case RM_ZHEAP_ID:
			DecodeZHeapOp(ctx, &buf);
			break;

		case RM_ZHEAP2_ID:
			DecodeZHeap2Op(ctx, &buf);
			break;

			/*
			 * The following rgmrs do not any carry useful information in
			 * terms of logical decoding. Specifically for RM_ZUNDO_ID: if any
			 * transaction / subtransaction aborts, it's changes simply will
			 * not get replayed.
			 */
		case RM_ZUNDO_ID:
		case RM_TPD_ID:
			break;
		case RM_NEXT_ID:
			elog(ERROR, "unexpected RM_NEXT_ID rmgr_id: %u", (RmgrIds) XLogRecGetRmid(buf.record));
	}
}

/*
 * Handle rmgr XLOG_ID records for DecodeRecordIntoReorderBuffer().
 */
static void
DecodeXLogOp(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	SnapBuild  *builder = ctx->snapshot_builder;
	uint8		info = XLogRecGetInfo(buf->record) & ~XLR_INFO_MASK;

	ReorderBufferProcessXid(ctx->reorder, XLogRecGetXid(buf->record),
							buf->origptr);

	switch (info)
	{
			/* this is also used in END_OF_RECOVERY checkpoints */
		case XLOG_CHECKPOINT_SHUTDOWN:
		case XLOG_END_OF_RECOVERY:
			SnapBuildSerializationPoint(builder, buf->origptr);

			break;
		case XLOG_CHECKPOINT_ONLINE:

			/*
			 * a RUNNING_XACTS record will have been logged near to this, we
			 * can restart from there.
			 */
			break;
		case XLOG_NOOP:
		case XLOG_NEXTOID:
		case XLOG_SWITCH:
		case XLOG_BACKUP_END:
		case XLOG_PARAMETER_CHANGE:
		case XLOG_RESTORE_POINT:
		case XLOG_FPW_CHANGE:
		case XLOG_FPI_FOR_HINT:
		case XLOG_FPI:
			break;
		default:
			elog(ERROR, "unexpected RM_XLOG_ID record type: %u", info);
	}
}

/*
 * Handle rmgr XACT_ID records for DecodeRecordIntoReorderBuffer().
 */
static void
DecodeXactOp(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	SnapBuild  *builder = ctx->snapshot_builder;
	ReorderBuffer *reorder = ctx->reorder;
	XLogReaderState *r = buf->record;
	uint8		info = XLogRecGetInfo(r) & XLOG_XACT_OPMASK;

	/*
	 * No point in doing anything yet, data could not be decoded anyway. It's
	 * ok not to call ReorderBufferProcessXid() in that case, except in the
	 * assignment case there'll not be any later records with the same xid;
	 * and in the assignment case we'll not decode those xacts.
	 */
	if (SnapBuildCurrentState(builder) < SNAPBUILD_FULL_SNAPSHOT)
		return;

	switch (info)
	{
		case XLOG_XACT_COMMIT:
		case XLOG_XACT_COMMIT_PREPARED:
			{
				xl_xact_commit *xlrec;
				xl_xact_parsed_commit parsed;
				TransactionId xid;

				xlrec = (xl_xact_commit *) XLogRecGetData(r);
				ParseCommitRecord(XLogRecGetInfo(buf->record), xlrec, &parsed);

				if (!TransactionIdIsValid(parsed.twophase_xid))
					xid = XLogRecGetXid(r);
				else
					xid = parsed.twophase_xid;

				DecodeCommit(ctx, buf, &parsed, xid);
				break;
			}
		case XLOG_XACT_ABORT:
		case XLOG_XACT_ABORT_PREPARED:
			{
				xl_xact_abort *xlrec;
				xl_xact_parsed_abort parsed;
				TransactionId xid;

				xlrec = (xl_xact_abort *) XLogRecGetData(r);
				ParseAbortRecord(XLogRecGetInfo(buf->record), xlrec, &parsed);

				if (!TransactionIdIsValid(parsed.twophase_xid))
					xid = XLogRecGetXid(r);
				else
					xid = parsed.twophase_xid;

				DecodeAbort(ctx, buf, &parsed, xid);
				break;
			}
		case XLOG_XACT_ASSIGNMENT:
			{
				xl_xact_assignment *xlrec;
				int			i;
				TransactionId *sub_xid;

				xlrec = (xl_xact_assignment *) XLogRecGetData(r);

				sub_xid = &xlrec->xsub[0];

				for (i = 0; i < xlrec->nsubxacts; i++)
				{
					ReorderBufferAssignChild(reorder, xlrec->xtop,
											 *(sub_xid++), buf->origptr);
				}
				break;
			}
		case XLOG_XACT_PREPARE:

			/*
			 * Currently decoding ignores PREPARE TRANSACTION and will just
			 * decode the transaction when the COMMIT PREPARED is sent or
			 * throw away the transaction's contents when a ROLLBACK PREPARED
			 * is received. In the future we could add code to expose prepared
			 * transactions in the changestream allowing for a kind of
			 * distributed 2PC.
			 */
			ReorderBufferProcessXid(reorder, XLogRecGetXid(r), buf->origptr);
			break;
		default:
			elog(ERROR, "unexpected RM_XACT_ID record type: %u", info);
	}
}

/*
 * Handle rmgr STANDBY_ID records for DecodeRecordIntoReorderBuffer().
 */
static void
DecodeStandbyOp(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	SnapBuild  *builder = ctx->snapshot_builder;
	XLogReaderState *r = buf->record;
	uint8		info = XLogRecGetInfo(r) & ~XLR_INFO_MASK;

	ReorderBufferProcessXid(ctx->reorder, XLogRecGetXid(r), buf->origptr);

	switch (info)
	{
		case XLOG_RUNNING_XACTS:
			{
				xl_running_xacts *running = (xl_running_xacts *) XLogRecGetData(r);

				SnapBuildProcessRunningXacts(builder, buf->origptr, running);

				/*
				 * Abort all transactions that we keep track of, that are
				 * older than the record's oldestRunningXid. This is the most
				 * convenient spot for doing so since, in contrast to shutdown
				 * or end-of-recovery checkpoints, we have information about
				 * all running transactions which includes prepared ones,
				 * while shutdown checkpoints just know that no non-prepared
				 * transactions are in progress.
				 */
				ReorderBufferAbortOld(ctx->reorder, running->oldestRunningXid);
			}
			break;
		case XLOG_STANDBY_LOCK:
			break;
		case XLOG_INVALIDATIONS:
			{
				xl_invalidations *invalidations =
				(xl_invalidations *) XLogRecGetData(r);

				if (!ctx->fast_forward)
					ReorderBufferImmediateInvalidation(ctx->reorder,
													   invalidations->nmsgs,
													   invalidations->msgs);
			}
			break;
		default:
			elog(ERROR, "unexpected RM_STANDBY_ID record type: %u", info);
	}
}

/*
 * Handle rmgr HEAP2_ID records for DecodeRecordIntoReorderBuffer().
 */
static void
DecodeHeap2Op(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	uint8		info = XLogRecGetInfo(buf->record) & XLOG_HEAP_OPMASK;
	TransactionId xid = XLogRecGetXid(buf->record);
	SnapBuild  *builder = ctx->snapshot_builder;

	ReorderBufferProcessXid(ctx->reorder, xid, buf->origptr);

	/*
	 * If we don't have snapshot or we are just fast-forwarding, there is no
	 * point in decoding changes.
	 */
	if (SnapBuildCurrentState(builder) < SNAPBUILD_FULL_SNAPSHOT ||
		ctx->fast_forward)
		return;

	switch (info)
	{
		case XLOG_HEAP2_MULTI_INSERT:
			if (!ctx->fast_forward &&
				SnapBuildProcessChange(builder, xid, buf->origptr))
				DecodeMultiInsert(ctx, buf);
			break;
		case XLOG_HEAP2_NEW_CID:
			{
				xl_heap_new_cid *xlrec;

				xlrec = (xl_heap_new_cid *) XLogRecGetData(buf->record);
				SnapBuildProcessNewCid(builder, xid, buf->origptr, xlrec);

				break;
			}
		case XLOG_HEAP2_REWRITE:

			/*
			 * Although these records only exist to serve the needs of logical
			 * decoding, all the work happens as part of crash or archive
			 * recovery, so we don't need to do anything here.
			 */
			break;

			/*
			 * Everything else here is just low level physical stuff we're not
			 * interested in.
			 */
		case XLOG_HEAP2_FREEZE_PAGE:
		case XLOG_HEAP2_CLEAN:
		case XLOG_HEAP2_CLEANUP_INFO:
		case XLOG_HEAP2_VISIBLE:
		case XLOG_HEAP2_LOCK_UPDATED:
			break;
		default:
			elog(ERROR, "unexpected RM_HEAP2_ID record type: %u", info);
	}
}

/*
 * Handle rmgr ZHEAP_IDrecords for DecodeRecordIntoReorderBuffer().
 */
static void
DecodeHeapOp(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	uint8		info = XLogRecGetInfo(buf->record) & XLOG_HEAP_OPMASK;
	TransactionId xid = XLogRecGetXid(buf->record);
	SnapBuild  *builder = ctx->snapshot_builder;

	ReorderBufferProcessXid(ctx->reorder, xid, buf->origptr);

	/*
	 * If we don't have snapshot or we are just fast-forwarding, there is no
	 * point in decoding data changes.
	 */
	if (SnapBuildCurrentState(builder) < SNAPBUILD_FULL_SNAPSHOT ||
		ctx->fast_forward)
		return;

	switch (info)
	{
		case XLOG_HEAP_INSERT:
			if (SnapBuildProcessChange(builder, xid, buf->origptr))
				DecodeInsert(ctx, buf);
			break;

			/*
			 * Treat HOT update as normal updates. There is no useful
			 * information in the fact that we could make it a HOT update
			 * locally and the WAL layout is compatible.
			 */
		case XLOG_HEAP_HOT_UPDATE:
		case XLOG_HEAP_UPDATE:
			if (SnapBuildProcessChange(builder, xid, buf->origptr))
				DecodeUpdate(ctx, buf);
			break;

		case XLOG_HEAP_DELETE:
			if (SnapBuildProcessChange(builder, xid, buf->origptr))
				DecodeDelete(ctx, buf);
			break;

		case XLOG_HEAP_TRUNCATE:
			if (SnapBuildProcessChange(builder, xid, buf->origptr))
				DecodeTruncate(ctx, buf);
			break;

		case XLOG_HEAP_INPLACE:

			/*
			 * Inplace updates are only ever performed on catalog tuples and
			 * can, per definition, not change tuple visibility.  Since we
			 * don't decode catalog tuples, we're not interested in the
			 * record's contents.
			 *
			 * In-place updates can be used either by XID-bearing transactions
			 * (e.g.  in CREATE INDEX CONCURRENTLY) or by XID-less
			 * transactions (e.g.  VACUUM).  In the former case, the commit
			 * record will include cache invalidations, so we mark the
			 * transaction as catalog modifying here. Currently that's
			 * redundant because the commit will do that as well, but once we
			 * support decoding in-progress relations, this will be important.
			 */
			if (!TransactionIdIsValid(xid))
				break;

			SnapBuildProcessChange(builder, xid, buf->origptr);
			ReorderBufferXidSetCatalogChanges(ctx->reorder, xid, buf->origptr);
			break;

		case XLOG_HEAP_CONFIRM:
			if (SnapBuildProcessChange(builder, xid, buf->origptr))
				DecodeSpecConfirm(ctx, buf);
			break;

		case XLOG_HEAP_LOCK:
			/* we don't care about row level locks for now */
			break;

		default:
			elog(ERROR, "unexpected RM_HEAP_ID record type: %u", info);
			break;
	}
}

/*
 * Handle rmgr ZHEAP_ID records for DecodeRecordIntoReorderBuffer().
 */
static void
DecodeZHeapOp(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	uint8		info = XLogRecGetInfo(buf->record) & XLOG_ZHEAP_OPMASK;
	TransactionId xid = XLogRecGetXid(buf->record);
	SnapBuild  *builder = ctx->snapshot_builder;

	ReorderBufferProcessXid(ctx->reorder, xid, buf->origptr);

	/*
	 * If we don't have snapshot or we are just fast-forwarding, there is no
	 * point in decoding data changes.
	 */
	if (SnapBuildCurrentState(builder) < SNAPBUILD_FULL_SNAPSHOT ||
		ctx->fast_forward)
		return;

	switch (info)
	{
		case XLOG_ZHEAP_INSERT:
			if (SnapBuildProcessChange(builder, xid, buf->origptr))
				DecodeZHeapInsert(ctx, buf);
			break;

		case XLOG_ZHEAP_DELETE:
			if (SnapBuildProcessChange(builder, xid, buf->origptr))
				DecodeZHeapDelete(ctx, buf);
			break;

		case XLOG_ZHEAP_UPDATE:
			if (SnapBuildProcessChange(builder, xid, buf->origptr))
				DecodeZHeapUpdate(ctx, buf);
			break;

		case XLOG_ZHEAP_MULTI_INSERT:
			if (SnapBuildProcessChange(builder, xid, buf->origptr))
				DecodeZHeapMultiInsert(ctx, buf);
			break;

		/* Low-level stuff not interesting for logical decoding. */
		case XLOG_ZHEAP_FREEZE_XACT_SLOT:
		case XLOG_ZHEAP_INVALID_XACT_SLOT:
		case XLOG_ZHEAP_LOCK:
		case XLOG_ZHEAP_CLEAN:
			break;

		default:
			elog(ERROR, "unexpected RM_HEAP_ID record type: %u", info);
			break;
	}
}

/*
 * Handle rmgr ZHEAP2_ID records for DecodeRecordIntoReorderBuffer().
 */
static void
DecodeZHeap2Op(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	uint8		info = XLogRecGetInfo(buf->record) & XLOG_ZHEAP_OPMASK;
	TransactionId xid = XLogRecGetXid(buf->record);
	SnapBuild  *builder = ctx->snapshot_builder;

	ReorderBufferProcessXid(ctx->reorder, xid, buf->origptr);

	/*
	 * If we don't have snapshot or we are just fast-forwarding, there is no
	 * point in decoding data changes.
	 */
	if (SnapBuildCurrentState(builder) < SNAPBUILD_FULL_SNAPSHOT ||
		ctx->fast_forward)
		return;

	switch (info)
	{
		case XLOG_ZHEAP_CONFIRM:
			if (SnapBuildProcessChange(builder, xid, buf->origptr))
				DecodeZHeapSpecConfirm(ctx, buf);
			break;

		/* Low-level stuff not interesting for logical decoding. */
		case XLOG_ZHEAP_UNUSED:
		case XLOG_ZHEAP_VISIBLE:
			break;

		default:
			elog(ERROR, "unexpected RM_HEAP_ID record type: %u", info);
			break;
	}
}

static inline bool
FilterByOrigin(LogicalDecodingContext *ctx, RepOriginId origin_id)
{
	if (ctx->callbacks.filter_by_origin_cb == NULL)
		return false;

	return filter_by_origin_cb_wrapper(ctx, origin_id);
}

/*
 * Handle rmgr LOGICALMSG_ID records for DecodeRecordIntoReorderBuffer().
 */
static void
DecodeLogicalMsgOp(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	SnapBuild  *builder = ctx->snapshot_builder;
	XLogReaderState *r = buf->record;
	TransactionId xid = XLogRecGetXid(r);
	uint8		info = XLogRecGetInfo(r) & ~XLR_INFO_MASK;
	RepOriginId origin_id = XLogRecGetOrigin(r);
	Snapshot	snapshot;
	xl_logical_message *message;

	if (info != XLOG_LOGICAL_MESSAGE)
		elog(ERROR, "unexpected RM_LOGICALMSG_ID record type: %u", info);

	ReorderBufferProcessXid(ctx->reorder, XLogRecGetXid(r), buf->origptr);

	/*
	 * If we don't have snapshot or we are just fast-forwarding, there is no
	 * point in decoding messages.
	 */
	if (SnapBuildCurrentState(builder) < SNAPBUILD_FULL_SNAPSHOT ||
		ctx->fast_forward)
		return;

	message = (xl_logical_message *) XLogRecGetData(r);

	if (message->dbId != ctx->slot->data.database ||
		FilterByOrigin(ctx, origin_id))
		return;

	if (message->transactional &&
		!SnapBuildProcessChange(builder, xid, buf->origptr))
		return;
	else if (!message->transactional &&
			 (SnapBuildCurrentState(builder) != SNAPBUILD_CONSISTENT ||
			  SnapBuildXactNeedsSkip(builder, buf->origptr)))
		return;

	snapshot = SnapBuildGetOrBuildSnapshot(builder, xid);
	ReorderBufferQueueMessage(ctx->reorder, xid, snapshot, buf->endptr,
							  message->transactional,
							  message->message, /* first part of message is
												 * prefix */
							  message->message_size,
							  message->message + message->prefix_size);
}

/*
 * Consolidated commit record handling between the different form of commit
 * records.
 */
static void
DecodeCommit(LogicalDecodingContext *ctx, XLogRecordBuffer *buf,
			 xl_xact_parsed_commit *parsed, TransactionId xid)
{
	XLogRecPtr	origin_lsn = InvalidXLogRecPtr;
	TimestampTz commit_time = parsed->xact_time;
	RepOriginId origin_id = XLogRecGetOrigin(buf->record);
	int			i;

	if (parsed->xinfo & XACT_XINFO_HAS_ORIGIN)
	{
		origin_lsn = parsed->origin_lsn;
		commit_time = parsed->origin_timestamp;
	}

	/*
	 * Process invalidation messages, even if we're not interested in the
	 * transaction's contents, since the various caches need to always be
	 * consistent.
	 */
	if (parsed->nmsgs > 0)
	{
		if (!ctx->fast_forward)
			ReorderBufferAddInvalidations(ctx->reorder, xid, buf->origptr,
										  parsed->nmsgs, parsed->msgs);
		ReorderBufferXidSetCatalogChanges(ctx->reorder, xid, buf->origptr);
	}

	SnapBuildCommitTxn(ctx->snapshot_builder, buf->origptr, xid,
					   parsed->nsubxacts, parsed->subxacts);

	/* ----
	 * Check whether we are interested in this specific transaction, and tell
	 * the reorderbuffer to forget the content of the (sub-)transactions
	 * if not.
	 *
	 * There can be several reasons we might not be interested in this
	 * transaction:
	 * 1) We might not be interested in decoding transactions up to this
	 *	  LSN. This can happen because we previously decoded it and now just
	 *	  are restarting or if we haven't assembled a consistent snapshot yet.
	 * 2) The transaction happened in another database.
	 * 3) The output plugin is not interested in the origin.
	 * 4) We are doing fast-forwarding
	 *
	 * We can't just use ReorderBufferAbort() here, because we need to execute
	 * the transaction's invalidations.  This currently won't be needed if
	 * we're just skipping over the transaction because currently we only do
	 * so during startup, to get to the first transaction the client needs. As
	 * we have reset the catalog caches before starting to read WAL, and we
	 * haven't yet touched any catalogs, there can't be anything to invalidate.
	 * But if we're "forgetting" this commit because it's it happened in
	 * another database, the invalidations might be important, because they
	 * could be for shared catalogs and we might have loaded data into the
	 * relevant syscaches.
	 * ---
	 */
	if (SnapBuildXactNeedsSkip(ctx->snapshot_builder, buf->origptr) ||
		(parsed->dbId != InvalidOid && parsed->dbId != ctx->slot->data.database) ||
		ctx->fast_forward || FilterByOrigin(ctx, origin_id))
	{
		for (i = 0; i < parsed->nsubxacts; i++)
		{
			ReorderBufferForget(ctx->reorder, parsed->subxacts[i], buf->origptr);
		}
		ReorderBufferForget(ctx->reorder, xid, buf->origptr);

		return;
	}

	/* tell the reorderbuffer about the surviving subtransactions */
	for (i = 0; i < parsed->nsubxacts; i++)
	{
		ReorderBufferCommitChild(ctx->reorder, xid, parsed->subxacts[i],
								 buf->origptr, buf->endptr);
	}

	/* replay actions of all transaction + subtransactions in order */
	ReorderBufferCommit(ctx->reorder, xid, buf->origptr, buf->endptr,
						commit_time, origin_id, origin_lsn);
}

/*
 * Get the data from the various forms of abort records and pass it on to
 * snapbuild.c and reorderbuffer.c
 */
static void
DecodeAbort(LogicalDecodingContext *ctx, XLogRecordBuffer *buf,
			xl_xact_parsed_abort *parsed, TransactionId xid)
{
	int			i;

	for (i = 0; i < parsed->nsubxacts; i++)
	{
		ReorderBufferAbort(ctx->reorder, parsed->subxacts[i],
						   buf->record->EndRecPtr);
	}

	ReorderBufferAbort(ctx->reorder, xid, buf->record->EndRecPtr);
}

/*
 * Parse XLOG_HEAP_INSERT (not MULTI_INSERT!) records into tuplebufs.
 *
 * Deletes can contain the new tuple.
 */
static void
DecodeInsert(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	Size		datalen;
	char	   *tupledata;
	Size		tuplelen;
	XLogReaderState *r = buf->record;
	xl_heap_insert *xlrec;
	ReorderBufferChange *change;
	RelFileNode target_node;

	xlrec = (xl_heap_insert *) XLogRecGetData(r);

	/*
	 * Ignore insert records without new tuples (this does happen when
	 * raw_heap_insert marks the TOAST record as HEAP_INSERT_NO_LOGICAL).
	 */
	if (!(xlrec->flags & XLH_INSERT_CONTAINS_NEW_TUPLE))
		return;

	/* only interested in our database */
	XLogRecGetBlockTag(r, 0, &target_node, NULL, NULL);
	if (target_node.dbNode != ctx->slot->data.database)
		return;

	/* output plugin doesn't look for this origin, no need to queue */
	if (FilterByOrigin(ctx, XLogRecGetOrigin(r)))
		return;

	change = ReorderBufferGetChange(ctx->reorder);
	if (!(xlrec->flags & XLH_INSERT_IS_SPECULATIVE))
		change->action = REORDER_BUFFER_CHANGE_INSERT;
	else
		change->action = REORDER_BUFFER_CHANGE_INTERNAL_SPEC_INSERT;
	change->origin_id = XLogRecGetOrigin(r);

	memcpy(&change->data.tp.relnode, &target_node, sizeof(RelFileNode));

	tupledata = XLogRecGetBlockData(r, 0, &datalen);
	tuplelen = datalen - SizeOfHeapHeader;

	change->data.tp.newtuple =
		ReorderBufferGetTupleBuf(ctx->reorder, tuplelen);

	DecodeXLogTuple(tupledata, datalen, change->data.tp.newtuple);

	change->data.tp.clear_toast_afterwards = true;

	ReorderBufferQueueChange(ctx->reorder, XLogRecGetXid(r), buf->origptr, change);
}

/*
 * Parse XLOG_ZHEAP_INSERT (not MULTI_INSERT!) records into tuplebufs.
 */
static void
DecodeZHeapInsert(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	Size		datalen;
	char	   *tupledata;
	Size		tuplelen;
	XLogReaderState *r = buf->record;
	xl_zheap_insert *xlrec;
	ReorderBufferChange *change;
	RelFileNode target_node;

	xlrec = (xl_zheap_insert *) XLogRecGetData(r);

	/*
	 * This flag should be set as long as ZHEAP_INSERT_NO_LOGICAL is never
	 * passed to zheap_insert().
	 *
	 * TODO Check if raw_zheap_insert() should pass this option yet. Or is
	 * there no need for rewrite with zheap? If it's so, make sure that VACUUM
	 * FULL / CLUSTER commands are not executed on zheap tables.
	 */
	Assert(xlrec->flags & XLZ_INSERT_CONTAINS_NEW_TUPLE);

	/* only interested in our database */
	XLogRecGetBlockTag(r, 0, &target_node, NULL, NULL);
	if (target_node.dbNode != ctx->slot->data.database)
		return;

	/* output plugin doesn't look for this origin, no need to queue */
	if (FilterByOrigin(ctx, XLogRecGetOrigin(r)))
		return;

	change = ReorderBufferGetChange(ctx->reorder);
	if (!(xlrec->flags & XLZ_INSERT_IS_SPECULATIVE))
		change->action = REORDER_BUFFER_CHANGE_INSERT_ZHEAP;
	else
		change->action = REORDER_BUFFER_CHANGE_INTERNAL_SPEC_INSERT_ZHEAP;
	change->origin_id = XLogRecGetOrigin(r);

	memcpy(&change->data.tp.relnode, &target_node, sizeof(RelFileNode));

	tupledata = XLogRecGetBlockData(r, 0, &datalen);
	tuplelen = datalen - SizeOfZHeapHeader;

	change->data.tp.newtuple =
		ReorderBufferGetZHeapTupleBuf(ctx->reorder, tuplelen);
	DecodeXLogZHeapTuple(tupledata, datalen, change->data.tp.newtuple);

	change->data.tp.clear_toast_afterwards = true;
	ReorderBufferQueueChange(ctx->reorder, XLogRecGetXid(r), buf->origptr, change);
}

/*
 * Parse XLOG_HEAP_UPDATE and XLOG_HEAP_HOT_UPDATE, which have the same layout
 * in the record, from wal into proper tuplebufs.
 *
 * Updates can possibly contain a new tuple and the old primary key.
 */
static void
DecodeUpdate(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	XLogReaderState *r = buf->record;
	xl_heap_update *xlrec;
	ReorderBufferChange *change;
	char	   *data;
	RelFileNode target_node;

	xlrec = (xl_heap_update *) XLogRecGetData(r);

	/* only interested in our database */
	XLogRecGetBlockTag(r, 0, &target_node, NULL, NULL);
	if (target_node.dbNode != ctx->slot->data.database)
		return;

	/* output plugin doesn't look for this origin, no need to queue */
	if (FilterByOrigin(ctx, XLogRecGetOrigin(r)))
		return;

	change = ReorderBufferGetChange(ctx->reorder);
	change->action = REORDER_BUFFER_CHANGE_UPDATE;
	change->origin_id = XLogRecGetOrigin(r);
	memcpy(&change->data.tp.relnode, &target_node, sizeof(RelFileNode));

	if (xlrec->flags & XLH_UPDATE_CONTAINS_NEW_TUPLE)
	{
		Size		datalen;
		Size		tuplelen;

		data = XLogRecGetBlockData(r, 0, &datalen);

		tuplelen = datalen - SizeOfHeapHeader;

		change->data.tp.newtuple =
			ReorderBufferGetTupleBuf(ctx->reorder, tuplelen);

		DecodeXLogTuple(data, datalen, change->data.tp.newtuple);
	}

	if (xlrec->flags & XLH_UPDATE_CONTAINS_OLD)
	{
		Size		datalen;
		Size		tuplelen;

		/* caution, remaining data in record is not aligned */
		data = XLogRecGetData(r) + SizeOfHeapUpdate;
		datalen = XLogRecGetDataLen(r) - SizeOfHeapUpdate;
		tuplelen = datalen - SizeOfHeapHeader;

		change->data.tp.oldtuple =
			ReorderBufferGetTupleBuf(ctx->reorder, tuplelen);

		DecodeXLogTuple(data, datalen, change->data.tp.oldtuple);
	}

	change->data.tp.clear_toast_afterwards = true;

	ReorderBufferQueueChange(ctx->reorder, XLogRecGetXid(r), buf->origptr, change);
}

/*
 * Parse XLOG_ZHEAP_UPDATE from wal into proper tuplebufs.
 */
static void
DecodeZHeapUpdate(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	XLogReaderState *r = buf->record;
	Size		datalen;
	Size	recordlen;
	char	   *tupledata;
	Size		tuplelen;
	xl_undo_header *xlundohdr;
	xl_zheap_update *xlrec;
	RelFileNode target_node;
	ReorderBufferChange *change;

	xlundohdr = (xl_undo_header *) XLogRecGetData(r);
	xlrec = (xl_zheap_update *) ((char *) xlundohdr + SizeOfUndoHeader);
	recordlen = XLogRecGetDataLen(r);

	/* only interested in our database */
	XLogRecGetBlockTag(r, 0, &target_node, NULL, NULL);
	if (target_node.dbNode != ctx->slot->data.database)
		return;

	/* output plugin doesn't look for this origin, no need to queue */
	if (FilterByOrigin(ctx, XLogRecGetOrigin(r)))
		return;

	change = ReorderBufferGetChange(ctx->reorder);
	change->action = REORDER_BUFFER_CHANGE_UPDATE_ZHEAP;
	change->origin_id = XLogRecGetOrigin(r);

	memcpy(&change->data.tp.relnode, &target_node, sizeof(RelFileNode));

	tupledata = (char *) xlrec + SizeOfZHeapUpdate;
	datalen = recordlen - SizeOfUndoHeader - SizeOfZHeapUpdate;
	if (xlrec->flags & XLZ_UPDATE_OLD_CONTAINS_TPD_SLOT)
	{
		/* The old transaction slot id. */
		tupledata += sizeof(int);
		datalen -= sizeof(int);
	}
	if (xlrec->flags & XLZ_NON_INPLACE_UPDATE)
	{
		tupledata += SizeOfUndoHeader;
		datalen -= SizeOfUndoHeader;
		if (xlrec->flags & XLZ_UPDATE_NEW_CONTAINS_TPD_SLOT)
		{
			/* The new transaction slot id. */
			tupledata += sizeof(int);
			datalen -= sizeof(int);
		}
	}
	/* Logical decoding does enforce inclusion of the tuple(s) in WAL. */
	Assert(xlrec->flags & XLZ_HAS_UPDATE_UNDOTUPLE);

	tuplelen = datalen - SizeOfZHeapHeader;
	change->data.tp.oldtuple =
		ReorderBufferGetZHeapTupleBuf(ctx->reorder, tuplelen);
	DecodeXLogZHeapTuple(tupledata, datalen, change->data.tp.oldtuple);

	/* The new tuple. */
	tupledata = XLogRecGetBlockData(r, 0, &datalen);

	/*
	 * The prefix-suffix compression should be disabled if logical decoding is
	 * active.
	 */
	Assert((xlrec->flags &
			(XLZ_UPDATE_PREFIX_FROM_OLD | XLZ_UPDATE_SUFFIX_FROM_OLD)) == 0);

	tuplelen = datalen - SizeOfZHeapHeader;
	change->data.tp.newtuple =
		ReorderBufferGetZHeapTupleBuf(ctx->reorder, tuplelen);
	DecodeXLogZHeapTuple(tupledata, datalen, change->data.tp.newtuple);

	change->data.tp.clear_toast_afterwards = true;

	ReorderBufferQueueChange(ctx->reorder, XLogRecGetXid(r), buf->origptr, change);
}

/*
 * Parse XLOG_HEAP_DELETE from wal into proper tuplebufs.
 *
 * Deletes can possibly contain the old primary key.
 */
static void
DecodeDelete(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	XLogReaderState *r = buf->record;
	xl_heap_delete *xlrec;
	ReorderBufferChange *change;
	RelFileNode target_node;

	xlrec = (xl_heap_delete *) XLogRecGetData(r);

	/* only interested in our database */
	XLogRecGetBlockTag(r, 0, &target_node, NULL, NULL);
	if (target_node.dbNode != ctx->slot->data.database)
		return;

	/*
	 * Super deletions are irrelevant for logical decoding, it's driven by the
	 * confirmation records.
	 */
	if (xlrec->flags & XLH_DELETE_IS_SUPER)
		return;

	/* output plugin doesn't look for this origin, no need to queue */
	if (FilterByOrigin(ctx, XLogRecGetOrigin(r)))
		return;

	change = ReorderBufferGetChange(ctx->reorder);
	change->action = REORDER_BUFFER_CHANGE_DELETE;
	change->origin_id = XLogRecGetOrigin(r);

	memcpy(&change->data.tp.relnode, &target_node, sizeof(RelFileNode));

	/* old primary key stored */
	if (xlrec->flags & XLH_DELETE_CONTAINS_OLD)
	{
		Size		datalen = XLogRecGetDataLen(r) - SizeOfHeapDelete;
		Size		tuplelen = datalen - SizeOfHeapHeader;

		Assert(XLogRecGetDataLen(r) > (SizeOfHeapDelete + SizeOfHeapHeader));

		change->data.tp.oldtuple =
			ReorderBufferGetTupleBuf(ctx->reorder, tuplelen);

		DecodeXLogTuple((char *) xlrec + SizeOfHeapDelete,
						datalen, change->data.tp.oldtuple);
	}

	change->data.tp.clear_toast_afterwards = true;

	ReorderBufferQueueChange(ctx->reorder, XLogRecGetXid(r), buf->origptr, change);
}

/*
 * Parse XLOG_ZHEAP_DELETE from wal into proper tuplebufs.
 */
static void
DecodeZHeapDelete(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	XLogReaderState *r = buf->record;
	Size		datalen;
	Size	recordlen;
	char	   *tupledata;
	Size		tuplelen;
	xl_undo_header *xlundohdr;
	xl_zheap_delete *xlrec;
	RelFileNode target_node;
	ReorderBufferChange *change;

	xlundohdr = (xl_undo_header *) XLogRecGetData(r);
	xlrec = (xl_zheap_delete *) ((char *) xlundohdr + SizeOfUndoHeader);
	recordlen = XLogRecGetDataLen(r);

	/* Logical decoding does enforce inclusion of the tuple in WAL. */
	Assert(xlrec->flags & XLZ_HAS_DELETE_UNDOTUPLE);

	/* only interested in our database */
	XLogRecGetBlockTag(r, 0, &target_node, NULL, NULL);
	if (target_node.dbNode != ctx->slot->data.database)
		return;

	/* output plugin doesn't look for this origin, no need to queue */
	if (FilterByOrigin(ctx, XLogRecGetOrigin(r)))
		return;

	change = ReorderBufferGetChange(ctx->reorder);
	change->action = REORDER_BUFFER_CHANGE_DELETE_ZHEAP;
	change->origin_id = XLogRecGetOrigin(r);

	memcpy(&change->data.tp.relnode, &target_node, sizeof(RelFileNode));

	tupledata = (char *) xlrec + SizeOfZHeapDelete;
	datalen = recordlen - SizeOfUndoHeader - SizeOfZHeapDelete;
	if (xlrec->flags & XLZ_DELETE_CONTAINS_TPD_SLOT)
	{
		/* Transaction slot id. */
		tupledata += sizeof(int);
		datalen -= sizeof(int);
	}
	tuplelen = datalen - SizeOfZHeapHeader;

	change->data.tp.oldtuple =
		ReorderBufferGetZHeapTupleBuf(ctx->reorder, tuplelen);
	DecodeXLogZHeapTuple(tupledata, datalen, change->data.tp.oldtuple);

	change->data.tp.clear_toast_afterwards = true;
	ReorderBufferQueueChange(ctx->reorder, XLogRecGetXid(r), buf->origptr, change);
}

/*
 * Parse XLOG_HEAP_TRUNCATE from wal
 */
static void
DecodeTruncate(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	XLogReaderState *r = buf->record;
	xl_heap_truncate *xlrec;
	ReorderBufferChange *change;

	xlrec = (xl_heap_truncate *) XLogRecGetData(r);

	/* only interested in our database */
	if (xlrec->dbId != ctx->slot->data.database)
		return;

	/* output plugin doesn't look for this origin, no need to queue */
	if (FilterByOrigin(ctx, XLogRecGetOrigin(r)))
		return;

	change = ReorderBufferGetChange(ctx->reorder);
	change->action = REORDER_BUFFER_CHANGE_TRUNCATE;
	change->origin_id = XLogRecGetOrigin(r);
	if (xlrec->flags & XLH_TRUNCATE_CASCADE)
		change->data.truncate.cascade = true;
	if (xlrec->flags & XLH_TRUNCATE_RESTART_SEQS)
		change->data.truncate.restart_seqs = true;
	change->data.truncate.nrelids = xlrec->nrelids;
	change->data.truncate.relids = ReorderBufferGetRelids(ctx->reorder,
														  xlrec->nrelids);
	memcpy(change->data.truncate.relids, xlrec->relids,
		   xlrec->nrelids * sizeof(Oid));
	ReorderBufferQueueChange(ctx->reorder, XLogRecGetXid(r),
							 buf->origptr, change);
}

/*
 * Decode XLOG_HEAP2_MULTI_INSERT_insert record into multiple tuplebufs.
 *
 * Currently MULTI_INSERT will always contain the full tuples.
 */
static void
DecodeMultiInsert(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	XLogReaderState *r = buf->record;
	xl_heap_multi_insert *xlrec;
	int			i;
	char	   *data;
	char	   *tupledata;
	Size		tuplelen;
	RelFileNode rnode;

	xlrec = (xl_heap_multi_insert *) XLogRecGetData(r);

	/* only interested in our database */
	XLogRecGetBlockTag(r, 0, &rnode, NULL, NULL);
	if (rnode.dbNode != ctx->slot->data.database)
		return;

	/* output plugin doesn't look for this origin, no need to queue */
	if (FilterByOrigin(ctx, XLogRecGetOrigin(r)))
		return;

	tupledata = XLogRecGetBlockData(r, 0, &tuplelen);

	data = tupledata;
	for (i = 0; i < xlrec->ntuples; i++)
	{
		ReorderBufferChange *change;
		xl_multi_insert_tuple *xlhdr;
		int			datalen;
		ReorderBufferTupleBuf *tuple;

		change = ReorderBufferGetChange(ctx->reorder);
		change->action = REORDER_BUFFER_CHANGE_INSERT;
		change->origin_id = XLogRecGetOrigin(r);

		memcpy(&change->data.tp.relnode, &rnode, sizeof(RelFileNode));

		/*
		 * CONTAINS_NEW_TUPLE will always be set currently as multi_insert
		 * isn't used for catalogs, but better be future proof.
		 *
		 * We decode the tuple in pretty much the same way as DecodeXLogTuple,
		 * but since the layout is slightly different, we can't use it here.
		 */
		if (xlrec->flags & XLH_INSERT_CONTAINS_NEW_TUPLE)
		{
			HeapTupleHeader header;

			xlhdr = (xl_multi_insert_tuple *) SHORTALIGN(data);
			data = ((char *) xlhdr) + SizeOfMultiInsertTuple;
			datalen = xlhdr->datalen;

			change->data.tp.newtuple =
				ReorderBufferGetTupleBuf(ctx->reorder, datalen);

			tuple = change->data.tp.newtuple;
			header = tuple->tuple.t_data;

			/* not a disk based tuple */
			ItemPointerSetInvalid(&tuple->tuple.t_self);

			/*
			 * We can only figure this out after reassembling the
			 * transactions.
			 */
			tuple->tuple.t_tableOid = InvalidOid;

			tuple->tuple.t_len = datalen + SizeofHeapTupleHeader;

			memset(header, 0, SizeofHeapTupleHeader);

			memcpy((char *) tuple->tuple.t_data + SizeofHeapTupleHeader,
				   (char *) data,
				   datalen);
			data += datalen;

			header->t_infomask = xlhdr->t_infomask;
			header->t_infomask2 = xlhdr->t_infomask2;
			header->t_hoff = xlhdr->t_hoff;
		}

		/*
		 * Reset toast reassembly state only after the last row in the last
		 * xl_multi_insert_tuple record emitted by one heap_multi_insert()
		 * call.
		 */
		if (xlrec->flags & XLH_INSERT_LAST_IN_MULTI &&
			(i + 1) == xlrec->ntuples)
			change->data.tp.clear_toast_afterwards = true;
		else
			change->data.tp.clear_toast_afterwards = false;

		ReorderBufferQueueChange(ctx->reorder, XLogRecGetXid(r),
								 buf->origptr, change);
	}
	Assert(data == tupledata + tuplelen);
}

/*
 * Decode XLOG_ZHEAP_MULTI_INSERT record into multiple tuplebufs.
 */
static void
DecodeZHeapMultiInsert(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	XLogReaderState *r = buf->record;
	char	   *data, *tupledata;
	Size	tuplelen;
	int		nranges, i;
	xl_undo_header *xlundohdr;
	xl_zheap_multi_insert *xlrec;
	RelFileNode rnode;

	xlundohdr = (xl_undo_header *) XLogRecGetData(r);
	xlrec = (xl_zheap_multi_insert *) ((char *) xlundohdr + SizeOfUndoHeader);

	/* only interested in our database */
	XLogRecGetBlockTag(r, 0, &rnode, NULL, NULL);
	if (rnode.dbNode != ctx->slot->data.database)
		return;

	/* output plugin doesn't look for this origin, no need to queue */
	if (FilterByOrigin(ctx, XLogRecGetOrigin(r)))
		return;

	data = (char *) xlrec + SizeOfZHeapMultiInsert;

	/* Not really interested in the ranges, just skip that part. */
	memcpy(&nranges, data, sizeof(int));
	data += sizeof(int);
	data += nranges * sizeof(OffsetNumber) * 2;

	tupledata = XLogRecGetBlockData(r, 0, &tuplelen);
	data = tupledata;

	for (i = 0; i < xlrec->ntuples; i++)
	{
		ReorderBufferChange *change;
		xl_multi_insert_ztuple *xlhdr;
		int			datalen;
		ReorderBufferTupleBuf *tuple;
		ZHeapTupleHeader header;

		change = ReorderBufferGetChange(ctx->reorder);
		change->action = REORDER_BUFFER_CHANGE_INSERT_ZHEAP;
		change->origin_id = XLogRecGetOrigin(r);
		memcpy(&change->data.tp.relnode, &rnode, sizeof(RelFileNode));

		/*
		 * CONTAINS_NEW_TUPLE will always be set currently as multi_insert
		 * isn't used for catalogs, but better be future proof.
		 *
		 * We decode the tuple in pretty much the same way as
		 * DecodeXLogZHeapTuple, but since the layout is slightly different,
		 * we can't use it here.
		 */
		Assert(xlrec->flags & XLZ_INSERT_CONTAINS_NEW_TUPLE);

		xlhdr = (xl_multi_insert_ztuple *) SHORTALIGN(data);
		data = ((char *) xlhdr) + SizeOfMultiInsertZTuple;
		datalen = xlhdr->datalen;

		change->data.tp.newtuple =
			ReorderBufferGetZHeapTupleBuf(ctx->reorder, datalen);

		tuple = change->data.tp.newtuple;
		header = (ZHeapTupleHeader) tuple->tuple.t_data;

		/* not a disk based tuple */
		ItemPointerSetInvalid(&tuple->tuple.t_self);

		/*
		 * We can only figure this out after reassembling the
		 * transactions.
		 */
		tuple->tuple.t_tableOid = InvalidOid;

		tuple->tuple.t_len = datalen + SizeofZHeapTupleHeader;

		memset(header, 0, SizeofZHeapTupleHeader);

		memcpy((char *) tuple->tuple.t_data + SizeofZHeapTupleHeader,
			   (char *) data,
			   datalen);
		data += datalen;

		header->t_infomask = xlhdr->t_infomask;
		header->t_infomask2 = xlhdr->t_infomask2;
		header->t_hoff = xlhdr->t_hoff;

		/*
		 * Reset toast reassembly state only after the last row in the last
		 * xl_multi_insert_tuple record emitted by one heap_multi_insert()
		 * call.
		 */
		if (xlrec->flags & XLZ_INSERT_LAST_IN_MULTI &&
			(i + 1) == xlrec->ntuples)
			change->data.tp.clear_toast_afterwards = true;
		else
			change->data.tp.clear_toast_afterwards = false;

		ReorderBufferQueueChange(ctx->reorder, XLogRecGetXid(r),
								 buf->origptr, change);
	}
	Assert(data == tupledata + tuplelen);
}

/*
 * Parse XLOG_HEAP_CONFIRM from wal into a confirmation change.
 *
 * This is pretty trivial, all the state essentially already setup by the
 * speculative insertion.
 */
static void
DecodeSpecConfirm(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	XLogReaderState *r = buf->record;
	ReorderBufferChange *change;
	RelFileNode target_node;

	/* only interested in our database */
	XLogRecGetBlockTag(r, 0, &target_node, NULL, NULL);
	if (target_node.dbNode != ctx->slot->data.database)
		return;

	/* output plugin doesn't look for this origin, no need to queue */
	if (FilterByOrigin(ctx, XLogRecGetOrigin(r)))
		return;

	change = ReorderBufferGetChange(ctx->reorder);
	change->action = REORDER_BUFFER_CHANGE_INTERNAL_SPEC_CONFIRM;
	change->origin_id = XLogRecGetOrigin(r);

	memcpy(&change->data.tp.relnode, &target_node, sizeof(RelFileNode));

	change->data.tp.clear_toast_afterwards = true;

	ReorderBufferQueueChange(ctx->reorder, XLogRecGetXid(r), buf->origptr, change);
}

/*
 * Like DecodeSpecConfirm but for zheap.
 */
static void
DecodeZHeapSpecConfirm(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	XLogReaderState *r = buf->record;
	ReorderBufferChange *change;
	RelFileNode target_node;
	xl_zheap_confirm *xlrec;

	xlrec = (xl_zheap_confirm *) XLogRecGetData(r);

	/* only interested in our database */
	XLogRecGetBlockTag(r, 0, &target_node, NULL, NULL);
	if (target_node.dbNode != ctx->slot->data.database)
		return;

	/* output plugin doesn't look for this origin, no need to queue */
	if (FilterByOrigin(ctx, XLogRecGetOrigin(r)))
		return;

	/*
	 * If the speculative insert is not confirmed by the following record,
	 * ReorderBufferCommit() will simply "forget" it. No need to do anything
	 * elsewhere.
	 */
	if ((xlrec->flags & XLZ_SPEC_INSERT_SUCCESS) == 0)
		return;

	change = ReorderBufferGetChange(ctx->reorder);
	change->action = REORDER_BUFFER_CHANGE_INTERNAL_SPEC_CONFIRM;
	change->origin_id = XLogRecGetOrigin(r);

	memcpy(&change->data.tp.relnode, &target_node, sizeof(RelFileNode));

	change->data.tp.clear_toast_afterwards = true;

	ReorderBufferQueueChange(ctx->reorder, XLogRecGetXid(r), buf->origptr, change);
}


/*
 * Read a HeapTuple as WAL logged by heap_insert, heap_update and heap_delete
 * (but not by heap_multi_insert) into a tuplebuf.
 *
 * The size 'len' and the pointer 'data' in the record need to be
 * computed outside as they are record specific.
 */
static void
DecodeXLogTuple(char *data, Size len, ReorderBufferTupleBuf *tuple)
{
	xl_heap_header xlhdr;
	int			datalen = len - SizeOfHeapHeader;
	HeapTupleHeader header;

	Assert(datalen >= 0);

	tuple->tuple.t_len = datalen + SizeofHeapTupleHeader;
	header = tuple->tuple.t_data;

	/* not a disk based tuple */
	ItemPointerSetInvalid(&tuple->tuple.t_self);

	/* we can only figure this out after reassembling the transactions */
	tuple->tuple.t_tableOid = InvalidOid;

	/* data is not stored aligned, copy to aligned storage */
	memcpy((char *) &xlhdr,
		   data,
		   SizeOfHeapHeader);

	memset(header, 0, SizeofHeapTupleHeader);

	memcpy(((char *) tuple->tuple.t_data) + SizeofHeapTupleHeader,
		   data + SizeOfHeapHeader,
		   datalen);

	header->t_infomask = xlhdr.t_infomask;
	header->t_infomask2 = xlhdr.t_infomask2;
	header->t_hoff = xlhdr.t_hoff;
}

/*
 * Like DecodeXLogTuple(), but process zheap tuple.
 *
 * Initially we (mis)use HeapTuple to accommodate the data. It will be
 * converted in ReorderBufferCommit, when the tuple descriptor is
 * available. It's not worth defining a variant of ReorderBufferTupleBuf,
 * which would be almost identical.
 */
static void
DecodeXLogZHeapTuple(char *data, Size len, ReorderBufferTupleBuf *tuple)
{
	xl_heap_header xlhdr;
	int			datalen = len - SizeOfZHeapHeader;
	ZHeapTupleHeader header;

	Assert(datalen >= 0);

	tuple->tuple.t_len = datalen + SizeofZHeapTupleHeader;
	/*
	 * ZHeapTupleHeader is stored in the buffer, until the tuple is converted
	 * to regular heap tuple.
	 */
	header = (ZHeapTupleHeader) tuple->tuple.t_data;

	/* not a disk based tuple */
	ItemPointerSetInvalid(&tuple->tuple.t_self);

	/* we can only figure this out after reassembling the transactions */
	tuple->tuple.t_tableOid = InvalidOid;

	/* data is not stored aligned, copy to aligned storage */
	memcpy((char *) &xlhdr,
		   data,
		   SizeOfZHeapHeader);

	memset(header, 0, SizeofZHeapTupleHeader);

	memcpy(((char *) tuple->tuple.t_data) + SizeofZHeapTupleHeader,
		   data + SizeOfZHeapHeader,
		   datalen);

	header->t_infomask = xlhdr.t_infomask;
	header->t_infomask2 = xlhdr.t_infomask2;
	header->t_hoff = xlhdr.t_hoff;
}
