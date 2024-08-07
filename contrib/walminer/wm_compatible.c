/*-------------------------------------------------------------------------
 *
 * IDENTIFICATION
 *	  wm_compatible.c
 *
 *-------------------------------------------------------------------------
 */
#include "wm_compatible.h"

void
wm_XLogRecGetBlockTag(XLogReaderState *record, uint8 block_id,
				   RelFileNode *rlocator, ForkNumber *forknum,
				   BlockNumber *blknum)
{
#if (defined PG_VERSION_16)
	XLogRecGetBlockTag(record, block_id, (RelFileLocator*)rlocator, forknum, blknum);
#else
	XLogRecGetBlockTag(record, block_id, rlocator, forknum, blknum);
#endif
}

#ifndef PG_VERSION_15
#ifndef PG_VERSION_16
bool
XLogRecGetBlockTagExtended(XLogReaderState *record, uint8 block_id,
						   RelFileNode *rnode, ForkNumber *forknum,
						   BlockNumber *blknum,
						   void *prefetch_buffer)
{
	DecodedBkpBlock *bkpb;

	if (!record->blocks[block_id].in_use)
		return false;

	bkpb = &record->blocks[block_id];
	if (rnode)
		*rnode = bkpb->rnode;
	if (forknum)
		*forknum = bkpb->forknum;
	if (blknum)
		*blknum = bkpb->blkno;
	return true;
}
#endif
#endif