/*-------------------------------------------------------------------------
 *
 * syncrep.c
 *
 * Synchronous replication is new as of PostgreSQL 9.1.
 *
 * If requested, transaction commits wait until their commit LSN is
 * acknowledged by the synchronous standbys.
 *
 * This module contains the code for waiting and release of backends.
 * All code in this module executes on the primary. The core streaming
 * replication transport remains within WALreceiver/WALsender modules.
 *
 * The essence of this design is that it isolates all logic about
 * waiting/releasing onto the primary. The primary defines which standbys
 * it wishes to wait for. The standby is completely unaware of the
 * durability requirements of transactions on the primary, reducing the
 * complexity of the code and streamlining both standby operations and
 * network bandwidth because there is no requirement to ship
 * per-transaction state information.
 *
 * Replication is either synchronous or not synchronous (async). If it is
 * async, we just fastpath out of here. If it is sync, then we wait for
 * the write or flush location on the standby before releasing the waiting
 * backend. Further complexity in that interaction is expected in later
 * releases.
 *
 * The best performing way to manage the waiting backends is to have a
 * single ordered queue of waiting backends, so that we can avoid
 * searching the through all waiters each time we receive a reply.
 *
 * In 9.1 we support only a single synchronous standby, chosen from a
 * priority list of synchronous_standby_names. Before it can become the
 * synchronous standby it must have caught up with the primary; that may
 * take some time. Once caught up, the current highest priority standby
 * will release waiters from the queue.
 * In 9.5 we support the possibility to have multiple synchronous standbys,
 * as defined in synchronous_standby_group. Before on standby can become a
 * synchronous standby it must have caught up with the primary;
 * that may take some time.
 *
 * Waiters will be released from the queue once the number of standbys
 * specified in synchronous_standby_group have caught.
 *
 * Portions Copyright (c) 2010-2016, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/syncrep.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/xact.h"
#include "miscadmin.h"
#include "replication/syncrep.h"
#include "replication/walsender.h"
#include "replication/walsender_private.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/ps_status.h"

/* User-settable parameters for sync rep */
SyncGroupNode	   *SyncRepStandbys;
char	   *SyncRepStandbyNamesString;

static bool announce_next_takeover = true;

static int	SyncRepWaitMode = SYNC_REP_NO_WAIT;

static void SyncRepQueueInsert(int mode);
static void SyncRepCancelWait(void);
static int	SyncRepWakeQueue(bool all, int mode);

static int	SyncRepGetStandbyPriority(void);
static int SyncRepFindWalSenderByName(char *name);
static void SyncRepClearStandbyGroupList(SyncGroupNode *node);
static bool SyncRepSyncedLsnAdvancedTo(XLogRecPtr *write_pos, XLogRecPtr *flush_pos);
static bool SyncRepStandbyIsSync(int pos);

#ifdef USE_ASSERT_CHECKING
static bool SyncRepQueueIsOrderedByLSN(int mode);
#endif

/*
 * ===========================================================
 * Synchronous Replication functions for normal user backends
 * ===========================================================
 */

/*
 * Wait for synchronous replication, if requested by user.
 *
 * Initially backends start in state SYNC_REP_NOT_WAITING and then
 * change that state to SYNC_REP_WAITING before adding ourselves
 * to the wait queue. During SyncRepWakeQueue() a WALSender changes
 * the state to SYNC_REP_WAIT_COMPLETE once replication is confirmed.
 * This backend then resets its state to SYNC_REP_NOT_WAITING.
 */
void
SyncRepWaitForLSN(XLogRecPtr XactCommitLSN)
{
	char	   *new_status = NULL;
	const char *old_status;
	int			mode = SyncRepWaitMode;

	/*
	 * Fast exit if user has not requested sync replication, or there are no
	 * sync replication standby names defined. Note that those standbys don't
	 * need to be connected.
	 */
	if (!SyncRepRequested() || !SyncStandbysDefined())
		return;

	Assert(SHMQueueIsDetached(&(MyProc->syncRepLinks)));
	Assert(WalSndCtl != NULL);

	LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);
	Assert(MyProc->syncRepState == SYNC_REP_NOT_WAITING);

	/*
	 * We don't wait for sync rep if WalSndCtl->sync_standbys_defined is not
	 * set.  See SyncRepUpdateSyncStandbysDefined.
	 *
	 * Also check that the standby hasn't already replied. Unlikely race
	 * condition but we'll be fetching that cache line anyway so it's likely
	 * to be a low cost check.
	 */
	if (!WalSndCtl->sync_standbys_defined ||
		XactCommitLSN <= WalSndCtl->lsn[mode])
	{
		LWLockRelease(SyncRepLock);
		return;
	}

	/*
	 * Set our waitLSN so WALSender will know when to wake us, and add
	 * ourselves to the queue.
	 */
	MyProc->waitLSN = XactCommitLSN;
	MyProc->syncRepState = SYNC_REP_WAITING;
	SyncRepQueueInsert(mode);
	Assert(SyncRepQueueIsOrderedByLSN(mode));
	LWLockRelease(SyncRepLock);

	/* Alter ps display to show waiting for sync rep. */
	if (update_process_title)
	{
		int			len;

		old_status = get_ps_display(&len);
		new_status = (char *) palloc(len + 32 + 1);
		memcpy(new_status, old_status, len);
		sprintf(new_status + len, " waiting for %X/%X",
				(uint32) (XactCommitLSN >> 32), (uint32) XactCommitLSN);
		set_ps_display(new_status, false);
		new_status[len] = '\0'; /* truncate off " waiting ..." */
	}

	/*
	 * Wait for specified LSN to be confirmed.
	 *
	 * Each proc has its own wait latch, so we perform a normal latch
	 * check/wait loop here.
	 */
	for (;;)
	{
		int			syncRepState;

		/* Must reset the latch before testing state. */
		ResetLatch(MyLatch);

		/*
		 * Try checking the state without the lock first.  There's no
		 * guarantee that we'll read the most up-to-date value, so if it looks
		 * like we're still waiting, recheck while holding the lock.  But if
		 * it looks like we're done, we must really be done, because once
		 * walsender changes the state to SYNC_REP_WAIT_COMPLETE, it will
		 * never update it again, so we can't be seeing a stale value in that
		 * case.
		 */
		syncRepState = MyProc->syncRepState;
		if (syncRepState == SYNC_REP_WAITING)
			syncRepState = MyProc->syncRepState;
		if (syncRepState == SYNC_REP_WAIT_COMPLETE)
			break;

		/*
		 * If a wait for synchronous replication is pending, we can neither
		 * acknowledge the commit nor raise ERROR or FATAL.  The latter would
		 * lead the client to believe that the transaction aborted, which
		 * is not true: it's already committed locally. The former is no good
		 * either: the client has requested synchronous replication, and is
		 * entitled to assume that an acknowledged commit is also replicated,
		 * which might not be true. So in this case we issue a WARNING (which
		 * some clients may be able to interpret) and shut off further output.
		 * We do NOT reset ProcDiePending, so that the process will die after
		 * the commit is cleaned up.
		 */
		if (ProcDiePending)
		{
			ereport(WARNING,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("canceling the wait for synchronous replication and terminating connection due to administrator command"),
					 errdetail("The transaction has already committed locally, but might not have been replicated to the standby(s).")));
			whereToSendOutput = DestNone;
			SyncRepCancelWait();
			break;
		}

		/*
		 * It's unclear what to do if a query cancel interrupt arrives.  We
		 * can't actually abort at this point, but ignoring the interrupt
		 * altogether is not helpful, so we just terminate the wait with a
		 * suitable warning.
		 */
		if (QueryCancelPending)
		{
			QueryCancelPending = false;
			ereport(WARNING,
					(errmsg("canceling wait for synchronous replication due to user request"),
					 errdetail("The transaction has already committed locally, but might not have been replicated to the standby(s).")));
			SyncRepCancelWait();
			break;
		}

		/*
		 * If the postmaster dies, we'll probably never get an
		 * acknowledgement, because all the wal sender processes will exit. So
		 * just bail out.
		 */
		if (!PostmasterIsAlive())
		{
			ProcDiePending = true;
			whereToSendOutput = DestNone;
			SyncRepCancelWait();
			break;
		}

		/*
		 * Wait on latch.  Any condition that should wake us up will set the
		 * latch, so no need for timeout.
		 */
		WaitLatch(MyLatch, WL_LATCH_SET | WL_POSTMASTER_DEATH, -1);
	}

	/*
	 * WalSender has checked our LSN and has removed us from queue. Clean up
	 * state and leave.  It's OK to reset these shared memory fields without
	 * holding SyncRepLock, because any walsenders will ignore us anyway when
	 * we're not on the queue.
	 */
	Assert(SHMQueueIsDetached(&(MyProc->syncRepLinks)));
	MyProc->syncRepState = SYNC_REP_NOT_WAITING;
	MyProc->waitLSN = 0;

	if (new_status)
	{
		/* Reset ps display */
		set_ps_display(new_status, false);
		pfree(new_status);
	}
}

/*
 * Insert MyProc into the specified SyncRepQueue, maintaining sorted invariant.
 *
 * Usually we will go at tail of queue, though it's possible that we arrive
 * here out of order, so start at tail and work back to insertion point.
 */
static void
SyncRepQueueInsert(int mode)
{
	PGPROC	   *proc;

	Assert(mode >= 0 && mode < NUM_SYNC_REP_WAIT_MODE);
	proc = (PGPROC *) SHMQueuePrev(&(WalSndCtl->SyncRepQueue[mode]),
								   &(WalSndCtl->SyncRepQueue[mode]),
								   offsetof(PGPROC, syncRepLinks));

	while (proc)
	{
		/*
		 * Stop at the queue element that we should after to ensure the queue
		 * is ordered by LSN.
		 */
		if (proc->waitLSN < MyProc->waitLSN)
			break;

		proc = (PGPROC *) SHMQueuePrev(&(WalSndCtl->SyncRepQueue[mode]),
									   &(proc->syncRepLinks),
									   offsetof(PGPROC, syncRepLinks));
	}

	if (proc)
		SHMQueueInsertAfter(&(proc->syncRepLinks), &(MyProc->syncRepLinks));
	else
		SHMQueueInsertAfter(&(WalSndCtl->SyncRepQueue[mode]), &(MyProc->syncRepLinks));
}

/*
 * Acquire SyncRepLock and cancel any wait currently in progress.
 */
static void
SyncRepCancelWait(void)
{
	LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);
	if (!SHMQueueIsDetached(&(MyProc->syncRepLinks)))
		SHMQueueDelete(&(MyProc->syncRepLinks));
	MyProc->syncRepState = SYNC_REP_NOT_WAITING;
	LWLockRelease(SyncRepLock);
}

void
SyncRepCleanupAtProcExit(void)
{
	if (!SHMQueueIsDetached(&(MyProc->syncRepLinks)))
	{
		LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);
		SHMQueueDelete(&(MyProc->syncRepLinks));
		LWLockRelease(SyncRepLock);
	}
}

/*
 * Clear all node in SyncRepStandbys recursively.
 */
static void
SyncRepClearStandbyGroupList(SyncGroupNode *group)
{
	SyncGroupNode *node = group->members;

	while (node != NULL)
	{
		SyncGroupNode *tmp = node->next;

		free(node);
		node = tmp;
	}
}


/*
 * ===========================================================
 * Synchronous Replication functions for wal sender processes
 * ===========================================================
 */

/*
 * Take any action required to initialise sync rep state from config
 * data. Called at WALSender startup and after each SIGHUP.
 */
void
SyncRepInitConfig(void)
{
	int			priority;

	/*
	 * Determine if we are a potential sync standby and remember the result
	 * for handling replies from standby.
	 */
	priority = SyncRepGetStandbyPriority();
	if (MyWalSnd->sync_standby_priority != priority)
	{
		char *walsnd_name;
		LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);
		MyWalSnd->sync_standby_priority = priority;
		walsnd_name = (char *)MyWalSnd->name;
		memcpy(walsnd_name, application_name, sizeof(MyWalSnd->name));
		LWLockRelease(SyncRepLock);
		ereport(DEBUG1,
			(errmsg("standby \"%s\" now has synchronous standby priority %u",
					application_name, priority)));
	}
}

/*
 * Check whether specified standby is active, which means not only having
 * pid but also having any priority.
 */
static bool
SyncRepStandbyIsSync(int pos)
{
	volatile WalSnd *walsnd = &WalSndCtl->walsnds[pos];

	/* Must be active */
	if (walsnd->pid == 0)
		return false;

	/* Must be streaming */
	if (walsnd->state != WALSNDSTATE_STREAMING)
		return false;

	/* Must be synchronous */
	if (walsnd->sync_standby_priority == 0)
		return false;

	/* Must have a valid flush position */
	if (XLogRecPtrIsInvalid(walsnd->flush))
		return false;

	return true;
}

/*
 * Finds the first active synchronous walsender with given name in
 * WalSndCtl->walsnds and returns the index of that. Returns -1 if not found.
 */
static int
SyncRepFindWalSenderByName(char *name)
{
	int	i;

	for (i = 0; i < max_wal_senders; i++)
	{
		/* Use volatile pointer to prevent code rearrangement */
		volatile WalSnd *walsnd = &WalSndCtl->walsnds[i];
		char *walsnd_name = (char *) walsnd->name;

		if (!SyncRepStandbyIsSync(i))
			continue;

		/* Compare wal sender name */
		if (pg_strcasecmp(name, walsnd_name) == 0)
			return i; /* Found */
	}

	/* Not found */
	return -1;
}

/*
 * Update the LSNs on each queue based upon our latest state. This
 * implements a simple policy of first-valid-standby-releases-waiter.
 *
 * Other policies are possible, which would change what we do here and what
 * perhaps also which information we store as well.
 */
void
SyncRepReleaseWaiters(void)
{
	volatile WalSndCtlData *walsndctl = WalSndCtl;
	XLogRecPtr	write_pos = InvalidXLogRecPtr;
	XLogRecPtr	flush_pos = InvalidXLogRecPtr;
	int			numwrite = 0;
	int			numflush = 0;

	/*
	 * If this WALSender is serving a standby that is not on the list of
	 * potential standbys then we have nothing to do. If we are still starting
	 * up, still running base backup or the current flush position is still
	 * invalid, then leave quickly also.
	 */
	if (MyWalSnd->sync_standby_priority == 0 ||
		MyWalSnd->state < WALSNDSTATE_STREAMING ||
		XLogRecPtrIsInvalid(MyWalSnd->flush))
		return;

	LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);

	if (!(SyncRepSyncedLsnAdvancedTo(&write_pos, &flush_pos)))
	{
		LWLockRelease(SyncRepLock);
		return;
	}

	/*
	 * Set the lsn first so that when we wake backends they will release up to
	 * this location.
	 */
	if (walsndctl->lsn[SYNC_REP_WAIT_WRITE] < write_pos)
	{
		walsndctl->lsn[SYNC_REP_WAIT_WRITE] = write_pos;
		numwrite = SyncRepWakeQueue(false, SYNC_REP_WAIT_WRITE);
	}
	if (walsndctl->lsn[SYNC_REP_WAIT_FLUSH] < flush_pos)
	{
		walsndctl->lsn[SYNC_REP_WAIT_FLUSH] = flush_pos;
		numflush = SyncRepWakeQueue(false, SYNC_REP_WAIT_FLUSH);
	}

	LWLockRelease(SyncRepLock);

	elog(DEBUG3, "released %d procs up to write %X/%X, %d procs up to flush %X/%X",
		 numwrite, (uint32) (MyWalSnd->write >> 32), (uint32) MyWalSnd->write,
	   numflush, (uint32) (MyWalSnd->flush >> 32), (uint32) MyWalSnd->flush);

	/*
	 * If we are managing the highest priority standby, though we weren't
	 * prior to this, then announce we are now the sync standby.
	 */
	if (announce_next_takeover)
	{
		announce_next_takeover = false;
		ereport(LOG,
				(errmsg("standby \"%s\" is now the synchronous standby with priority %u",
						application_name, MyWalSnd->sync_standby_priority)));
	}
}

/*
 * Return true if we have enough synchrononized standbys and the 'safe' written
 * flushed LSNs, which are LSNs assured in all standbys considered should be
 * synchronized.
 */
static bool
SyncRepSyncedLsnAdvancedTo(XLogRecPtr *write_pos, XLogRecPtr *flush_pos)
{
	XLogRecPtr	safe_write_pos;
	XLogRecPtr	safe_flush_pos;
	bool		got_lsns;

	/* Get synced LSNs at this moment */
	got_lsns = SyncRepStandbys->SyncRepGetSyncedLsnsFn(SyncRepStandbys,
													  &safe_write_pos,
													  &safe_flush_pos);
	if (!got_lsns)
		return false;

	/* Check whether each LSN has advanced to */
	if (MyWalSnd->write >= safe_write_pos)
		*write_pos = safe_write_pos;
	if (MyWalSnd->flush >= safe_flush_pos)
		*flush_pos = safe_flush_pos;

	return true;
}

/*
 * Decide synced LSNs at this moment using priority method.
 * If there are not active standbys enough to determine LSNs, return false.
 */
bool
SyncRepGetSyncedLsnsUsingPriority(SyncGroupNode *group, XLogRecPtr *write_pos, XLogRecPtr *flush_pos)
{
	int	*sync_list = (int *)palloc(sizeof(int) * group->wait_num);
	int	sync_num;
	int i;

	/* Get standbys list that are considered as synchronous at this moment */
	sync_num = group->SyncRepGetSyncStandbysFn(group, sync_list);

	/* If we could not get standbys enough, return false */
	if (sync_num != group->wait_num)
		return false;

	*write_pos = InvalidXLogRecPtr;
	*flush_pos = InvalidXLogRecPtr;

	/*
	 * In priority method, we seek the lowest each LSNs(write, flush) from
	 * standbys which are considered as synchronous.
	 */
	for (i = 0; i < sync_num; i++)
	{
		volatile WalSnd *walsnd = &WalSndCtl->walsnds[i];
		XLogRecPtr	write;
		XLogRecPtr	flush;

		SpinLockAcquire(&walsnd->mutex);
		write = walsnd->write;
		flush = walsnd->flush;
		SpinLockRelease(&walsnd->mutex);

		if (XLogRecPtrIsInvalid(*write_pos) || *write_pos > write)
			*write_pos = write;
		if (XLogRecPtrIsInvalid(*flush_pos) || *flush_pos > flush)
			*flush_pos = flush;
	}

	return true;
}

/*
 * Return the positions of the first group->wait_num synchronized standbys
 * in group->member list into sync_list. sync_list is assumed to have enough
 * space for at least group->wait_num elements.
 */
int
SyncRepGetSyncStandbysUsingPriority(SyncGroupNode *group, int *sync_list)
{
	SyncGroupNode	*node;
	int	num = 0;

	for (node = group->members; node != NULL; node = node->next)
	{
		int	pos = -1;

		/* We got enough synchronous standbys, return */
		if (num >= group->wait_num)
			return num;

		if (pg_strcasecmp(node->name, "*") != 0)
		{
			pos = SyncRepFindWalSenderByName(node->name);

			/* Could not find wal sender by this name */
			if (pos == -1)
				continue;

			sync_list[num] = pos;
			num++;
		}
		else
		{
			/*
			 * '*' means that all remaining standbys up to group->wait_num
			 * are considered as synchronous. We allows user to use only '*'
			 * in synchronous_standby_names, or use '*' at last node of
			 * synchronous_standby_names.
			 */
			int i;

			for (i = 0; i < max_wal_senders; i++)
			{
				volatile WalSnd *walsnd = &WalSndCtl->walsnds[i];
				char *walsnd_name = (char *) walsnd->name;
				bool	already_listed = false;
				int j;

				/* We got enough synchronous standbys, return */
				if (num >= group->wait_num)
					return num;

				if (!SyncRepStandbyIsSync(i))
					continue;

				pos = SyncRepFindWalSenderByName(walsnd_name);

				/* Could not find wal sender by this name */
				if (pos == -1)
					continue;

				for (j = 0; j < num; j++)
				{
					if (sync_list[j] == pos)
					{
						already_listed = true;
						break;
					}
				}

				/* We already listed this standby, ignore */
				if (already_listed)
					continue;

				/* Add this position to list */
				sync_list[num] = pos;
				num++;
			}
		}
	}

	return num;
}

/*
 * Check if we are in the list of sync standbys, and if so, determine
 * priority sequence. Return priority if set, or zero to indicate that
 * we are not a potential sync standby.
 *
 * Compare the parameter SyncRepStandbys against the application_name
 * for this WALSender, or allow any name if we find a wildcard "*".
 */
static int
SyncRepGetStandbyPriority(void)
{
	int			priority = 0;
	bool		found = false;

	/*
	 * Since synchronous cascade replication is not allowed, we always set the
	 * priority of cascading walsender to zero.
	 */
	if (am_cascading_walsender)
		return 0;

	if (SyncStandbysDefined())
	{
		SyncGroupNode	*node;

		for (node = SyncRepStandbys->members; node != NULL; node = node->next)
		{
			priority++;

			if (pg_strcasecmp(node->name, application_name) == 0 ||
				pg_strcasecmp(node->name, "*") == 0)
			{
				found = true;
				break;
			}
		}
	}

	return (found ? priority : 0);
}

/*
 * Walk the specified queue from head.  Set the state of any backends that
 * need to be woken, remove them from the queue, and then wake them.
 * Pass all = true to wake whole queue; otherwise, just wake up to
 * the walsender's LSN.
 *
 * Must hold SyncRepLock.
 */
static int
SyncRepWakeQueue(bool all, int mode)
{
	volatile WalSndCtlData *walsndctl = WalSndCtl;
	PGPROC	   *proc = NULL;
	PGPROC	   *thisproc = NULL;
	int			numprocs = 0;

	Assert(mode >= 0 && mode < NUM_SYNC_REP_WAIT_MODE);
	Assert(SyncRepQueueIsOrderedByLSN(mode));

	proc = (PGPROC *) SHMQueueNext(&(WalSndCtl->SyncRepQueue[mode]),
								   &(WalSndCtl->SyncRepQueue[mode]),
								   offsetof(PGPROC, syncRepLinks));

	while (proc)
	{
		/*
		 * Assume the queue is ordered by LSN
		 */
		if (!all && walsndctl->lsn[mode] < proc->waitLSN)
			return numprocs;

		/*
		 * Move to next proc, so we can delete thisproc from the queue.
		 * thisproc is valid, proc may be NULL after this.
		 */
		thisproc = proc;
		proc = (PGPROC *) SHMQueueNext(&(WalSndCtl->SyncRepQueue[mode]),
									   &(proc->syncRepLinks),
									   offsetof(PGPROC, syncRepLinks));

		/*
		 * Set state to complete; see SyncRepWaitForLSN() for discussion of
		 * the various states.
		 */
		thisproc->syncRepState = SYNC_REP_WAIT_COMPLETE;

		/*
		 * Remove thisproc from queue.
		 */
		SHMQueueDelete(&(thisproc->syncRepLinks));

		/*
		 * Wake only when we have set state and removed from queue.
		 */
		SetLatch(&(thisproc->procLatch));

		numprocs++;
	}

	return numprocs;
}

/*
 * The checkpointer calls this as needed to update the shared
 * sync_standbys_defined flag, so that backends don't remain permanently wedged
 * if synchronous_standby_names is unset.  It's safe to check the current value
 * without the lock, because it's only ever updated by one process.  But we
 * must take the lock to change it.
 */
void
SyncRepUpdateSyncStandbysDefined(void)
{
	bool		sync_standbys_defined = SyncStandbysDefined();

	if (sync_standbys_defined != WalSndCtl->sync_standbys_defined)
	{
		LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);

		/*
		 * If synchronous_standby_names has been reset to empty, it's futile
		 * for backends to continue to waiting.  Since the user no longer
		 * wants synchronous replication, we'd better wake them up.
		 */
		if (!sync_standbys_defined)
		{
			int			i;

			for (i = 0; i < NUM_SYNC_REP_WAIT_MODE; i++)
				SyncRepWakeQueue(true, i);
		}

		/*
		 * Only allow people to join the queue when there are synchronous
		 * standbys defined.  Without this interlock, there's a race
		 * condition: we might wake up all the current waiters; then, some
		 * backend that hasn't yet reloaded its config might go to sleep on
		 * the queue (and never wake up).  This prevents that.
		 */
		WalSndCtl->sync_standbys_defined = sync_standbys_defined;

		LWLockRelease(SyncRepLock);
	}
}

#ifdef USE_ASSERT_CHECKING
static bool
SyncRepQueueIsOrderedByLSN(int mode)
{
	PGPROC	   *proc = NULL;
	XLogRecPtr	lastLSN;

	Assert(mode >= 0 && mode < NUM_SYNC_REP_WAIT_MODE);

	lastLSN = 0;

	proc = (PGPROC *) SHMQueueNext(&(WalSndCtl->SyncRepQueue[mode]),
								   &(WalSndCtl->SyncRepQueue[mode]),
								   offsetof(PGPROC, syncRepLinks));

	while (proc)
	{
		/*
		 * Check the queue is ordered by LSN and that multiple procs don't
		 * have matching LSNs
		 */
		if (proc->waitLSN <= lastLSN)
			return false;

		lastLSN = proc->waitLSN;

		proc = (PGPROC *) SHMQueueNext(&(WalSndCtl->SyncRepQueue[mode]),
									   &(proc->syncRepLinks),
									   offsetof(PGPROC, syncRepLinks));
	}

	return true;
}
#endif

/*
 * ===========================================================
 * Synchronous Replication functions executed by any process
 * ===========================================================
 */

bool
check_synchronous_standby_names(char **newval, void **extra, GucSource source)
{
	int	parse_rc;

	if (*newval != NULL && (*newval)[0] != '\0')
	{
		syncgroup_scanner_init(*newval);
		parse_rc = syncgroup_yyparse();

		if (parse_rc != 0)
		{
			GUC_check_errdetail("Invalid syntax");
			return false;
		}

		syncgroup_scanner_finish();

		/*
		 * Any additional validation of standby names should go here.
		 *
		 * Don't attempt to set WALSender priority because this is executed by
		 * postmaster at startup, not WALSender, so the application_name is not
		 * yet correctly set.
		 */

		/*
		 * syncgroup_yyparse sets the global SyncRepStandbys as side effect.
		 * But this function is required to just check, so frees SyncRepStandbyNanes
		 * once parsing parameter.
		 */
		SyncRepClearStandbyGroupList(SyncRepStandbys);
		SyncRepStandbys = NULL;
	}

	return true;
}

void
assign_synchronous_commit(int newval, void *extra)
{
	switch (newval)
	{
		case SYNCHRONOUS_COMMIT_REMOTE_WRITE:
			SyncRepWaitMode = SYNC_REP_WAIT_WRITE;
			break;
		case SYNCHRONOUS_COMMIT_REMOTE_FLUSH:
			SyncRepWaitMode = SYNC_REP_WAIT_FLUSH;
			break;
		default:
			SyncRepWaitMode = SYNC_REP_NO_WAIT;
			break;
	}
}

void
assign_synchronous_standby_names(const char *newval, void *extra)
{
	int	parse_rc;

	if (newval != NULL && newval[0] != '\0')
	{
		syncgroup_scanner_init(newval);
		parse_rc = syncgroup_yyparse();

		if (parse_rc != 0)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 (errmsg_internal("Invalid syntax. synchronous_standby_names parse returned %d",
									  parse_rc))));

		GUC_check_errdetail("Invalid syntax");
		syncgroup_scanner_finish();
	}
}
