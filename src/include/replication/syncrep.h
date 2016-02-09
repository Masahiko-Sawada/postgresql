/*-------------------------------------------------------------------------
 *
 * syncrep.h
 *	  Exports from replication/syncrep.c.
 *
 * Portions Copyright (c) 2010-2016, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/include/replication/syncrep.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _SYNCREP_H
#define _SYNCREP_H

#include "access/xlogdefs.h"
#include "utils/guc.h"

#define SyncRepRequested() \
	(max_wal_senders > 0 && synchronous_commit > SYNCHRONOUS_COMMIT_LOCAL_FLUSH)

/* SyncRepWaitMode */
#define SYNC_REP_NO_WAIT		-1
#define SYNC_REP_WAIT_WRITE		0
#define SYNC_REP_WAIT_FLUSH		1

#define NUM_SYNC_REP_WAIT_MODE	2

/* syncRepState */
#define SYNC_REP_NOT_WAITING		0
#define SYNC_REP_WAITING			1
#define SYNC_REP_WAIT_COMPLETE		2

/* SyncRepMethod */
#define SYNC_REP_METHOD_PRIORITY	0

/* SyncGroupNode */
#define SYNC_REP_GROUP_MAIN			0x01
#define SYNC_REP_GROUP_NAME			0x02
#define SYNC_REP_GROUP_GROUP		0x04

struct SyncGroupNode;
typedef struct SyncGroupNode SyncGroupNode;

struct	SyncGroupNode
{
	int		type;
	char	*name;
	SyncGroupNode	*next;

	/* For group ndoe */
	int sync_method;
	int	wait_num;
	SyncGroupNode	*member;
	bool (*SyncRepGetSyncedLsnsFn) (SyncGroupNode *group, XLogRecPtr *write_pos,
									XLogRecPtr *flush_pos);
	int (*SyncRepGetSyncStandbysFn) (SyncGroupNode *group, int *list);
};

/* user-settable parameters for synchronous replication */
extern char *SyncRepStandbyNames;
extern char *SyncRepStandbyGroupString;
extern SyncGroupNode	*SyncRepStandbyGroup;

/* called by user backend */
extern void SyncRepWaitForLSN(XLogRecPtr XactCommitLSN);

/* called at backend exit */
extern void SyncRepCleanupAtProcExit(void);

/* called by wal sender */
extern void SyncRepInitConfig(void);
extern void SyncRepReleaseWaiters(void);

/* called by checkpointer */
extern void SyncRepUpdateSyncStandbysDefined(void);

/* forward declaration to avoid pulling in walsender_private.h */
struct WalSnd;
extern struct WalSnd *SyncRepGetSynchronousStandby(void);

extern bool check_synchronous_standby_names(char **newval, void **extra, GucSource source);
extern void assign_synchronous_commit(int newval, void *extra);
extern bool	check_synchronous_standby_group(char **newval, void **extra, GucSource source);
extern void assign_synchronous_standby_group(const char *newval, void *extra);

/*
 * Internal functions for parsing the replication grammar, in syncgroup_gram.y and
 * syncgroup_scanner.l
 */
extern int  syncgroup_yyparse(void);
extern int  syncgroup_yylex(void);
extern void syncgroup_yyerror(const char *str) pg_attribute_noreturn();
extern void syncgroup_scanner_init(const char *query_string);
extern void syncgroup_scanner_finish(void);

/* function for synchronous replication group */
extern bool SyncRepGetSyncedLsnsPriority(SyncGroupNode *group,
								 XLogRecPtr *write_pos, XLogRecPtr *flush_pos);
extern int SyncRepGetSyncStandbysPriority(SyncGroupNode *group, int *sync_list);

extern Datum pg_stat_get_synchronous_replication_group(PG_FUNCTION_ARGS);
#endif   /* _SYNCREP_H */
