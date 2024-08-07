/*-------------------------------------------------------------------------
 *
 * IDENTIFICATION
 *	  imagemanage.c
 *
 *-------------------------------------------------------------------------
 */
#include "walminer_decode.h"
#include "wm_utils.h"
#include "storage/bufpage.h"
#include "common/pg_lzcompress.h"

typedef struct ImageStore
{
	RelFileNode rnode;
	ForkNumber	forknum;
	BlockNumber blkno;
}ImageStore;

typedef struct ImageStoreHashEntry
{
	ImageStore	key;
	int		index;
} ImageStoreHashEntry;

typedef	struct	PageRange
{
	BlockNumber	max_pageno;
	BlockNumber	min_pageno;
	bool		inited;
}PageRange;

PageRange pagerange;

static bool get_block_image(XLogReaderState *record, uint8 block_id, char *page);
static void append_image(ImageStore *image, char* page);
//static bool imageEqueal(ImageStore *image1, ImageStore *image2);
static void read_page(int index, char* page);
static HTAB *create_image_store_hash(void);
static int get_image_index(ImageStore *image);
static int put_image_index(ImageStore *image, bool *use_existed_imageindex);
static void update_page_range(uint32 pageno);

/*
 * 获取这个wal记录中，block_id序号块内存储的FPI,出参为page
 */
static bool
get_block_image(XLogReaderState *record, uint8 block_id, char *page)
{
	DecodedBkpBlock *bkpb;
	char	   *ptr;
#if ((defined PG_VERSION_15) || (defined PG_VERSION_16))
	PGAlignedBlock tmp;
	if (!record->record->blocks[block_id].in_use)
		return false;
	if (!record->record->blocks[block_id].has_image)
		return false;

	bkpb = &record->record->blocks[block_id];
#else
	char		tmp[BLCKSZ];
	if (!record->blocks[block_id].in_use)
		return false;
	if (!record->blocks[block_id].has_image)
		return false;

	bkpb = &record->blocks[block_id];
#endif
	ptr = bkpb->bkp_image;

#if (defined PG_VERSION_10) || (defined PG_VERSION_11) || (defined PG_VERSION_12)|| (defined PG_VERSION_13) || (defined PG_VERSION_14)
	if (bkpb->bimg_info & BKPIMAGE_IS_COMPRESSED)
	{
		/* If a backup block image is compressed, decompress it */
#if (defined PG_VERSION_10)  || (defined PG_VERSION_11)
		if (pglz_decompress(ptr, bkpb->bimg_len, tmp,
							BLCKSZ - bkpb->hole_length) < 0)	
#else
		if (pglz_decompress(ptr, bkpb->bimg_len, tmp,
							BLCKSZ - bkpb->hole_length,true) < 0)
#endif
		{
			elog(LOG, "invalid compressed image at %X/%X, block %d",
								  (uint32) (record->ReadRecPtr >> 32),
								  (uint32) record->ReadRecPtr,
								  block_id);
			return false;
		}
		ptr = tmp;
	}
#endif
#if ((defined PG_VERSION_15) || (defined PG_VERSION_16))
	if (BKPIMAGE_COMPRESSED(bkpb->bimg_info))
	{
		bool		decomp_success = true;

		if ((bkpb->bimg_info & BKPIMAGE_COMPRESS_PGLZ) != 0)
		{
			if (pglz_decompress(ptr, bkpb->bimg_len, tmp.data,
								BLCKSZ - bkpb->hole_length, true) < 0)
				decomp_success = false;
		}
		else if ((bkpb->bimg_info & BKPIMAGE_COMPRESS_LZ4) != 0)
		{
	#ifdef USE_LZ4
			if (LZ4_decompress_safe(ptr, tmp.data,
									bkpb->bimg_len, BLCKSZ - bkpb->hole_length) <= 0)
				decomp_success = false;
	#else
			elog(LOG, "invalid compressed image at %X/%X, block %d",
								  (uint32) (record->ReadRecPtr >> 32),
								  (uint32) record->ReadRecPtr,
								  block_id);
			return false;
	#endif
		}
		else if ((bkpb->bimg_info & BKPIMAGE_COMPRESS_ZSTD) != 0)
		{
	#ifdef USE_ZSTD
			size_t		decomp_result = ZSTD_decompress(tmp.data,
														BLCKSZ - bkpb->hole_length,
														ptr, bkpb->bimg_len);

			if (ZSTD_isError(decomp_result))
				decomp_success = false;
	#else
			elog(LOG, "invalid compressed image at %X/%X, block %d",
								  (uint32) (record->ReadRecPtr >> 32),
								  (uint32) record->ReadRecPtr,
								  block_id);
			return false;
	#endif
		}
		else
		{
			elog(LOG, "invalid compressed image at %X/%X, block %d",
								  (uint32) (record->ReadRecPtr >> 32),
								  (uint32) record->ReadRecPtr,
								  block_id);
			return false;
		}

		if (!decomp_success)
		{
			elog(LOG, "could not decompress image at %X/%X, block %d",
								  (uint32) (record->ReadRecPtr >> 32),
								  (uint32) record->ReadRecPtr,
								  block_id);
			return false;
		}
		ptr = tmp.data;
	}
#endif

	/* generate page, taking into account hole if necessary */
	if (bkpb->hole_length == 0)
	{
		memcpy(page, ptr, BLCKSZ);
	}
	else
	{
		memcpy(page, ptr, bkpb->hole_offset);
		/* must zero-fill the hole */
		MemSet(page + bkpb->hole_offset, 0, bkpb->hole_length);
		memcpy(page + (bkpb->hole_offset + bkpb->hole_length),
			   ptr + bkpb->hole_offset,
			   BLCKSZ - (bkpb->hole_offset + bkpb->hole_length));
	}

	return true;
}

void
clean_image(void)
{
	char	storefile[MAXPGPATH] = {0};
	char	path[MAXPGPATH] = {0};
	char	*filename = "storeimage";
	FILE	*fp = NULL;

	get_image_path((char*)path);
	sprintf(storefile,"%s/%s", path, filename);
	walminer_debug("Clean image at %x/%x", 
			(uint32)(walminer_decode_context->reader->ReadRecPtr >> 32), (uint32)walminer_decode_context->reader->ReadRecPtr);
	fp = fopen(storefile, "w");
	if(!fp)
	{
		elog(ERROR,"fail to open file %s to read", storefile);
	}
	fclose(fp);
}


static void
read_page(int index, char* page)
{
	char	storefile[MAXPGPATH] = {0};
	char	path[MAXPGPATH] = {0};
	char	*filename = "storeimage";
	FILE	*fp = NULL;
	int64	seeksize = 0;
	Assert(page);
	
	get_image_path((char*)path);
	sprintf(storefile,"%s/%s", path, filename);

	fp = fopen(storefile, "rb");
	if(!fp)
	{
		elog(ERROR,"fail to open file %s to read", storefile);
	}
	fseek(fp, 0, SEEK_SET);
	Assert(0 == ftell(fp));
	Assert(0 <= index);
	if(0 != index)
	{
		seeksize = (int64)index * BLCKSZ;
		fseek(fp, seeksize, SEEK_SET);
	}
	if(BLCKSZ != fread(page, 1, BLCKSZ, fp))
	{
		elog(ERROR,"fail to read %s", storefile);
	}

	fclose(fp);
	fp = NULL;
}

/*
 * 向硬盘存储FPW的代码
 * TODO(lchch)设计一个FPW的存盘机制，使解析效率增加
 */
void
flush_page(int index, char* page)
{
	char	storefile[MAXPGPATH] = {0};
	char	path[MAXPGPATH] = {0};
	char	*filename = "storeimage";
	FILE	*fp = NULL;
	int64	seeksize = 0;

	Assert(page);

    get_image_path((char*)path);
	sprintf(storefile,"%s/%s", path, filename);

	fp = fopen(storefile, "rb+");
	if(!fp)
	{
		elog(ERROR,"fail to open file %s to write:%m", storefile);
	}	
	fseek(fp, 0, SEEK_SET);
	Assert(0 == ftell(fp));
	Assert(0 <= index);
	if(0 != index)
	{
		seeksize = (int64)index * BLCKSZ;
		fseek(fp, seeksize, SEEK_SET);
		//walminer_debug("[flush_page]index=%d, ftell=%d,seeksize=%d",index, ftell(fp), seeksize);
		
	}
	if(BLCKSZ != fwrite(page, 1, BLCKSZ, fp))
	{
		elog(ERROR,"fail to write to %s", storefile);
	}
	fclose(fp);
	fp = NULL;
}

static void
update_page_range(uint32 pageno)
{
	if(!pagerange.inited)
	{
		pagerange.max_pageno = pageno;
		pagerange.min_pageno = pageno;
		pagerange.inited = true;
	}
	else if(pageno > pagerange.max_pageno)
		pagerange.max_pageno = pageno;
	else if(pageno < pagerange.min_pageno)
		pagerange.min_pageno = pageno;
}

static void
append_image(ImageStore *image, char* page)
{
	static int	imageondisklast = 0;						//静态变量用来控制输出log，警告内存占用量
	int			reporstep = (512 * 1024 * 1024) / BLCKSZ;	//512M image on disk
	int			append_index = 0;
	bool		use_existed_imageindex = false;
	int			length = 0;

	append_index = put_image_index(image, &use_existed_imageindex);
	update_page_range(image->blkno);
	flush_page(append_index, page);

	if(use_existed_imageindex)
		return;

	/* 下面的代码仅仅用于告警输出 */
	length = append_index + 1;
	if(imageondisklast + reporstep <= length)
	{
		imageondisklast = length;
		elog(NOTICE, "there be %d image pages on disk", imageondisklast);
	}
}

void
get_pageno_range(BlockNumber *maxno, BlockNumber *minno, bool *inited)
{
	Assert(pagerange.inited);
	*maxno = pagerange.max_pageno;
	*minno = pagerange.min_pageno;
	*inited = pagerange.inited;
}

void
record_store_image(XLogReaderState *record)
{
	uint8			block_id = 0;
	DecodedBkpBlock *bkpb = NULL;
	ImageStore 		*image = NULL;
    RelFileNode     target_node;
	char			page[BLCKSZ] = {0};
	int				max_block = 0;

    memset(&target_node, 0, sizeof(RelFileNode));
#if ((defined PG_VERSION_15) || (defined PG_VERSION_16))
	if(!XLogRecHasBlockRef(record, 0))
		return;
#endif
    wm_XLogRecGetBlockTag(record, 0, &target_node, NULL, NULL);
    if(!filter_in_decode(&target_node))
	{
        return;
	}
#if ((defined PG_VERSION_15) || (defined PG_VERSION_16))
	max_block = XLogRecMaxBlockId(record);
#else
	max_block = XLR_MAX_BLOCK_ID;
#endif
	for(block_id = 0; block_id <= max_block; block_id++)
	{
#if ((defined PG_VERSION_15) || (defined PG_VERSION_16))
		bkpb = &record->record->blocks[block_id];
#else
		bkpb = &record->blocks[block_id];
#endif
		if(!bkpb->in_use)
			return;
		if(!bkpb->has_image)
			continue;
		memset(page, 0, BLCKSZ);
		image = palloc0(sizeof(ImageStore));
#if (defined PG_VERSION_16)
		image->rnode.dbNode = bkpb->rlocator.dbOid;
		image->rnode.relNode = bkpb->rlocator.relNumber;
		image->rnode.spcNode = bkpb->rlocator.spcOid;
#else
		image->rnode.dbNode = bkpb->rnode.dbNode;
		image->rnode.relNode = bkpb->rnode.relNode;
		image->rnode.spcNode = bkpb->rnode.spcNode;
#endif
		image->forknum = bkpb->forknum;
		image->blkno = bkpb->blkno;


		if(get_block_image(record, block_id, page))
		{
			walminer_debug("record_store_image relNode=%d, blkno=%d", image->rnode.relNode, image->blkno );
			//out_page_to_file(page, image->rnode.relNode, image->blkno, 0);
			append_image(image, page);
		}
		else
		{
			walminer_debug("record_store_image NULL");
		}
		pfree(image);
	}
}

void
page_init_by_xlog(RelFileNode *rnode, ForkNumber forknum, BlockNumber blkno)
{
	ImageStore 		*image = NULL;
	char 			page[BLCKSZ] = {0};
	
	PageInit(page, BLCKSZ, 0);

	image = palloc(sizeof(ImageStore));
	image->rnode.dbNode = rnode->dbNode;
	image->rnode.relNode = rnode->relNode;
	image->rnode.spcNode = rnode->spcNode;
	walminer_debug("[page_init_by_xlog]initpage:relnode %u, blckno %d",rnode->relNode, blkno);

	image->forknum = forknum;
	image->blkno = blkno;
	append_image(image, page);
	pfree(image);
}

bool
get_image_from_store(RelFileNode *rnode, ForkNumber forknum, BlockNumber blkno, char* page, int *index)
{
	ImageStore	images;
	int			imageindex = 0;

	Assert(rnode);
	Assert(page);
	memset(&images, 0, sizeof(ImageStore));

	memcpy(&images.rnode, rnode, sizeof(RelFileNode));
	images.forknum = forknum;
	images.blkno = blkno;

	imageindex = get_image_index(&images);

	if(-1 == imageindex)
	{
		walminer_debug("[get_image_from_store]dbNode=%u,relnode=%u, spcnode=%u, blkno=%u, forknum=%d NOT FOUND",
				rnode->dbNode, rnode->relNode, rnode->spcNode, blkno, forknum);
		return false;
	}

	read_page(imageindex, page);
	//out_page_to_file(page, images.rnode.relNode, images.blkno, 1);
	*index = imageindex;
	walminer_debug("[getImageFromStore]page:relnode %u, blckno %d,index %d", rnode->relNode, blkno, *index);
	return true;
}

/*
static bool
imageEqueal(ImageStore *image1, ImageStore *image2)
{
	if(!image2)
		return false;

	if(image1->forknum != image2->forknum || image1->blkno != image2->blkno)
		return false;
	if(0 != memcmp(&image1->rnode, &image2->rnode, sizeof(RelFileNode)))
		return false;

	return true;
}*/

void
out_page_to_file(char *page, Oid relfilenode, BlockNumber blkno, int kind)
{
	char	log_path[MAXPGPATH] = {0};
	char	file_path[MAXPGPATH] = {0};
	FILE	*fp = NULL;

	get_runtemp_path(log_path);
	sprintf(file_path, "%s/%u_%u_%d", log_path, blkno, relfilenode, kind);
	
	fp = fopen(file_path, "wb");
	if(!fp)
	{
		elog(ERROR, "can not open %s to write:%m", file_path);
	}
	fwrite(page, 1, BLCKSZ, fp);
	fclose(fp);
}

static HTAB *
create_image_store_hash(void)
{
	HASHCTL		ctl;

	ctl.keysize = sizeof(ImageStore);
	ctl.entrysize = sizeof(ImageStoreHashEntry);
	memset(&pagerange, 0, sizeof(PageRange));

	return hash_create("walminer ImageStore hash", 128, &ctl, HASH_ELEM|HASH_BLOBS);
}

/**
 * Returns index of matched ImageStore or -1 if not found
 */
static int
get_image_index(ImageStore *image)
{
	ImageStore	key;
	ImageStoreHashEntry * imageStoreHashEntry;

	if (!walminer_decode_context->anapro.imageStoreHash)
		walminer_decode_context->anapro.imageStoreHash = create_image_store_hash();

	memset(&key, 0, sizeof(ImageStore));

	memcpy(&key.rnode, &image->rnode, sizeof(RelFileNode));
	key.rnode.dbNode = image->rnode.dbNode;
	key.rnode.relNode = image->rnode.relNode;
	key.rnode.spcNode = image->rnode.spcNode;
	key.forknum = image->forknum;
	key.blkno = image->blkno;

	imageStoreHashEntry = (ImageStoreHashEntry *) hash_search(walminer_decode_context->anapro.imageStoreHash,&key, HASH_FIND, NULL);

	if(imageStoreHashEntry)
		return imageStoreHashEntry->index;

	return -1;
}
 
static int
put_image_index(ImageStore *image, bool *use_existed_imageindex)
{
	ImageStore			key;
	bool				found = false;
	ImageStoreHashEntry *imageStoreHashEntry = NULL;

	if (!walminer_decode_context->anapro.imageStoreHash)
		walminer_decode_context->anapro.imageStoreHash = create_image_store_hash();

	memset(&key, 0, sizeof(ImageStore));
	key.rnode.dbNode = image->rnode.dbNode;
	key.rnode.relNode = image->rnode.relNode;
	key.rnode.spcNode = image->rnode.spcNode;
	key.forknum = image->forknum;
	key.blkno = image->blkno;

	walminer_debug("[put_image_index]dbNode=%u,relnode=%u, spcnode=%u, blkno=%u, forknum=%d",
				image->rnode.dbNode,  image->rnode.relNode, image->rnode.spcNode, image->blkno, image->forknum);
	imageStoreHashEntry = (ImageStoreHashEntry *) hash_search(walminer_decode_context->anapro.imageStoreHash, &key,
											   HASH_ENTER, &found);

	if (!found)
	{
		memcpy(&imageStoreHashEntry->key,&key,sizeof(ImageStore));
		imageStoreHashEntry->index = hash_get_num_entries(walminer_decode_context->anapro.imageStoreHash) - 1;
		if(WALMINER_INT32_MAX <= imageStoreHashEntry->index + 1)
		{
			elog(ERROR, "Can not support page numer greater than %d", imageStoreHashEntry->index);
		}
	}

	*use_existed_imageindex = found;

	return imageStoreHashEntry->index;
}
