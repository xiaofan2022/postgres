/*-------------------------------------------------------------------------
 *
 * IDENTIFICATION
 *	  wm_compatible.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WM_COMPATIBLE_H
#define WM_COMPATIBLE_H 1

#include "postgres.h"
#include "access/xlogreader.h"
#include "storage/block.h"

#if (defined PG_VERSION_16)
#include "storage/relfilelocator.h"
#endif

#if (defined PG_VERSION_16)
typedef struct RelFileNode
{
	Oid			spcNode;		/* tablespace */
	Oid			dbNode;			/* database */
	Oid			relNode;		/* relation */
} RelFileNode;
#endif

#if (defined PG_VERSION_16)
#define GET_CHANGE_RELNODE(change) ((RelFileNode*)&change->data.tp.rlocator)
#else
#define GET_CHANGE_RELNODE(change) ((RelFileNode*)&change->data.tp.relnode)
#endif

#ifndef PG_VERSION_15
#ifndef PG_VERSION_16
bool
XLogRecGetBlockTagExtended(XLogReaderState *record, uint8 block_id,
						   RelFileNode *rnode, ForkNumber *forknum,
						   BlockNumber *blknum,
						   void *prefetch_buffer);
#endif
#endif

void
wm_XLogRecGetBlockTag(XLogReaderState *record, uint8 block_id,
				   RelFileNode *rlocator, ForkNumber *forknum,
				   BlockNumber *blknum);


#endif