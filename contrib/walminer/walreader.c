/*-------------------------------------------------------------------------
 *
 * IDENTIFICATION
 *	  walreader.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "access/xlogreader.h"
#include "datadictionary.h"
#include "wm_utils.h"
#include "walminer_decode.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static int open_file_in_directory(const char *directory, const char *fname);
static XLogRecPtr WalFindNextRecord(XLogReaderState *state, XLogRecPtr RecPtr);
static void walminer_wal_read(const char *dirtemp, TimeLineID timeline_id,
				 XLogRecPtr startptr, char *buf, Size count, WalminerPrivate  *private);
static ListCell* walminer_get_first_valid_walfile(WalminerPrivate  *private);


/*
 * 此为从src/backend/access/transam/xlogreader.c文件拷贝来的函数，没有改动
 */
#if (defined PG_VERSION_13) || (defined PG_VERSION_14) || (defined PG_VERSION_15) || (defined PG_VERSION_16)
static int
ReadPageInternal(XLogReaderState *state, XLogRecPtr pageptr, int reqLen)
{
	int			readLen;
	uint32		targetPageOff;
	XLogSegNo	targetSegNo;
	XLogPageHeader hdr;

	Assert((pageptr % XLOG_BLCKSZ) == 0);

	XLByteToSeg(pageptr, targetSegNo, state->segcxt.ws_segsize);
	targetPageOff = XLogSegmentOffset(pageptr, state->segcxt.ws_segsize);

	/* check whether we have all the requested data already */
	if (targetSegNo == state->seg.ws_segno &&
		targetPageOff == state->segoff && reqLen <= state->readLen)
		return state->readLen;

	/*
	 * Data is not in our buffer.
	 *
	 * Every time we actually read the segment, even if we looked at parts of
	 * it before, we need to do verification as the page_read callback might
	 * now be rereading data from a different source.
	 *
	 * Whenever switching to a new WAL segment, we read the first page of the
	 * file and validate its header, even if that's not where the target
	 * record is.  This is so that we can check the additional identification
	 * info that is present in the first page's "long" header.
	 */
	if (targetSegNo != state->seg.ws_segno && targetPageOff != 0)
	{
		XLogRecPtr	targetSegmentPtr = pageptr - targetPageOff;

		readLen = state->routine.page_read(state, targetSegmentPtr, XLOG_BLCKSZ,
										   state->currRecPtr,
										   state->readBuf);
		if (readLen < 0)
			goto err;

		/* we can be sure to have enough WAL available, we scrolled back */
		Assert(readLen == XLOG_BLCKSZ);

		if (!XLogReaderValidatePageHeader(state, targetSegmentPtr,
										  state->readBuf))
			goto err;
	}

	/*
	 * First, read the requested data length, but at least a short page header
	 * so that we can validate it.
	 */
	readLen = state->routine.page_read(state, pageptr, Max(reqLen, SizeOfXLogShortPHD),
									   state->currRecPtr,
									   state->readBuf);
	if (readLen < 0)
		goto err;

	Assert(readLen <= XLOG_BLCKSZ);

	/* Do we have enough data to check the header length? */
	if (readLen <= SizeOfXLogShortPHD)
		goto err;

	Assert(readLen >= reqLen);

	hdr = (XLogPageHeader) state->readBuf;

	/* still not enough */
	if (readLen < XLogPageHeaderSize(hdr))
	{
		readLen = state->routine.page_read(state, pageptr, XLogPageHeaderSize(hdr),
										   state->currRecPtr,
										   state->readBuf);
		if (readLen < 0)
			goto err;
	}

	/*
	 * Now that we know we have the full header, validate it.
	 */
	if (!XLogReaderValidatePageHeader(state, pageptr, (char *) hdr))
		goto err;

	/* update read state information */
	state->seg.ws_segno = targetSegNo;
	state->segoff = targetPageOff;
	state->readLen = readLen;

	return readLen;

err:
	state->seg.ws_segno = 0;
	state->segoff = 0;
	state->readLen = 0;
	return -1;
}
#else
static int
ReadPageInternal(XLogReaderState *state, XLogRecPtr pageptr, int reqLen)
{
	int			readLen;
	uint32		targetPageOff;
	XLogSegNo	targetSegNo;
	XLogPageHeader hdr;

	Assert((pageptr % XLOG_BLCKSZ) == 0);
#ifndef PG_VERSION_10
	XLByteToSeg(pageptr, targetSegNo, state->wal_segment_size);
	targetPageOff = XLogSegmentOffset(pageptr, state->wal_segment_size);
#else
	XLByteToSeg(pageptr, targetSegNo);
	targetPageOff = (pageptr % XLogSegSize);
#endif
	

	/* check whether we have all the requested data already */
	if (targetSegNo == state->readSegNo && targetPageOff == state->readOff &&
		reqLen <= state->readLen)
		return state->readLen;

	/*
	 * Data is not in our buffer.
	 *
	 * Every time we actually read the page, even if we looked at parts of it
	 * before, we need to do verification as the read_page callback might now
	 * be rereading data from a different source.
	 *
	 * Whenever switching to a new WAL segment, we read the first page of the
	 * file and validate its header, even if that's not where the target
	 * record is.  This is so that we can check the additional identification
	 * info that is present in the first page's "long" header.
	 */
	if (targetSegNo != state->readSegNo && targetPageOff != 0)
	{
		XLogRecPtr	targetSegmentPtr = pageptr - targetPageOff;

		readLen = state->read_page(state, targetSegmentPtr, XLOG_BLCKSZ,
								   state->currRecPtr,
								   state->readBuf, &state->readPageTLI);
		if (readLen < 0)
			goto err;

		/* we can be sure to have enough WAL available, we scrolled back */
		Assert(readLen == XLOG_BLCKSZ);

		if (!XLogReaderValidatePageHeader(state, targetSegmentPtr,
										  state->readBuf))
			goto err;
	}

	/*
	 * First, read the requested data length, but at least a short page header
	 * so that we can validate it.
	 */
	readLen = state->read_page(state, pageptr, Max(reqLen, SizeOfXLogShortPHD),
							   state->currRecPtr,
							   state->readBuf, &state->readPageTLI);
	if (readLen < 0)
		goto err;

	Assert(readLen <= XLOG_BLCKSZ);

	/* Do we have enough data to check the header length? */
	if (readLen <= SizeOfXLogShortPHD)
		goto err;

	Assert(readLen >= reqLen);

	hdr = (XLogPageHeader) state->readBuf;

	/* still not enough */
	if (readLen < XLogPageHeaderSize(hdr))
	{
		readLen = state->read_page(state, pageptr, XLogPageHeaderSize(hdr),
								   state->currRecPtr,
								   state->readBuf, &state->readPageTLI);
		if (readLen < 0)
			goto err;
	}

	/*
	 * Now that we know we have the full header, validate it.
	 */
	if (!XLogReaderValidatePageHeader(state, pageptr, (char *) hdr))
		goto err;

	/* update read state information */
	state->readSegNo = targetSegNo;
	state->readOff = targetPageOff;
	state->readLen = readLen;

	return readLen;

err:
	state->readSegNo = 0;
	state->readOff = 0;
	state->readLen = 0;
	return -1;
}
#endif

/*
 * 此为从src/backend/access/transam/xlogreader.c文件拷贝来的函数，没有改动
 */
XLogRecPtr
WalFindNextRecord(XLogReaderState *state, XLogRecPtr RecPtr)
{
	XLogReaderState saved_state = *state;
	XLogRecPtr	tmpRecPtr;
	XLogRecPtr	found = InvalidXLogRecPtr;
	XLogPageHeader header;
	char	   *errormsg;

	Assert(!XLogRecPtrIsInvalid(RecPtr));
	/*
	 * skip over potential continuation data, keeping in mind that it may span
	 * multiple pages
	 */
	tmpRecPtr = RecPtr;
	while (true)
	{
		XLogRecPtr	targetPagePtr;
		int			targetRecOff;
		uint32		pageHeaderSize;
		int			readLen;

		/*
		 * Compute targetRecOff. It should typically be equal or greater than
		 * short page-header since a valid record can't start anywhere before
		 * that, except when caller has explicitly specified the offset that
		 * falls somewhere there or when we are skipping multi-page
		 * continuation record. It doesn't matter though because
		 * ReadPageInternal() is prepared to handle that and will read at
		 * least short page-header worth of data
		 */
		targetRecOff = tmpRecPtr % XLOG_BLCKSZ;

		/* scroll back to page boundary */
		targetPagePtr = tmpRecPtr - targetRecOff;

		/* Read the page containing the record */
		readLen = ReadPageInternal(state, targetPagePtr, targetRecOff);
		if (readLen < 0)
			goto err;

		header = (XLogPageHeader) state->readBuf;

		pageHeaderSize = XLogPageHeaderSize(header);

		/* make sure we have enough data for the page header */
		readLen = ReadPageInternal(state, targetPagePtr, pageHeaderSize);
		if (readLen < 0)
			goto err;

		/* skip over potential continuation data */
		if (header->xlp_info & XLP_FIRST_IS_CONTRECORD)
		{
			/*
			 * If the length of the remaining continuation data is more than
			 * what can fit in this page, the continuation record crosses over
			 * this page. Read the next page and try again. xlp_rem_len in the
			 * next page header will contain the remaining length of the
			 * continuation data
			 *
			 * Note that record headers are MAXALIGN'ed
			 */
			if (MAXALIGN(header->xlp_rem_len) >= (XLOG_BLCKSZ - pageHeaderSize))
				tmpRecPtr = targetPagePtr + XLOG_BLCKSZ;
			else
			{
				/*
				 * The previous continuation record ends in this page. Set
				 * tmpRecPtr to point to the first valid record
				 */
				tmpRecPtr = targetPagePtr + pageHeaderSize
					+ MAXALIGN(header->xlp_rem_len);
				break;
			}
		}
		else
		{
			tmpRecPtr = targetPagePtr + pageHeaderSize;
			break;
		}
	}
#if (defined PG_VERSION_13) || (defined PG_VERSION_14) || (defined PG_VERSION_15) || (defined PG_VERSION_16)
	XLogBeginRead(state, tmpRecPtr);
	while (XLogReadRecord(state, &errormsg) != NULL)
#else
	while (XLogReadRecord(state, tmpRecPtr, &errormsg) != NULL)
#endif
	{
		/* continue after the record */
		tmpRecPtr = InvalidXLogRecPtr;

		/* past the record we've found, break out */
		if (RecPtr <= state->ReadRecPtr)
		{
			found = state->ReadRecPtr;
			goto out;
		}
	}

err:
out:
	/* Reset state to what we had before finding the record */
	state->ReadRecPtr = saved_state.ReadRecPtr;
	state->EndRecPtr = saved_state.EndRecPtr;
#if (defined PG_VERSION_13) || (defined PG_VERSION_14)|| (defined PG_VERSION_15) || (defined PG_VERSION_16)
	state->seg.ws_segno = 0;
	state->segoff = 0;
#else
	state->readSegNo = 0;
	state->readOff = 0;
#endif
	state->readLen = 0;

	return found;
}

/*
 * 此为从src/backend/access/transam/xlogreader.c文件拷贝来的函数，没有改动
 */
static int
open_file_in_directory(const char *directory, const char *fname)
{
	int			fd = -1;
	char		fpath[MAXPGPATH];

	Assert(directory != NULL);

	snprintf(fpath, MAXPGPATH, "%s/%s", directory, fname);
	fd = open(fpath, O_RDONLY | PG_BINARY, 0);

	if (fd < 0 && errno != ENOENT)
		elog(ERROR, "could not open file \"%s\": %m",fname);
	return fd;
}

/*
 * 在加入的wal日志中如果有大量的不解析wal段，那么需要使用此函数跳过
 * wal段号比较靠前的wal段
 */
static ListCell*
walminer_get_first_valid_walfile(WalminerPrivate  *private)
{
	ListCell 	*cell = NULL;
	WalFile		*wf = NULL;
	bool		get_target_wal = false;
	char		*directory = NULL;
	char		*filename = NULL;
	TimeLineID	timelinecur = 0;
	XLogSegNo	segnocur = 0;
	bool		inseg = false;

	Assert(list_length(wal_file_list));
	cell = list_head(wal_file_list);

	while(!get_target_wal)
	{
		wf = lfirst(cell);
		split_path_fname(wf->filepath, &directory, &filename);
#ifndef PG_VERSION_10
			XLogFromFileName(filename, &timelinecur, &segnocur,  get_wal_seg_size(wf->filepath));
			inseg = XLByteInSeg(private->startptr, segnocur, get_wal_seg_size(wf->filepath));
#else
			XLogFromFileName(filename, &timelinecur, &segnocur);
			inseg = XLByteInSeg(private->startptr, segnocur);
#endif

		if(!inseg)
		{
			walminer_debug("Skip read walfile %s", filename);
#if (defined PG_VERSION_13) || (defined PG_VERSION_14)|| (defined PG_VERSION_15) || (defined PG_VERSION_16)
        cell = lnext(wal_file_list, cell);
#else
        cell = lnext(cell);
#endif
			if(!cell)
			{
				break;
			}
		}
		else
		{
			get_target_wal = true;
		}
	}
	if(!get_target_wal)
	{
		elog(ERROR, "Can not find a useful wal segment.");
	}
	walminer_debug("Get first walfile %s", filename);
	return cell;
}

/*
 * 读取wal文件，并将读取的数据存放入buff中
 */
static void
walminer_wal_read(const char *dirtemp, TimeLineID timeline_id,
				 XLogRecPtr startptr, char *buf, Size count, WalminerPrivate  *private)
{
	char	   *p;
	XLogRecPtr	recptr;
	Size		nbytes; 
	int			*sendFile = &private->sendFile;
	XLogSegNo 	*sendSegNo = &private->sendSegNo;
	uint32		*sendOff = &private->sendOff;
	ListCell	*cell = private->cell;
	int			*wal_seg_sz = &private->wal_seg_sz;

	p = buf;
	recptr = startptr;
	nbytes = count;

	if(!cell)
	{
#ifndef PG_VERSION_10
		WalFile *wf = NULL;
#endif

		cell = walminer_get_first_valid_walfile(private);
		
		private->cell = cell;
#ifndef PG_VERSION_10
		wf = lfirst(cell);
		*wal_seg_sz = get_wal_seg_size(wf->filepath); 
#else
		*wal_seg_sz = XLogSegSize;
#endif

	}
	while (nbytes > 0)
	{
		uint32		startoff;
		int			segbytes;
		int			readbytes;
		char		*directory = NULL;
		char		*filename = NULL;
		TimeLineID	timelinecur = 0;
		XLogSegNo	segnocur = 0;

#ifndef PG_VERSION_10
		startoff = XLogSegmentOffset(recptr, *wal_seg_sz);
		if (*sendFile < 0 || !XLByteInSeg(recptr, *sendSegNo, *wal_seg_sz))
#else
		startoff = (recptr % XLogSegSize);
		if (*sendFile < 0 || !XLByteInSeg(recptr, *sendSegNo))
#endif
		{
			WalFile		*wf = NULL;
			char		fname[MAXFNAMELEN] = {0};
			int			tries;

			/* Switch to another logfile segment */
			if (*sendFile >= 0)
				close(*sendFile);
			if(!cell)
			{
				*sendFile = -1;
				break;
			}
			wf = lfirst(cell);
#ifndef PG_VERSION_10
			*wal_seg_sz = get_wal_seg_size(wf->filepath); 
#else
			*wal_seg_sz = XLogSegSize;
#endif
#if (defined PG_VERSION_13) || (defined PG_VERSION_14)|| (defined PG_VERSION_15) || (defined PG_VERSION_16)
			cell = lnext(wal_file_list, cell);
#else
			cell = lnext(cell);
#endif
			private->cell = cell;

			split_path_fname(wf->filepath, &directory, &filename);
#ifndef PG_VERSION_10
			XLogFromFileName(filename, &timelinecur, &segnocur,  get_wal_seg_size(wf->filepath));
#else
			XLogFromFileName(filename, &timelinecur, &segnocur);
#endif
			strncpy(fname, filename, MAXFNAMELEN);
			*sendSegNo = segnocur;

			/*
			 * In follow mode there is a short period of time after the server
			 * has written the end of the previous file before the new file is
			 * available. So we loop for 5 seconds looking for the file to
			 * appear before giving up.
			 */
			for (tries = 0; tries < 10; tries++)
			{
				*sendFile = open_file_in_directory(directory, fname);
				if (*sendFile >= 0)
					break;
				if (errno == ENOENT)
				{
					int			save_errno = errno;

					/* File not there yet, try again */
					pg_usleep(500 * 1000);

					errno = save_errno;
					continue;
				}
				/* Any other error, fall through and fail */
				break;
			}

			if (*sendFile < 0)
				elog(ERROR, "could not find file \"~~%s/%s~~\": %m", directory, fname);
			if(!regression_mode && !private->search)
				elog(NOTICE, "Switch wal to %s on time %s", fname, 
											timestamptz_to_str(GetCurrentTimestamp()));
			*sendOff = 0;
		}

		/* Need to seek in the file? */
		if (*sendOff != startoff)
		{
			if (lseek(*sendFile, (off_t) startoff, SEEK_SET) < 0)
			{
				char		fname[MAXPGPATH];
#ifndef PG_VERSION_10
				XLogFileName(fname, timeline_id, *sendSegNo, *wal_seg_sz);
#else
				XLogFileName(fname, timeline_id, *sendSegNo);
#endif

				elog(ERROR, "could not seek in log file %s to offset %u: %m",
							fname, startoff);
			}
			*sendOff = startoff;
		}

		/* How many bytes are within this segment? */
		if (nbytes > (*wal_seg_sz - startoff))
			segbytes = *wal_seg_sz - startoff;
		else
			segbytes = nbytes;
		readbytes = read(*sendFile, p, segbytes);
		if (readbytes <= 0)
		{
			char		fname[MAXPGPATH];
			int			save_errno = errno;
#ifndef PG_VERSION_10
			XLogFileName(fname, timeline_id, *sendSegNo, *wal_seg_sz);
#else
			XLogFileName(fname, timeline_id, *sendSegNo);
#endif
			errno = save_errno;

			if (readbytes < 0)
				elog(ERROR, "could not read from log file %s, offset %u, length %d: %m",
							fname, *sendOff, segbytes);
			else if (readbytes == 0)
				elog(ERROR, "could not read from log file %s, offset %u: read %d of %zu",
							fname, *sendOff, readbytes, (Size) segbytes);
		}
		/* Update state for read */
		recptr += readbytes;

		*sendOff += readbytes;
		nbytes -= readbytes;
		p += readbytes;
	}
}

#if (defined PG_VERSION_13) || (defined PG_VERSION_14)|| (defined PG_VERSION_15) || (defined PG_VERSION_16)
static int
Walminer_read_page(XLogReaderState *state, XLogRecPtr targetPagePtr, int reqLen,
				 XLogRecPtr targetPtr, char *readBuff)
#else
static int
Walminer_read_page(XLogReaderState *state, XLogRecPtr targetPagePtr, int reqLen,
				 XLogRecPtr targetPtr, char *readBuff, TimeLineID *curFileTLI)
#endif
{
	WalminerPrivate  *private = state->private_data;
	int			     count = XLOG_BLCKSZ;

	if (private->endptr != InvalidXLogRecPtr)
	{
		if (targetPagePtr + XLOG_BLCKSZ <= private->endptr)
			count = XLOG_BLCKSZ;
		else if (targetPagePtr + reqLen <= private->endptr)
			count = private->endptr - targetPagePtr;
		else
		{
			private->endptr_reached = true;
			return -1;
		}
	}
	walminer_wal_read(private->inpath, private->timeline, targetPagePtr,
					 readBuff, count, private);
	return count;
}

XLogRecPtr
prepare_read(bool issearch, XLogRecPtr point_end_lsn)
{
	XLogRecPtr	        first_record = 0;
	WalminerPrivate		*private_temp = NULL;
	//static char			inpath[MAXPGPATH] = {0};

	private_temp = (WalminerPrivate*)walminer_malloc(sizeof(WalminerPrivate), 0);
	private_temp->sendFile = -1;
	private_temp->sendSegNo = 0;
	private_temp->sendOff = 0;
	private_temp->cell = NULL;
	private_temp->wal_seg_sz = 0;
	private_temp->search = issearch;

	if(issearch)
	{
		XLogSegNo			segno;
		WalFile				*wfs = NULL, *wfe = NULL;
		char				*fname = NULL, *dir = NULL;
		/*
		* 获取解析开始位置
		*/
		
		wfs = (WalFile*)lfirst(list_head(wal_file_list));
		split_path_fname(wfs->filepath, &dir, &fname);
		walminer_debug("[prepare_read 1] walfile_start = %s ", fname);

#ifndef PG_VERSION_10
		XLogFromFileName(fname, &private_temp->timeline, &segno,  get_wal_seg_size(wfs->filepath));
		XLogSegNoOffsetToRecPtr(segno, 0, get_wal_seg_size(wfs->filepath), private_temp->startptr);
#else
		XLogFromFileName(fname, &private_temp->timeline, &segno);
		XLogSegNoOffsetToRecPtr(segno, 0, private_temp->startptr);
#endif
		

		/*
		* 获取解析结束位置
		*/
		wfe = get_last_valid_wal(wal_file_list);
		split_path_fname(wfe->filepath, &dir, &fname);
		walminer_debug("[prepare_read 1] walfile_end = %s ", fname);
#ifndef PG_VERSION_10
		XLogFromFileName(fname, &private_temp->timeline, &segno,  get_wal_seg_size(wfe->filepath));
		XLogSegNoOffsetToRecPtr(segno + 1, 0, get_wal_seg_size(wfe->filepath), private_temp->endptr);
#else
		XLogFromFileName(fname, &private_temp->timeline, &segno);
		XLogSegNoOffsetToRecPtr(segno + 1, 0, private_temp->endptr);
#endif
		if(InvalidXLogRecPtr != point_end_lsn && private_temp->endptr > point_end_lsn)
		{
			private_temp->endptr = point_end_lsn;
		}
#if (defined PG_VERSION_13) || (defined PG_VERSION_14)|| (defined PG_VERSION_15) || (defined PG_VERSION_16)
		walminer_decode_context->reader_search = XLogReaderAllocate(get_wal_seg_size(wfs->filepath), NULL,
							XL_ROUTINE(.page_read = &Walminer_read_page), private_temp);
		
#elif (defined PG_VERSION_11) ||  (defined PG_VERSION_12)
		walminer_decode_context->reader_search = XLogReaderAllocate(get_wal_seg_size(wfs->filepath), Walminer_read_page, private_temp);
#else
		walminer_decode_context->reader_search = XLogReaderAllocate(Walminer_read_page, private_temp);
#endif
		/* 获取first record */
		first_record = WalFindNextRecord(walminer_decode_context->reader_search, private_temp->startptr);
		walminer_debug("[prepare_read 1] first_record = %x/%x ", 
											(uint32)(first_record >> 32), (uint32)first_record);
		walminer_decode_context->search_prive = (void*)private_temp;
	}
	else
	{
		WalminerSearchWal	*swal = NULL;

		swal = &walminer_decode_context->swpro;
		if(wdecoder.w_call_funcs.walminer_front_read)
			wdecoder.w_call_funcs.walminer_front_read();
#if (defined PG_VERSION_13) || (defined PG_VERSION_14)|| (defined PG_VERSION_15) || (defined PG_VERSION_16)
		walminer_decode_context->reader = XLogReaderAllocate(swal->wal_seg_size, NULL,
							XL_ROUTINE(.page_read = &Walminer_read_page), private_temp);
		
#elif (defined PG_VERSION_11) ||  (defined PG_VERSION_12)
		walminer_decode_context->reader = XLogReaderAllocate(swal->wal_seg_size, Walminer_read_page, private_temp);
#else
		walminer_decode_context->reader = XLogReaderAllocate(Walminer_read_page, private_temp);
#endif
		private_temp->timeline = swal->timeline_id;
		private_temp->startptr = swal->decode_lsn;
		walminer_decode_context->decode_prive = (void*)private_temp;
		first_record = private_temp->startptr;
		walminer_debug("[prepare_read 2] first_record = %x/%x ", 
											(uint32)(first_record >> 32), (uint32)first_record);
	}

	return first_record;
}


XLogRecord*
get_next_record(XLogReaderState *reader ,void *private_temp ,XLogRecPtr first_record)
{
    WalminerPrivate		*walminerprivate = NULL;
    XLogRecord          *record = NULL;
    char	            *errormsg;
	
	/*
	 * 在返回endptr_reached=true时，需要同时处理返回的record，所以不立即停止
	 * 再次获取下一个record时，返回null停止解析
	 */
	walminerprivate = (WalminerPrivate*)private_temp;
	if(walminerprivate->endptr_reached)
		return NULL;
#if (defined PG_VERSION_13) || (defined PG_VERSION_14)|| (defined PG_VERSION_15) || (defined PG_VERSION_16)
	record = XLogReadRecord(reader, &errormsg);
#else
	record = XLogReadRecord(reader, first_record, &errormsg);
	first_record = InvalidXLogRecPtr;
#endif
	return record;
}

