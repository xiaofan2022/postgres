/*
 * wallist.c
 */
#include "datadictionary.h"
#include "wm_utils.h"

#define	WALMINER_WALFILE_REPEAT_CHECK_DIFFERENT	1
#define	WALMINER_WALFILE_REPEAT_CHECK_SAMESEGNO	2
#define	WALMINER_WALFILE_REPEAT_CHECK_SAME		3

List	*wal_file_list = NULL;


static bool check_walfile_valid(char *path,int pathkind);
static void add_walfile_to_list(char *path);
#if (defined PG_VERSION_13)  || (defined PG_VERSION_14) || (defined PG_VERSION_15) || (defined PG_VERSION_16)
static int wal_file_compare(const ListCell *v1,const ListCell *v2);
#else
static int wal_file_compare(const void *v1,const void *v2);
#endif
static WalFile* fill_wal_file(char *path);

#if (defined PG_VERSION_10) ||  (defined PG_VERSION_9_6) || (defined PG_VERSION_9_5)
typedef int (*list_qsort_comparator) (const void *a, const void *b);
static List* list_qsort(const List *list, list_qsort_comparator cmp);
static List* new_list(NodeTag type);
#endif

#if (defined PG_VERSION_10) ||  (defined PG_VERSION_9_6) || (defined PG_VERSION_9_5)
static List *
new_list(NodeTag type)
{
	List	   *new_list;
	ListCell   *new_head;

	new_head = (ListCell *) palloc(sizeof(*new_head));
	new_head->next = NULL;
	/* new_head->data is left undefined! */

	new_list = (List *) palloc(sizeof(*new_list));
	new_list->type = type;
	new_list->length = 1;
	new_list->head = new_head;
	new_list->tail = new_head;

	return new_list;
}

static List *
list_qsort(const List *list, list_qsort_comparator cmp)
{
	int			len = list_length(list);
	ListCell  **list_arr;
	List	   *newlist;
	ListCell   *newlist_prev;
	ListCell   *cell;
	int			i;

	/* Empty list is easy */
	if (len == 0)
		return NIL;

	/* Flatten list cells into an array, so we can use qsort */
	list_arr = (ListCell **) palloc(sizeof(ListCell *) * len);
	i = 0;
	foreach(cell, list)
		list_arr[i++] = cell;

	qsort(list_arr, len, sizeof(ListCell *), cmp);

	/* Construct new list (this code is much like list_copy) */
	newlist = new_list(list->type);
	newlist->length = len;

	/*
	 * Copy over the data in the first cell; new_list() has already allocated
	 * the head cell itself
	 */
	newlist->head->data = list_arr[0]->data;

	newlist_prev = newlist->head;
	for (i = 1; i < len; i++)
	{
		ListCell   *newlist_cur;

		newlist_cur = (ListCell *) palloc(sizeof(*newlist_cur));
		newlist_cur->data = list_arr[i]->data;
		newlist_prev->next = newlist_cur;

		newlist_prev = newlist_cur;
	}

	newlist_prev->next = NULL;
	newlist->tail = newlist_prev;

	/* Might as well free the workspace array */
	pfree(list_arr);

	return newlist;
}
#endif



/*
 * 时间线、文件名、数据字典匹配检查
 * 
 * 对单文件入参来说，直接报错error
 * 对目录入参来说，返回false，再检查下一个文件
 */
static bool
check_walfile_valid(char *path,int pathkind)
{
	FILE 					*fp = NULL;
	int						length = 0, loop = 0;
	bool					filenamevalid = true;
	char					*filedir = NULL, *filename = NULL;
	TimeLineID				timelinecheck = 0;
	XLogSegNo				segnocheck = 0;
	
	XLogLongPageHeaderData 	xlphd;

	split_path_fname(path,&filedir,&filename);

	/*文件名检查*/
	length = strlen(filename);
	if(length != 24)
		filenamevalid = false;
	else
	{
#ifdef PG_VERSION_10
		XLogFromFileName(filename, &timelinecheck, &segnocheck);
#else
		XLogFromFileName(filename, &timelinecheck, &segnocheck, get_wal_seg_size(path));
#endif

		/*时间线检查*/
		if(timelinecheck != wdd.ddh.timeline)
		{
			/*单文件入参直接报错，目录入参继续检查下一个文件，即使没有符合条件的wal也不会报错*/
			if(PATH_KIND_DIR == pathkind)
				return false;
			else
				elog(ERROR,"The timeline of the xlog file does not match the time line of the data dictionary.");
		}
	}
	
	for(;loop < length && filenamevalid; loop++)
	{
		if(filename[loop] < '0' || filename[loop] > 'F')
			filenamevalid = false;
	}
	
	if(filename)
		pfree(filename);
	if(filedir)
		pfree(filedir);
	
	if(!filenamevalid)
	{
		if(PATH_KIND_DIR != pathkind)
			ereport(ERROR,(errmsg("wal file \"%s\" is invalid.",path)));
		else
			return false;
	}
	
	/*数据字典匹配检查*/
	fp = fopen(path, "rb");
	if(!fp)
		ereport(ERROR,
				(errcode(ERRCODE_SYSTEM_ERROR),
				errmsg("Open file \"%s\" failed",path)));
	memset(&xlphd, 0, SizeOfXLogLongPHD);
	fread(&xlphd, SizeOfXLogLongPHD, 1, fp);
	fclose(fp);

	if(wdd.ddh.sysid == xlphd.xlp_sysid || wdd.set_user_map)
		return true;
	else
	{
		ereport(NOTICE,(errmsg("Wal file \"%s\" is not match with datadictionary.",path)));
		return false;
	}
}


/*
 * 检查两个可能不在同一目录的wal文件，是否为同一个wal文件
 * 
 * 结果为:同一个wal文件;不同目录的同一个wal文件;不同的wal文件
 */
static int
check_walfile_repeat(char *path, WalFile **conflict_file)
{
	List 		*wallist = NULL;
	ListCell    *cell = NULL;
	WalFile	 	*wfptr = NULL;
	char		*filedir = NULL,*filename = NULL;
	TimeLineID	timelinecheck = 0;
	XLogSegNo	segnocheck = 0;
	TimeLineID	timelinecur = 0;
	XLogSegNo	segnocur = 0;

	
	wallist = wal_file_list;
	if(!wallist)
		return WALMINER_WALFILE_REPEAT_CHECK_DIFFERENT;

	split_path_fname(path,&filedir,&filename);
#ifdef PG_VERSION_10
	XLogFromFileName(filename, &timelinecheck, &segnocheck);
#else
	XLogFromFileName(filename, &timelinecheck, &segnocheck, get_wal_seg_size(path));
#endif

	foreach(cell, wallist)
	{
		wfptr = lfirst(cell);

		split_path_fname(wfptr->filepath,&filedir,&filename);
#ifdef PG_VERSION_10
		XLogFromFileName(filename, &timelinecur, &segnocur);
#else
		XLogFromFileName(filename, &timelinecur, &segnocur,  get_wal_seg_size(wfptr->filepath));
#endif
		if(0 == strcmp(path, wfptr->filepath))
		{
			*conflict_file = wfptr;
			return WALMINER_WALFILE_REPEAT_CHECK_SAME;
		}
		else if(timelinecur == timelinecheck && segnocur == segnocheck)
		{
			*conflict_file = wfptr;
			return WALMINER_WALFILE_REPEAT_CHECK_SAMESEGNO;
		}
	}
	return WALMINER_WALFILE_REPEAT_CHECK_DIFFERENT;
}


static WalFile*
fill_wal_file(char *path)
{
	WalFile		*wf = NULL;
	TimeLineID	timelinecheck = 0;
	XLogSegNo	segnocheck = 0;
	char		*filedir = NULL, *filename = NULL;

	wf = (WalFile*)walminer_malloc(sizeof(WalFile),0);
	if(!wf)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				errmsg("Out of memory")));

	split_path_fname(path,&filedir,&filename);
#ifdef PG_VERSION_10
		XLogFromFileName(filename, &timelinecheck, &segnocheck);
#else
		XLogFromFileName(filename, &timelinecheck, &segnocheck, get_wal_seg_size(path));
#endif

	strncpy(wf->filepath, path, MAXPGPATH);
	wf->segno = segnocheck;
	wf->tl = timelinecheck;

	return wf;
}

int
add_wal(char *path)
{
	int 		pathkind = 0;
	int			filenum = 0;
	int			validnum = 0;
	int			loop = 0;
	NameData	*filenamelist = NULL;
	char		walfile[MAXPGPATH] = {0};
	char		dict_path[MAXPGPATH] = {0};

	
	/* 检查入参格式 */
	pathkind = path_judge(path);

	if(PATH_KIND_INVALID == pathkind)
	{
		if(is_empt_str(path))
		{
			elog(ERROR,"Please enter a file path or directory.");
		}
		else
			elog(ERROR,"File or directory \"%s\" access is denied or does not exists.", path);
	}
	else if(PATH_KIND_NULL == pathkind)
		elog(ERROR,"Please enter a file path or directory.");

	memset(walfile, 0, MAXPGPATH);
	if(PATH_KIND_FILE == pathkind || PATH_KIND_SFILE == pathkind)
	{
		snprintf(walfile, MAXPGPATH, "%s",path);
		if(!is_file_exist(walfile) && (PATH_KIND_FILE == pathkind))
			ereport(ERROR,(errmsg("File or directory \"%s\" access is denied or does not exists.",path)));
		filenum = 1;
	}
	else if(PATH_KIND_DIR == pathkind)
	{
		filenum = scan_dir_get_filenum(path);
		if(0 < filenum)
		{
			filenamelist = (NameData *)palloc0(filenum * sizeof(NameData));
			scan_dir_get_filename(path, filenamelist,false);
			/*for code Simplify. Make process same with "FILE == pathkind" while filenum was 1*/
			if(1 == filenum)
#ifdef WIN32
				snprintf(walfile, MAXPGPATH, "%s\\%s",path,filenamelist[0].data);
#else
				snprintf(walfile, MAXPGPATH, "%s/%s",path,filenamelist[0].data);
#endif
		}
	}

	if(!wdd.loaded)
	{
		get_dict_path(dict_path);
		load_dictionary(dict_path, WALMINER_LOADDICT_ADDWAL);
	}
	

	for(loop = 0; loop < filenum; loop++)
	{
		if(1 != filenum)
		{
			memset(walfile, 0, MAXPGPATH);
#ifdef WIN32
			snprintf(walfile, MAXPGPATH, "%s\\%s",path, filenamelist[loop].data);
#else
			snprintf(walfile, MAXPGPATH, "%s/%s",path, filenamelist[loop].data);
#endif
		}
		if(check_walfile_valid(walfile, pathkind))
		{
			WalFile	*wf = NULL;
			int		repeat_check_result = check_walfile_repeat(walfile, &wf);

			if(WALMINER_WALFILE_REPEAT_CHECK_DIFFERENT ==repeat_check_result)
			{
				validnum++;
				add_walfile_to_list(walfile);
			}
			else if(WALMINER_WALFILE_REPEAT_CHECK_SAMESEGNO == repeat_check_result)
			{
				elog(NOTICE, "\"%s\" is conflicted with file \"%s\" ",walfile, wf->filepath);
			}
		}
	}
#if (defined PG_VERSION_13) || (defined PG_VERSION_14) || (defined PG_VERSION_15) || (defined PG_VERSION_16)
	list_sort(wal_file_list, wal_file_compare);
#else
	wal_file_list = list_qsort(wal_file_list, wal_file_compare);
#endif
	if(filenamelist)
		pfree(filenamelist);

	return validnum;
}

static void 
add_walfile_to_list(char *path)
{
	WalFile 			*wf = NULL;

	wf = fill_wal_file(path);
	wal_file_list = lappend(wal_file_list, wf);
}

#if (defined PG_VERSION_13)  || (defined PG_VERSION_14) || (defined PG_VERSION_15) || (defined PG_VERSION_16)
static int
wal_file_compare(const ListCell *v1,const ListCell *v2)
{
	WalFile	*wf1 = (WalFile *) lfirst(v1);
	WalFile	*wf2 = (WalFile *) lfirst(v2);

	if(wf1->segno > wf2->segno)
		return 1;
	return -1;
}
#else
static int
wal_file_compare(const void *v1,const void *v2)
{
	WalFile	*wf1 = (WalFile *) lfirst(*(ListCell **) v1);
	WalFile	*wf2 = (WalFile *) lfirst(*(ListCell **) v2);

	if(wf1->segno > wf2->segno)
		return 1;
	return -1;
}
#endif

void
free_wallist(void)
{
	list_free(wal_file_list);
	wal_file_list = NULL;
}

/*
 * 查看wal文件是否连续，将不连续的wal文件的valid置为false
 */
void
go_through_list(List *list)
{
	ListCell 	*cell = NULL;
	WalFile		*last_wf = NULL;

	foreach(cell, list)
	{
		WalFile	*wf = lfirst(cell);

		if(!last_wf)
		{
			/*
			 * list头是有效的
			 */
			wf->valid = true;
		}
		else
		{
			/*
			 * 如果与上一个wal文件不连续，那么是无效的
			 * 如果与上一个wal文件连续，valid等于上一个wal文件的valid
			 */
			if(wf->segno != last_wf->segno + 1)
				wf->valid = false;
			else
				wf->valid = last_wf->valid;
			walminer_debug("[go_through_list]%s is valid %d, %lu", wf->filepath, wf->valid, wf->segno);
		}
		last_wf = wf;
	}
}

WalFile*
get_last_valid_wal(List *list)
{
	ListCell 	*cell = NULL;
	WalFile		*lastwf = NULL;

	Assert(list_length(list));

	foreach(cell, list)
	{
		WalFile	*wf = lfirst(cell);

		if(wf->valid)
		{
			walminer_debug("[get_last_valid_wal]%s is valid %d", wf->filepath, wf->valid);
			
		}
		else
		{
			walminer_debug("[get_last_valid_wal]return %s is valid %d", lastwf->filepath, lastwf->valid);
			return lastwf;
		}
		lastwf = wf;
	}
	return lastwf;
}

int
remove_wal_file(char *path)
{
	int			removenum = 0;
	WalFile		*wfptr = NULL;
	ListCell	*cell = NULL;
	char		*filedir = NULL, *filename = NULL;

	walminer_debug("[remove_wal_file]path=%s", path);

	if(is_dir_exist(path))
	{
		elog(ERROR, "Argument can be file only, an not be a directory");
	}

	if(0 == strcmp("",path))
		elog(ERROR,"Please enter a file path.");


	if(WALMINER_WALFILE_REPEAT_CHECK_SAME == check_walfile_repeat(path, &wfptr))
	{
		removenum++;
		wal_file_list = list_delete_ptr(wal_file_list, wfptr);
		return removenum;
	}

	foreach(cell, wal_file_list)
	{
		wfptr = lfirst(cell);

		split_path_fname(wfptr->filepath,&filedir,&filename);
		fix_path_end(path);

		walminer_debug("[remove_wal_file]path=%s, filedir=%s", path,filedir);
		if(0 == strcmp(path, filedir))
		{
			wal_file_list = list_delete_ptr(wal_file_list, wfptr);
			removenum++;
		}
	}

	return removenum;
}