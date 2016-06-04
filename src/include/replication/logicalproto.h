/*-------------------------------------------------------------------------
 *
 * logicalproto.h
 *		logical replication protocol
 *
 * Copyright (c) 2015, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/include/replication/logicalproto.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LOGICAL_PROTO_H
#define LOGICAL_PROTO_H

#include "replication/reorderbuffer.h"
#include "utils/rel.h"

/*
 * Protocol capabilities
 *
 * LOGICAL_PROTO_VERSION_NUM is our native protocol and the greatest version
 * we can support. PGLOGICAL_PROTO_MIN_VERSION_NUM is the oldest version we
 * have backwards compatibility for. The client requests protocol version at
 * connect time.
 */
#define LOGICALREP_PROTO_MIN_VERSION_NUM 1
#define LOGICALREP_PROTO_VERSION_NUM 1

/* Tuple coming via logical replication. */
typedef struct LogicalRepTupleData
{
        char   *values[MaxTupleAttributeNumber];	/* value in out function format or NULL if values is NULL */
        bool    changed[MaxTupleAttributeNumber];	/* marker for changed/unchanged values */
} LogicalRepTupleData;

typedef uint32	LogicalRepRelId;

/* Relation information */
typedef struct LogicalRepRelation
{
	/* Info coming from the remote side. */
	LogicalRepRelId		remoteid;	/* unique id of the relation */
	char       *nspname;			/* schema name */
	char       *relname;			/* relation name */
    int         natts;				/* number of columns */
    char      **attnames;			/* column names */
} LogicalRepRelation;

extern char *logicalrep_build_options(List *publications);
extern void logicalrep_write_begin(StringInfo out,  ReorderBufferTXN *txn);
extern void logicalrep_read_begin(StringInfo in, XLogRecPtr *remote_lsn,
					  TimestampTz *committime, TransactionId *remote_xid);
extern void logicalrep_write_commit(StringInfo out, ReorderBufferTXN *txn,
						XLogRecPtr commit_lsn);
extern void logicalrep_read_commit(StringInfo in, XLogRecPtr *commit_lsn,
					   XLogRecPtr *end_lsn, TimestampTz *committime);
extern void logicalrep_write_origin(StringInfo out, const char *origin,
						XLogRecPtr origin_lsn);
extern char *logicalrep_read_origin(StringInfo in, XLogRecPtr *origin_lsn);
extern void logicalrep_write_insert(StringInfo out, Relation rel,
							 HeapTuple newtuple);
extern LogicalRepRelId logicalrep_read_insert(StringInfo in, LogicalRepTupleData *newtup);
extern void logicalrep_write_update(StringInfo out, Relation rel, HeapTuple oldtuple,
					   HeapTuple newtuple);
extern LogicalRepRelId logicalrep_read_update(StringInfo in, bool *hasoldtup,
					   LogicalRepTupleData *oldtup,
					   LogicalRepTupleData *newtup);
extern void logicalrep_write_delete(StringInfo out, Relation rel,
							 HeapTuple oldtuple);
extern LogicalRepRelId logicalrep_read_delete(StringInfo in, LogicalRepTupleData *oldtup);
extern void logicalrep_write_rel(StringInfo out, Relation rel);

extern LogicalRepRelation *logicalrep_read_rel(StringInfo in);

#endif /* LOGICALREP_PROTO_H */
