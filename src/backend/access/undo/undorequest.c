/*-------------------------------------------------------------------------
 *
 * undorequest.c
 *		Undo request manager.
 *
 * From the moment a transaction begins until the moment that it commits,
 * there is a possibility that it might abort, either due to an exception
 * or because the entire system is restarted (e.g. because of a power
 * cut). If this happens, all undo generated by that transaction prior
 * to the abort must be applied.  To ensure this, the calling code must
 * ensure that an "undo request" is registered for every transaction
 * that generates undo.
 *
 * The undo request should be registered before the transaction writes any
 * undo records (except for temporary undo records, which the creating backend
 * will need to process locally). If the transaction goes on to commit, the
 * undo request can be deleted; if it goes on to abort, it needs to be updated
 * with the final size of the undo generated by that transaction so that
 * we can prioritize it appropriately. One of the key tasks of this module
 * is to decide on the order in which undo requests should been processed;
 * see GetNextUndoRequest for details.
 *
 * We have only a fixed amount of shared memory to store undo requests;
 * because an undo request has to be created before any undo that might
 * need to be processed is written, we should never end up in a situation
 * where there are more existing undo requests that can fit. In extreme
 * cases, this might cause us to have to refuse to create new requests,
 * but that should very rare.  If we're starting to run low on space,
 * FinalizeUndoRequest() will signal callers that undo should be
 * performed in the foreground; actually hitting the hard limit requires
 * foreground undo to be interrupted by a crash.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/undo/undorequest.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/undorequest.h"
#include "lib/rbtree.h"
#include "storage/shmem.h"
#include "utils/timestamp.h"

/*
 * An UndoRequestData object stores the information that we must have in order
 * to perform background undo for a transaction. It may be stored in memory or
 * serialized to disk.
 *
 * We won't have all of this information while the transaction is still
 * runnning and need not ever collect it if the transaction commits, but if
 * the transaction aborts or is prepared, we need to remember all of these
 * details at that point.
 *
 * If the transaction is aborted by a crash, we need to reconstruct these
 * details after restarting.
 *
 * Note that we don't care about temporary undo, because it can never need
 * to be performed in the background. If the session dies without taking care
 * of permanent or unlogged undo, the associated undo actions still need to
 * be performed at some later point, but the same principle does not apply
 * to temporary undo. All temporary objects disappear with the session that
 * owned them, making the undo irrelevant.
 */
typedef struct UndoRequestData
{
	FullTransactionId fxid;
	Oid			dbid;
	Size		size;
	UndoRecPtr	start_location_logged;
	UndoRecPtr	end_location_logged;
	UndoRecPtr	start_location_unlogged;
	UndoRecPtr	end_location_unlogged;
} UndoRequestData;

/*
 * An UndoRequest object represents the possible need to perform background
 * undo actions for a transaction if it aborts; unlike an UndoRequestData
 * object, it only ever exists in memory.
 *
 * The main purpose of this module is to manage a fixed pool of UndoRequest
 * objects. Because the pool is of fixed size, an UndoRequest should be
 * allocated before a transaction writes any permanent or unlogged undo (see
 * comments on UndoRequestData for why we don't care about temporary undo).
 * It can be deallocated when it is clear that no such actions will need to
 * be performed or when they have all been performed successfully.
 *
 * At any given time, an UndoRequest managed by an UndoRequestManager is in
 * one of three states: FREE, UNLISTED, or LISTED. FREE can be distinguished
 * from the other states by examining the state of the UndoRequest itself:
 * if d.fxid is InvalidFullTransactionId, the UndoRequest is FREE; otherwise,
 * it is either LISTED or UNLISTED.  When an UndoRequest is FREE, it is not
 * allocated to any transaction and is available for reuse.
 *
 * Nothing in the UndoRequest explicitly distinguishes between the LISTED and
 * UNLISTED state. In either state, the UndoRequest has been allocated to the
 * transaction identified by d.fxid. When UNLISTED, the UndoRequestManager
 * has not added the UndoRequest to any RBTRee; when LISTED, it has been
 * added either to both of requests_by_fxid and requests_by_size or else to
 * requests_by_retry_time.
 *
 * When an UndoRequest is either FREE or LISTED, all changes to it require
 * the UndoRequestManager's lock. When it is UNLISTED, changes be made without
 * taking the lock, except for d.fxid and next_retry_time, which should not
 * be modified. This is safe because nothing should rely on the data in an
 * UNLISTED request actually being correct; it corresponds to a tranasction
 * which is still running, for which the final values of fields in the
 * UndoRequestData are not yet known.
 *
 * Callers must be careful never to lose track of an entry that is UNLISTED;
 * such entries will be permanently leaked. An entry that is FREE can be
 * reallocated by this module, while one that is LISTED should eventually
 * get processed and become FREE, but an UNLISTED entry remains the caller's
 * responsibility until the state is changed.
 */
struct UndoRequest
{
	UndoRequestData	d;
	TimestampTz retry_time;
	UndoRequest *next_free_request;
};

/*
 * An UndoRequestNode just points to an UndoRequest. We use it so that the
 * same UndoRequest can be placed into more than one RBTree at the same
 * time.
 */
typedef struct UndoRequestNode
{
	RBTNode		rbtnode;
	UndoRequest *req;
} UndoRequestNode;

/*
 * Possible sources of UndoRequest objects in need of processing.
 */
typedef enum UndoRequestSource
{
	UNDO_SOURCE_FXID,
	UNDO_SOURCE_SIZE,
	UNDO_SOURCE_RETRY_TIME
} UndoRequestSource;

/*
 * An UndoRequestManager manages a collection of UndoRequest and
 * UndoRequestNode objects. Typically, there would only be one such object
 * for the whole system, but it's possible to create others for testing
 * purposes.
 */
struct UndoRequestManager
{
	LWLock	   *lock;			/* for synchronization */
	Size		capacity;		/* max # of UndoRequests */
	Size		utilization;	/* # of non-FREE UndoRequests */
	Size		soft_size_limit;	/* threshold to not background */
	UndoRequestSource source;	/* which RBTree to check next? */
	RBTree		requests_by_fxid;	/* lower FXIDs first */
	RBTree		requests_by_size;	/* bigger sizes first */
	RBTree		requests_by_retry_time; /* sooner retry times first */
	bool		oldest_fxid_valid;	/* true if next field is valid */
	FullTransactionId oldest_fxid;	/* oldest FXID of any UndoRequest */
	UndoRequest *all_requests;
	UndoRequest *first_free_request;
	UndoRequestNode *first_free_request_node;
};

/* Static functions. */
static UndoRequest *FindUndoRequestForDatabase(UndoRequestManager *urm,
											   Oid dbid);
static bool BackgroundUndoOK(UndoRequestManager *urm,
							 UndoRequest *req);
static RBTNode *UndoRequestNodeAllocate(void *arg);
static void UndoRequestNodeFree(RBTNode *x, void *arg);
static void UndoRequestNodeCombine(RBTNode *existing, const RBTNode *newdata,
								   void *arg);
static int	UndoRequestNodeCompareRetryTime(const RBTNode *a,
											const RBTNode *b,
											void *arg);
static int	UndoRequestNodeCompareFXID(const RBTNode *a, const RBTNode *b,
									   void *arg);
static int	UndoRequestNodeCompareSize(const RBTNode *a, const RBTNode *b,
									   void *arg);
static void InsertUndoRequest(RBTree *rbt, UndoRequest *req);
static void RemoveUndoRequest(RBTree *rbt, UndoRequest *req);
static UndoRequest *FindUndoRequest(UndoRequestManager *urm,
									FullTransactionId fxid);

/*
 * Compute the amount of space that will be needed by an undo request manager.
 *
 * We need space for the UndoRequestManager itself, for the UndoRequest
 * objects, and for the UndoRequestNode objects.  We need twice as many
 * UndoRequestNode objects as we do UndoRequest objects, because unfailed
 * requests are stored in both requests_by_fxid and requests_by_size; failed
 * requests are stored only in requests_by_retry_time.
 */
Size
EstimateUndoRequestManagerSize(Size capacity)
{
	Size		s = MAXALIGN(sizeof(UndoRequestManager));

	s = add_size(s, MAXALIGN(mul_size(capacity, sizeof(UndoRequest))));
	s = add_size(s, MAXALIGN(mul_size(capacity,
									  mul_size(2, sizeof(UndoRequestNode)))));

	return s;
}

/*
 * Initialize an undo request manager.
 *
 * The caller is responsible for providing an appropriately-sized chunk of
 * memory; use EstimateUndoRequestManagerSize to find out how much space will
 * be needed. This means that this infrastructure can potentially be used in
 * either shared memory or, if desired, in backend-private memory. It will not
 * work in DSM, though, because it uses pointers.
 *
 * The caller must also provide a lock that will be used to protect access
 * to the data managed by this undo request manager.  This cannot be NULL,
 * even if the memory is private.
 */
void
InitializeUndoRequestManager(UndoRequestManager *urm, LWLock *lock,
							 Size capacity, Size soft_limit)
{
	UndoRequest *reqs;
	UndoRequestNode *nodes;
	int			i;

	/* Basic initialization. */
	urm->lock = lock;
	urm->capacity = capacity;
	urm->utilization = 0;
	urm->soft_size_limit = soft_limit;
	urm->source = UNDO_SOURCE_FXID;
	rbt_initialize(&urm->requests_by_fxid, sizeof(UndoRequestNode),
				   UndoRequestNodeCompareFXID, UndoRequestNodeCombine,
				   UndoRequestNodeAllocate, UndoRequestNodeFree, urm);
	rbt_initialize(&urm->requests_by_size, sizeof(UndoRequestNode),
				   UndoRequestNodeCompareSize, UndoRequestNodeCombine,
				   UndoRequestNodeAllocate, UndoRequestNodeFree, urm);
	rbt_initialize(&urm->requests_by_retry_time, sizeof(UndoRequestNode),
				   UndoRequestNodeCompareRetryTime, UndoRequestNodeCombine,
				   UndoRequestNodeAllocate, UndoRequestNodeFree, urm);
	urm->oldest_fxid_valid = true;
	urm->oldest_fxid = InvalidFullTransactionId;

	/* Find memory for UndoRequest and UndoRequestNode arenas. */
	reqs = (UndoRequest *)
		(((char *) urm) + MAXALIGN(sizeof(UndoRequestManager)));
	urm->all_requests = reqs;
	nodes = (UndoRequestNode *)
		(((char *) reqs) + MAXALIGN(capacity * sizeof(UndoRequest)));

	/* Build a free list of UndoRequest objects.  */
	urm->first_free_request = reqs;
	for (i = 0; i < capacity - 1; ++i)
	{
		UndoRequest *current = &reqs[i];
		UndoRequest *next = &reqs[i + 1];

		current->next_free_request = next;
	}
	reqs[capacity - 1].next_free_request = NULL;

	/*
	 * Similarly, build a free list of UndoRequestNode objects.  In this case,
	 * we use the first few bytes of the free object to store a pointer to the
	 * next free object.
	 */
	StaticAssertStmt(sizeof(UndoRequestNode) >= sizeof(UndoRequestNode *),
					 "UndoRequestNode is too small");
	urm->first_free_request_node = nodes;
	for (i = 0; i < 2 * capacity - 1; ++i)
	{
		UndoRequestNode *current = &nodes[i];
		UndoRequestNode *next = &nodes[i + 1];

		*(UndoRequestNode **) current = next;
	}
	*(UndoRequestNode **) &nodes[2 * capacity - 1] = NULL;
}

/*
 * Register a new undo request. If unable, returns NULL.
 *
 * This function should be called before a transaction first writes any undo;
 * at end of transaction, the caller call either UnregisterUndoRequest (on
 * commit) or FinalizeUndoRequest (on abort).
 *
 * The returned request is UNLISTED (as defined above).
 */
UndoRequest *
RegisterUndoRequest(UndoRequestManager *urm, FullTransactionId fxid, Oid dbid)
{
	UndoRequest *req;

	LWLockAcquire(urm->lock, LW_EXCLUSIVE);

	req = urm->first_free_request;
	if (req != NULL)
	{
		/* Pop free list. */
		urm->first_free_request = req->next_free_request;
		req->next_free_request = NULL;

		/* Increase utilization. */
		++urm->utilization;

		/* Initialize request object. */
		req->d.fxid = fxid;
		req->d.dbid = dbid;
		req->d.size = 0;
		req->d.start_location_logged = InvalidUndoRecPtr;
		req->d.end_location_logged = InvalidUndoRecPtr;
		req->d.start_location_unlogged = InvalidUndoRecPtr;
		req->d.end_location_unlogged = InvalidUndoRecPtr;
		req->retry_time = DT_NOBEGIN;

		/* Save this fxid as the oldest one, if necessary. */
		if (urm->oldest_fxid_valid &&
			(!FullTransactionIdIsValid(urm->oldest_fxid)
			 || FullTransactionIdPrecedes(fxid, urm->oldest_fxid)))
			urm->oldest_fxid = fxid;
	}

	LWLockRelease(urm->lock);

	return req;
}

/*
 * Finalize details for an undo request.
 *
 * Since an UndoRequest should be registered before beginning to write undo,
 * the undo size won't be known at that point; this function should be getting
 * called at prepare time for a prepared transaction, or at abort time
 * otherwise, by which point the size should be known.
 *
 * Caller should report the total size of generated undo in bytes, counting
 * only logged and unlogged undo that will be processed by background workers.
 * Any undo bytes that aren't part of the logged or unlogged undo records
 * that may need cleanup actions performed should not be included in size;
 * for example, temporary undo doesn't count, as the caller must deal with
 * that outside of this mechanism.
 *
 * Caller must also pass the end location for logged and unlogged undo;
 * each should be if InvalidUndoRecPtr if and only if the corresponding
 * start location was never set.
 *
 * We don't need a lock here, because this request must be UNLISTED (as
 * defined above).
 */
void
FinalizeUndoRequest(UndoRequestManager *urm, UndoRequest *req, Size size,
					UndoRecPtr start_location_logged,
					UndoRecPtr start_location_unlogged,
					UndoRecPtr end_location_logged,
					UndoRecPtr end_location_unlogged)
{
	Assert(size != 0);
	Assert(UndoRecPtrIsValid(end_location_logged) ||
		   UndoRecPtrIsValid(end_location_unlogged));
	Assert(UndoRecPtrIsValid(end_location_logged) ==
		   UndoRecPtrIsValid(start_location_logged));
	Assert(UndoRecPtrIsValid(end_location_unlogged) ==
		   UndoRecPtrIsValid(start_location_unlogged));
	req->d.size = size;
	req->d.start_location_logged = start_location_logged;
	req->d.start_location_unlogged = start_location_unlogged;
	req->d.end_location_logged = end_location_logged;
	req->d.end_location_unlogged = end_location_unlogged;
}

/*
 * Release a previously-allocated undo request.
 *
 * On entry, the undo request should be either LISTED or UNLISTED; on exit,
 * it will be FREE (as these terms are defined above).
 *
 * This should be used at transaction commit, if an UndoRequest was
 * registered, or when undo for an aborted transaction has been succesfully
 * processed.
 *
 * Because this function may be called as a post-commit step, it must never
 * throw an ERROR.
 */
void
UnregisterUndoRequest(UndoRequestManager *urm, UndoRequest *req)
{
	LWLockAcquire(urm->lock, LW_EXCLUSIVE);

	/*
	 * Remove the UndoRequest from any RBTree that contains it.  If the retry
	 * time is not DT_NOBEGIN, then the request has been finalized and undo
	 * has subsequently failed.  If the size is 0, the request has not been
	 * finalized yet, so it's not in any RBTree.
	 */
	if (req->retry_time != DT_NOBEGIN)
		RemoveUndoRequest(&urm->requests_by_retry_time, req);
	else if (req->d.size != 0)
	{
		RemoveUndoRequest(&urm->requests_by_fxid, req);
		RemoveUndoRequest(&urm->requests_by_size, req);
	}

	/* Plan to recompute oldest_fxid, if necessary. */
	if (FullTransactionIdEquals(req->d.fxid, urm->oldest_fxid))
		urm->oldest_fxid_valid = false;

	/* Push onto freelist. */
	req->next_free_request = urm->first_free_request;
	urm->first_free_request = req;

	/* Decrease utilization. */
	--urm->utilization;

	LWLockRelease(urm->lock);
}

/*
 * Try to hand an undo request off for background processing.
 *
 * If this function returns true, the UndoRequest can be left for background
 * processing; the caller need not do anything more. If this function returns
 * false, the caller should try to process it in the foreground, and must
 * call either UnregisterUndoRequest on success or RescheduleUndoRequest
 * on failure.
 *
 * If 'force' is true, it indicates that foreground undo is impossible and
 * the request *must* be pushed into the background. This option should be
 * used as sparingly as possible for feature of exhausting the capacity of
 * the UndoRequestManager.
 *
 * Because this function may be called as during transaction abort, it must
 * never throw an ERROR. Technically, InsertUndoRequest might reach
 * UndoRequestNodeAllocate which could ERROR if the freelist is empty, but
 * if that happens there's a bug someplace.
 *
 * On entry, the UndoRequest should be UNLISTED; on exit, it is LISTED
 * if this function returns true, and remains UNLISTED if this function
 * returns false (see above for definitions).
 */
bool
PerformUndoInBackground(UndoRequestManager *urm, UndoRequest *req, bool force)
{
	bool		background;

	/*
	 * If we failed after allocating an UndoRequest but before setting any
	 * start locations, there's no work to be done.  In that case, we can just
	 * unregister the request.
	 */
	if (!UndoRecPtrIsValid(req->d.start_location_logged) &&
		!UndoRecPtrIsValid(req->d.start_location_unlogged))
	{
		UnregisterUndoRequest(urm, req);
		return true;
	}

	/*
	 * We need to check shared state in order to determine whether or not to
	 * perform this undo in the background, and if we are going to perform it
	 * in the background, also to add it to requests_by_fxid and
	 * requests_by_size.
	 */
	LWLockAcquire(urm->lock, LW_EXCLUSIVE);
	background = force || BackgroundUndoOK(urm, req);
	if (background)
	{
		/*
		 * We're going to handle this in the background, so add it to
		 * requests_by_fxid and requests_by_size, so that GetNextUndoRequest
		 * can find it.
		 */
		InsertUndoRequest(&urm->requests_by_fxid, req);
		InsertUndoRequest(&urm->requests_by_size, req);
	}
	LWLockRelease(urm->lock);

	return background;
}

/*
 * Get an undo request that needs background processing.
 *
 * Unless dbid is InvalidOid, any request returned must be from the indicated
 * database.  If minimum_runtime_reached is true, the caller only wants to
 * process another request if the next request happens to be from the correct
 * database. If it's false, the caller wants to avoiding exiting too quickly,
 * and would like to process a request from the database if there's one
 * available.
 *
 * If no suitable request is found, *fxid gets InvalidFullTransactionId;
 * otherwise, *fxid gets the FullTransactionId of the transaction and
 * the parameters which follow get the start and end locations of logged
 * and unlogged undo for that transaction.  It's possible that the transaction
 * wrote only logged undo or only unlogged undo, in which case the other
 * pair fields will have a value of InvalidUndoRecPtr, but it should never
 * happen that all of the fields get InvalidUndoRecPtr, because that would
 * mean we queued up an UndoRequest to do nothing.
 *
 * This function, as a side effect, makes the returned UndoRequest UNLISTED,
 * as defined above, so that no other backend will attempt to process it
 * simultaneously. The caller must be certain to call either
 * UnregisterUndoRequest (if successful) or RescheduleUndoRequest (on
 * failure) to avoid leaking the UndoRequest.
 */
UndoRequest *
GetNextUndoRequest(UndoRequestManager *urm, Oid dbid,
				   bool minimum_runtime_reached,
				   Oid *out_dbid, FullTransactionId *fxid,
				   UndoRecPtr *start_location_logged,
				   UndoRecPtr *end_location_logged,
				   UndoRecPtr *start_location_unlogged,
				   UndoRecPtr *end_location_unlogged)
{
	UndoRequest *req = NULL;
	int			nloops;
	bool		saw_db_mismatch = false;

	LWLockAcquire(urm->lock, LW_EXCLUSIVE);

	/* Some might have no work, so loop until all are checked. */
	for (nloops = 0; nloops < 3; ++nloops)
	{
		RBTree	   *rbt;
		UndoRequestSource source = urm->source;
		UndoRequestNode *node;

		/*
		 * We rotate between the three possible sources of UndoRequest
		 * objects.
		 *
		 * The idea here is that processing the requests with the oldest
		 * transaction IDs is important because it helps us discard undo log
		 * data sooner and because it allows XID horizons to advance. On the
		 * other hand, handling transactions that generated a very large
		 * amount of undo is also a priority, because undo will probably take
		 * a long to finish and thus should be started as early as possible
		 * and also because it likely touched a large number of pages which
		 * will be slow to access until the undo is processed.
		 *
		 * However, we also need to make sure to periodically retry undo for
		 * transactions that previously failed. We hope that this will be very
		 * rare, but if it does happen we can neither affort to retry those
		 * transactions over and over in preference to all others, nor on the
		 * other hand to just ignore them forever.
		 *
		 * We could try to come up with some scoring system that assigns
		 * relative levels of importance to FullTransactionId age, undo size,
		 * and retry time, but it seems difficult to come up with a weighting
		 * system that can ensure that nothing gets starved. By rotating among
		 * the sources evenly, we know that as long as we continue to process
		 * undo requests on some sort of regular basis, each source will get
		 * some amount of attention.
		 */
		switch (source)
		{
			case UNDO_SOURCE_FXID:
				rbt = &urm->requests_by_fxid;
				urm->source = UNDO_SOURCE_SIZE;
				break;
			case UNDO_SOURCE_SIZE:
				rbt = &urm->requests_by_size;
				urm->source = UNDO_SOURCE_RETRY_TIME;
				break;
			case UNDO_SOURCE_RETRY_TIME:
				rbt = &urm->requests_by_retry_time;
				urm->source = UNDO_SOURCE_FXID;
				break;
		}

		/* Get highest-priority item. */
		node = (UndoRequestNode *) rbt_leftmost(rbt);
		if (node == NULL)
			continue;

		/*
		 * We can only take an item from the retry time RBTree if the retry
		 * time is in the past.
		 */
		if (source == UNDO_SOURCE_RETRY_TIME &&
			node->req->retry_time > GetCurrentTimestamp())
			continue;

		/*
		 * If a database OID was specified, it must match. If it does not, we
		 * go ahead and try any remaining RBTree.  Note that this needs to be
		 * after the other tests so that we get the right value for the
		 * saw_db_mismatch flag.
		 */
		if (OidIsValid(dbid) && node->req->d.dbid != dbid)
		{
			saw_db_mismatch = true;
			continue;
		}

		/* Looks like we have a winner. */
		req = node->req;
		break;
	}

	/*
	 * Determine whether we should do a more exhaustive search.
	 *
	 * If we found a node, we don't need look any harder.  If we didn't see a
	 * database mismatch, then looking harder can't help: there's nothing to
	 * do at all, never mind for which database.  If the caller set
	 * minimum_runtime_reached, then they don't want us to look harder.
	 */
	if (req == NULL && saw_db_mismatch && !minimum_runtime_reached)
		req = FindUndoRequestForDatabase(urm, dbid);

	/*
	 * If we found a suitable request, remove it from any RBTree that contains
	 * it.
	 */
	if (req != NULL)
	{
		if (req->retry_time != DT_NOBEGIN)
			RemoveUndoRequest(&urm->requests_by_retry_time, req);
		else
		{
			RemoveUndoRequest(&urm->requests_by_fxid, req);
			RemoveUndoRequest(&urm->requests_by_size, req);
		}
	}

	LWLockRelease(urm->lock);

	/*
	 * Set output parameters.  Any request we found is now UNLISTED, so it's
	 * safe to do this without the lock.
	 */
	if (req == NULL)
		*out_dbid = InvalidOid;
	else
	{
		*out_dbid = req->d.dbid;
		*fxid = req->d.fxid;
		*start_location_logged = req->d.start_location_logged;
		*end_location_logged = req->d.end_location_logged;
		*start_location_unlogged = req->d.start_location_unlogged;
		*end_location_unlogged = req->d.end_location_unlogged;
	}

	/* All done. */
	return req;
}

/*
 * Reschedule an undo request after undo failure.
 *
 * This function should be called when undo processing fails, either in the
 * foreground or in the background.  The foreground case occurs when
 * FinalizeUndoRequest returns false and undo then also fails; the background
 * case occurs when GetNextUndoRequest returns an UndoRequest and undo then
 * fails. Note that this function isn't used after a shutdown or crash: see
 * comments in RecreateUndoRequest for how we handle that case.
 *
 * In either of the cases where this function is reached, the UndoRequest
 * should be UNLISTED; on return, it will be LISTED (both as defined above).
 * If it's a foreground undo failure, it's never been LISTED; if it's a
 * background undo failure, it was made UNLISTED by GetNextUndoRequest. So,
 * we don't have to remove the request from anywhere, not even conditionally;
 * we just need to add it to the set of failed requests.
 *
 * Because this function may be called as during transaction abort, it must
 * never throw an ERROR. Technically, InsertUndoRequest might reach
 * UndoRequestNodeAllocate which could ERROR if the freelist is empty, but
 * if that happens there's a bug someplace.
 */
void
RescheduleUndoRequest(UndoRequestManager *urm, UndoRequest *req)
{
	LWLockAcquire(urm->lock, LW_EXCLUSIVE);

	/*
	 * This algorithm for determining the next retry time is fairly
	 * unsophisticated: the first retry happens after 10 seconds, and each
	 * subsequent retry after 30 seconds. We could do something more
	 * complicated here, but we'd need to do more bookkeeping and it's unclear
	 * what we'd gain.
	 */
	if (req->retry_time == DT_NOBEGIN)
		req->retry_time =
			TimestampTzPlusMilliseconds(GetCurrentTimestamp(), 10 * 1000);
	else
		req->retry_time =
			TimestampTzPlusMilliseconds(GetCurrentTimestamp(), 30 * 1000);

	InsertUndoRequest(&urm->requests_by_retry_time, req);
	LWLockRelease(urm->lock);
}

/*
 * Serialize state that needs to survive a shutdown.
 *
 * We don't worry about saving the retry time; see the comments in
 * RestoreUndoRequestData for further details.
 *
 * We only need to save data for LISTED undo requests. An UNLISTED request
 * doesn't necessarily contain fully valid data yet, and a FREE request
 * certainly doesn't.
 *
 * The return value is a pointer to the serialized data; *nbytes is set to
 * the length of that data. The serialized data is allocated in the current
 * memory context and the caller may free it using pfree if desired.
 */
char *
SerializeUndoRequestData(UndoRequestManager *urm, Size *nbytes)
{
	UndoRequestData *darray;
	RBTreeIterator iter;
	int		nrequests = 0;
	int		i = 0;

	LWLockAcquire(urm->lock, LW_EXCLUSIVE);

	/* Count the number of LISTED requests. */
	rbt_begin_iterate(&urm->requests_by_fxid, LeftRightWalk, &iter);
	while (rbt_iterate(&iter) != NULL)
		++nrequests;
	rbt_begin_iterate(&urm->requests_by_retry_time, LeftRightWalk, &iter);
	while (rbt_iterate(&iter) != NULL)
		++nrequests;

	/* Allocate memory. */
	*nbytes = sizeof(UndoRequestData) * nrequests;
	darray = palloc(*nbytes);

	/* Save requests. */
	rbt_begin_iterate(&urm->requests_by_fxid, LeftRightWalk, &iter);
	while (rbt_iterate(&iter) != NULL)
	{
		UndoRequestNode *node = (UndoRequestNode *) rbt_iterate(&iter);

		memcpy(&darray[i++], &node->req->d, sizeof(UndoRequestData));
	}
	rbt_begin_iterate(&urm->requests_by_retry_time, LeftRightWalk, &iter);
	while (rbt_iterate(&iter) != NULL)
	{
		UndoRequestNode *node = (UndoRequestNode *) rbt_iterate(&iter);

		memcpy(&darray[i++], &node->req->d, sizeof(UndoRequestData));
	}

	/* All done. */
	LWLockRelease(urm->lock);
	Assert(i == nrequests);
	return (char *) darray;
}

/*
 * Restore state previously saved by RestoreUndoRequestData.
 */
void
RestoreUndoRequestData(UndoRequestManager *urm, Size nbytes, char *data)
{
	UndoRequestData *darray = (UndoRequestData *) data;
	int			nrequests;
	int			i;

	/* Caller should have ensured a sane size, but let's double-check. */
	if (nbytes % sizeof(UndoRequestData) != 0)
		elog(ERROR, "undo request data size is corrupt");

	/* Compute number of requests and check capacity. */
	nrequests = nbytes / sizeof(UndoRequestData);
	if (nrequests > urm->capacity)
		ereport(ERROR,
				(errmsg("too many undo requests"),
				 errdetail("There are %d outstanding undo requests, but only enough shared memory for %zu requests.",
						   nrequests, urm->capacity),
				 errhint("Consider increasing max_connctions.")));

	/* Now we acquire the lock. */
	LWLockAcquire(urm->lock, LW_EXCLUSIVE);

	for (i = 0; i < nrequests; ++i)
	{
		UndoRequestData	   *d = &darray[i];
		UndoRequest		   *req;

		/* Allocate a request. */
		Assert(urm->first_free_request != NULL);
		req = urm->first_free_request;
		urm->first_free_request = req->next_free_request;

		/* Increase utilization. */
		++urm->utilization;

		/* Sanity checks. */
		Assert(FullTransactionIdIsValid(d->fxid));
		Assert(OidIsValid(d->dbid));
		Assert(d->size != 0);

		/*
		 * Populate data and list the request.
		 *
		 * List this request so that undo workers will see it.  Note that we
		 * assume that these are new aborts, but it's possible that there are
		 * actually a whole series of previous undo failures before the
		 * shutdown or crash. If we had the information about whether this
		 * request had failed previously, we could set req->retry_time and
		 * insert it into requests_by_retry_time rather than requests_by_fxid
		 * and requests_by_size, but it doesn't seem important to retain
		 * information about undo failure across crashes or shutdowns, because
		 * we're just trying to guarantee that we don't busy-loop or starve
		 * other requests. (FindUndoRequest would get confused, too.)
		 */
		memcpy(&req->d, d, sizeof(UndoRequestData));
		req->retry_time = DT_NOBEGIN;
		req->next_free_request = NULL;
		InsertUndoRequest(&urm->requests_by_fxid, req);
		InsertUndoRequest(&urm->requests_by_size, req);
	}

	/* All done. */
	LWLockRelease(urm->lock);
}

/*
 * Adjust UndoRequestManager state for prepared transactions.
 *
 * After a restart, once all calls to RecreateUndoRequest have been completed
 * and before the first call to GetNextUndoRequest, this function should
 * be called for each prepared transaction. That's necessary to avoid
 * prematurely executed undo actions for transactions that haven't aborted
 * yet and might go on to commit. The UndoRequest for the indicated fxid is
 * made UNLISTED (as defined above) so that GetNextUndoRequest does not find
 * them.
 *
 * The caller should retain a pointer to the returned UndoRequest and, when
 * the prepared transaction is eventually committed or rolled back, should
 * invoke UnregisterUndoRequest on commit or FinalizeUndoRequest on abort.
 */
UndoRequest *
SuspendPreparedUndoRequest(UndoRequestManager *urm, FullTransactionId fxid)
{
	UndoRequest *req;

	LWLockAcquire(urm->lock, LW_EXCLUSIVE);
	req = FindUndoRequest(urm, fxid);
	Assert(req != NULL);
	Assert(req->d.size != 0);
	RemoveUndoRequest(&urm->requests_by_fxid, req);
	RemoveUndoRequest(&urm->requests_by_size, req);
	LWLockRelease(urm->lock);

	return req;
}

/*
 * Get oldest registered FXID, whether LISTED or UNLISTED (as defined above).
 *
 * We cache the result of this computation so as to avoid repeating it too
 * often.
 */
FullTransactionId
UndoRequestManagerOldestFXID(UndoRequestManager *urm)
{
	FullTransactionId result = InvalidFullTransactionId;

	LWLockAcquire(urm->lock, LW_EXCLUSIVE);

	if (urm->oldest_fxid_valid)
		result = urm->oldest_fxid;
	else
	{
		int			i;

		for (i = 0; i < urm->capacity; ++i)
		{
			UndoRequest *req = &urm->all_requests[i];

			if (FullTransactionIdIsValid(req->d.fxid) &&
				(!FullTransactionIdIsValid(result) ||
				 FullTransactionIdPrecedes(req->d.fxid, result)))
				result = req->d.fxid;
		}

		urm->oldest_fxid = result;
		urm->oldest_fxid_valid = true;
	}

	LWLockRelease(urm->lock);

	return result;
}

/*
 * Perform a left-to-right search of all three RBTrees, looking for a request
 * for a given database. The searches are interleaved so that we latch
 * onto the highest-priority request in any RBTree.
 *
 * It's possible that we should have some kind of limit on this search, so
 * that it doesn't do an exhaustive search of every RBTree. However, it's not
 * exactly clear how that would affect the behavior, or how to pick a
 * reasonable limit.
 */
static UndoRequest *
FindUndoRequestForDatabase(UndoRequestManager *urm, Oid dbid)
{
	RBTreeIterator iter[3];
	int			doneflags = 0;
	int			i = 0;

	rbt_begin_iterate(&urm->requests_by_fxid, LeftRightWalk, &iter[0]);
	rbt_begin_iterate(&urm->requests_by_size, LeftRightWalk, &iter[1]);
	rbt_begin_iterate(&urm->requests_by_retry_time, LeftRightWalk, &iter[2]);

	while (1)
	{
		UndoRequestNode *node;

		if ((doneflags & (1 << i)) == 0)
		{
			node = (UndoRequestNode *) rbt_iterate(&iter[i]);
			if (node == NULL)
			{
				doneflags |= 1 << i;
				if (doneflags == 7) /* all bits set */
					break;
			}
			else if (node->req->d.dbid == dbid)
				return node->req;
		}
		i = (i + 1) % 3;
	}

	return NULL;
}

/*
 * Is it OK to handle this UndoRequest in the background?
 */
static bool
BackgroundUndoOK(UndoRequestManager *urm, UndoRequest *req)
{
	/*
	 * If we've passed the soft size limit, it's not OK to background it.
	 */
	if (urm->utilization > urm->soft_size_limit)
		return false;

	/*
	 * Otherwise, allow it.
	 *
	 * TODO: We probably want to introduce some additional rules here based on
	 * the size of the request.
	 */
	return true;
}

/*
 * RBTree callback to allocate an UndoRequestNode.
 *
 * Everything is preallocated, so we're just popping the freelist.
 */
static RBTNode *
UndoRequestNodeAllocate(void *arg)
{
	UndoRequestManager *urm = arg;
	UndoRequestNode *node = urm->first_free_request_node;

	/*
	 * Any LISTED UndoRequest should either be in both requests_by_fxid and
	 * requests_by_size, or it should be in requests_by_retry_time, or it
	 * should be in neither RBTree; consequently, it should be impossible to
	 * use more than 2 UndoRequestNode objects per UndoRequest. Since we
	 * preallocate that number, we should never run out. In case there's a bug
	 * in the logic, let's insert a runtime check here even when Asserts are
	 * disabled.
	 */
	if (node == NULL)
		elog(ERROR, "no free UndoRequestNode");

	/* Pop freelist. */
	urm->first_free_request_node = *(UndoRequestNode **) node;

	return &node->rbtnode;
}

/*
 * RBTree callback to free an UndoRequestNode.
 *
 * Just put it back on the freelist.
 */
static void
UndoRequestNodeFree(RBTNode *x, void *arg)
{
	UndoRequestManager *urm = arg;
	UndoRequestNode *node = (UndoRequestNode *) x;

	*(UndoRequestNode **) node = urm->first_free_request_node;
	urm->first_free_request_node = node;
}

/*
 * RBTree callback to combine an UndoRequestNode with another one.
 *
 * The key for every RBTree includes the FXID, which is unique, so it should
 * never happen that we need to merge requests.
 */
static void
UndoRequestNodeCombine(RBTNode *existing, const RBTNode *newdata, void *arg)
{
	elog(ERROR, "undo requests should never need to be combined");
}

/*
 * RBTree comparator for requests_by_retry_time. Older retry
 * times first; in the case of a tie, smaller FXIDs first.  This avoids ties,
 * which is important since we don't want to merge requests, and also favors
 * retiring older transactions first, which is generally desirable.
 */
static int
UndoRequestNodeCompareRetryTime(const RBTNode *a, const RBTNode *b, void *arg)
{
	const UndoRequestNode *aa = (UndoRequestNode *) a;
	const UndoRequestNode *bb = (UndoRequestNode *) b;
	FullTransactionId fxid_a = aa->req->d.fxid;
	FullTransactionId fxid_b = bb->req->d.fxid;
	TimestampTz retry_time_a = aa->req->retry_time;
	TimestampTz retry_time_b = bb->req->retry_time;

	if (retry_time_a != retry_time_b)
		return retry_time_a < retry_time_b ? -1 : 1;

	if (FullTransactionIdPrecedes(fxid_a, fxid_b))
		return -1;
	else if (FullTransactionIdPrecedes(fxid_b, fxid_a))
		return 1;
	else
		return 0;
}

/*
 * RBTree comparator for requests_by_size. Lower FXIDs first. No tiebreak,
 * because FXIDs should be unique.
 */
static int
UndoRequestNodeCompareFXID(const RBTNode *a, const RBTNode *b, void *arg)
{
	const UndoRequestNode *aa = (UndoRequestNode *) a;
	const UndoRequestNode *bb = (UndoRequestNode *) b;
	FullTransactionId fxid_a = aa->req->d.fxid;
	FullTransactionId fxid_b = bb->req->d.fxid;

	if (FullTransactionIdPrecedes(fxid_a, fxid_b))
		return -1;
	else if (FullTransactionIdPrecedes(fxid_b, fxid_a))
		return 1;
	else
		return 0;
}

/*
 * RBTree comparator for requests_by_size. As in we do for the retry
 * time RBTree, break ties in favor of lower FXIDs.
 */
static int
UndoRequestNodeCompareSize(const RBTNode *a, const RBTNode *b, void *arg)
{
	const UndoRequestNode *aa = (UndoRequestNode *) a;
	const UndoRequestNode *bb = (UndoRequestNode *) b;
	FullTransactionId fxid_a = aa->req->d.fxid;
	FullTransactionId fxid_b = bb->req->d.fxid;
	Size		size_a = aa->req->d.size;
	Size		size_b = bb->req->d.size;

	if (size_a != size_b)
		return size_a < size_b ? 1 : -1;

	if (FullTransactionIdPrecedes(fxid_a, fxid_b))
		return -1;
	else if (FullTransactionIdPrecedes(fxid_b, fxid_a))
		return 1;
	else
		return 0;
}

/*
 * Insert an UndoRequest into one RBTree.
 *
 * The actual RBTree element is an UndoRequestNode, which just points to
 * the actual UndoRequest.
 */
static void
InsertUndoRequest(RBTree *rbt, UndoRequest *req)
{
	UndoRequestNode dummy;
	bool		isNew;

	/*
	 * The rbt_insert interface is a bit strange: we have to pass something
	 * that looks like an RBTNode, but the RBTNode itself doesn't need to be
	 * initialized - only the "extra" data that follows the end of the
	 * structure needs to be correct.
	 */
	dummy.req = req;
	rbt_insert(rbt, &dummy.rbtnode, &isNew);
	Assert(isNew);
}

/*
 * Remove an UndoRequest from one RBTree.
 *
 * This is just the reverse of InsertUndoRequest, with the same interface
 * quirk.
 */
static void
RemoveUndoRequest(RBTree *rbt, UndoRequest *req)
{
	UndoRequestNode dummy;
	RBTNode    *node;

	dummy.req = req;
	node = rbt_find(rbt, &dummy.rbtnode);
	rbt_delete(rbt, node);
}

/*
 * Find an UndoRequest by FXID.
 *
 * If we needed to do this frequently, it might be worth maintaining a hash
 * table mapping FXID -> UndoRequest, but since we only need it after a system
 * restart, RBTree's O(lg n) performance seems good enough.
 *
 * Note that this can only find an UndoRequest that has not failed and is not
 * yet being processed, because a failed UndoRequest would be in
 * requests_by_retry_time, not requests_by_fxid, and an in-progress
 * UndoRequest wouldn't be in either data structure. That restriction, too,
 * is OK for current uses.
 */
static UndoRequest *
FindUndoRequest(UndoRequestManager *urm, FullTransactionId fxid)
{
	UndoRequest dummy_request;
	UndoRequestNode dummy_node;
	RBTNode    *node;

	/*
	 * Here we need both a dummy UndoRequest and a dummy UndoRequestNode; only
	 * the comparator will look at the dummy UndoRequestNode, and it will only
	 * look at UndoRequest, and specifically its FXID.
	 */
	dummy_request.d.fxid = fxid;
	dummy_node.req = &dummy_request;
	node = rbt_find(&urm->requests_by_fxid, &dummy_node.rbtnode);
	if (node == NULL)
		return NULL;
	return ((UndoRequestNode *) node)->req;
}
