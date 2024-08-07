/*-------------------------------------------------------------------------
 *
 * IDENTIFICATION
 *	  wm_utils.c
 *
 *-------------------------------------------------------------------------
 */
#include "wm_utils.h"
#include "miscadmin.h"
#include "access/xlog_internal.h"
#include "datadictionary.h"
#include "utils/timestamp.h"
#include "utils/builtins.h"
#include "walminer_decode.h"
#include "replication/reorderbuffer.h"


#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

static bool create_dir(char *path);
static void check_dir(void);
static void check_user(void);

bool debug_mode = false;

bool
is_file_exist(char *path)
{
	if(!path)
		return 0;
#ifdef WIN32
	if(_access(path,F_OK) == 0)
#else
	if(access(path,F_OK) == 0)
#endif
		return 1;
	return 0;
}

bool
is_dir_exist(char *path)
{
	DIR		*dirptr = NULL;

	if(!path)
		return 0;

	dirptr = opendir(path);
	if(!dirptr)
	{
		//elog(WARNING, "is_dir_exist %s:%m",abs_path);
		return 0;
	}
	closedir(dirptr);
	return 1;
}

bool
is_path_write_access(char *path)
{
	if(!path)
		return 0;
#ifdef WIN32
	if(_access(path,W_OK) == 0)
#else
	if(access(path,W_OK) == 0)
#endif
		return 1;
	return 0;
}

static bool
create_dir(char *path)
{	
	int	result = 0;

	if(is_dir_exist(path))
		return true;
	result = mkdir(path,S_IRWXU);
	if(0 == result)
		return true;
	else
		return false;
}

void
create_file(char* path)
{
	FILE *fp = NULL;
	
	Assert(path);

	fp = fopen(path,"wb");
	if(!fp)
		elog(ERROR,"Can not create dictionary file %s",path);
	fclose(fp);
}

/*
 *  检查pg_walminer目录结构，缺失任何一个目录结构则创建
 *  TODO(lchch):此处应设计一套容错机制来检测目录完整性，防止人为删除某些
 *  目录导致缺失重要解析信息。
 * 
 */
static void
check_dir(void)
{
	char	check_path[MAXPGPATH] = {0};
	//如果改动这里，需同时改动walminer.c的描述信息
	char	*path_array[4] = {"pg_walminer",
							 "pg_walminer/wm_analyselog",
							 "pg_walminer/wm_datadict",
							 "pg_walminer/wm_image"};
	int		array_num = 4;
	int		loop = 0;

	Assert(is_dir_exist(DataDir));

	for (; loop < array_num; loop++)
	{
		memset(check_path, 0, MAXPGPATH);
		sprintf(check_path, "%s/%s", DataDir, path_array[loop]);

		if(!is_dir_exist(check_path))
		{
			if(!create_dir(check_path))
			{
				ereport(ERROR, (errcode_for_file_access(),
						errmsg("could not create dir \"%s\": %m",check_path)));
			}
		}
	}
}


/*
 * 函数返回对一个路径的检查结果：
 * 
 * PATH_KIND_INVALID 不存在的路径或者文件路径，或者无读写权限的路径或者文件路径
 * define PATH_KIND_NULL 参数为空
 * define PATH_KIND_DIR  这是一个有读写权限的目录
 * define PATH_KIND_SFILE 这是一个有读写权限的没有路径的文件
 * define PATH_KIND_FILE 这是一个有读写权限的有路径的文件
 * 
 */
int
path_judge(char *path)
{
	int 		result = PATH_KIND_INVALID;
	char	   *directory = NULL;
	char	   *fname = NULL;
	int			posend = 0;
	
	if(!path || 0 == strcmp("",path))
		return PATH_KIND_NULL;
	
	if(1 == is_dir_exist(path))
	{
		//这是一个目录，有权限返会这是目录，无权限返回invalid
		if(!is_path_write_access(path))
			result = PATH_KIND_INVALID;
		else
		{
			posend = strlen(path) - 1;
			if('/' == path[posend])
				path[posend] = 0;
			result = PATH_KIND_DIR;
		}
		return result;
	}
	
	split_path_fname(path, &directory, &fname);
	if(strlen(fname) == strlen(path))
	{
		result = PATH_KIND_SFILE;	//相对路径，只有一个文件名
	}
	else if(1 == is_dir_exist(directory))
	{
		result = PATH_KIND_FILE;	
	}
	else
	{
		// 这里result = PATH_KIND_INVALID防止是一些没有查看权限的目录;
	}
	
	if(fname)
		pfree(fname);
	if(directory)
		pfree(directory);
	return result;
}

static void
check_user(void)
{	
	bool result = false;
	result = superuser();
	if(!result)
		ereport(ERROR,(errmsg("Only the superuser execute walminer.")));
}


void
check_all(void)
{
	check_user();
	check_dir();
}

FILE*
prepare_logfile(void)
{
	char	log_path[MAXPGPATH] = {0};
	FILE	*fp = NULL;

	get_log_pach(log_path);
	fp = fopen(log_path, "w+");
	if(!fp)
	{
		elog(ERROR, "Can not open file %s to write", log_path);
	}
	return fp;
}

void
end_logfile(void)
{
	if(wdecoder.logout)
		fclose(wdecoder.logfp);
}

FILE*
prepare_debugfile(void)
{
	char	*log_path = NULL;
	FILE	*fp = NULL;

	if(log_path)
	{
		fp = fopen(log_path, "w+");
		if(!fp)
		{
			elog(ERROR, "Can not open file %s to write", log_path);
		}
	}
	return fp;
}

void
end_debug_file(void)
{
	if(debug_mode && wdecoder.debugfp)
	{
		fclose(wdecoder.debugfp);
		wdecoder.debugfp = NULL;
	}
}

char*
walminer_malloc(int size,int checkflag)
{
	char *result = NULL;

	result = (char*)palloc0(size);
	return result;
}

void
walminer_free(char* ptr,int checkflag)
{
	if(ptr)
	{
		pfree(ptr);
	}
}

uint64
proCheckBit(char *outPtr, int outsize)
{
	int		loop = 0;
	uint64	checkbit = 0;

	for(;loop < outsize; loop++)
	{
		checkbit += outPtr[loop];
	}
	return checkbit;
}

void
cheCheckBit(char *outPtr, int outsize, uint64 checkbit)
{
	int		loop = 0;
	uint64	checkbitcal = 0;
	
	for(;loop < outsize; loop++)
	{
		checkbitcal += outPtr[loop];
	}
	if(checkbitcal != checkbit)
		elog(ERROR,"Invalid data dictionary file.");
}

void
walminer_elog(const char *fmt,...)
{
	va_list		args;

	if(!wdecoder.logout)
		return;

	va_start(args, fmt);
	vfprintf(wdecoder.logfp, _(fmt), args);
	va_end(args);
	fputc('\n', wdecoder.logfp);

	/* TODO(lchch) log输出效率提升*/
	fflush(wdecoder.logfp);
}

void
walminer_debug(const char *fmt,...)
{
	va_list		args;
	FILE		*temp = NULL;

	if(!debug_mode)
		return;
	
	if(wdecoder.debugfp)
	{
		temp = wdecoder.debugfp;
	}
	else if(wdecoder.logfp)
	{
		temp = wdecoder.logfp;
	}
	else
	{
		temp = stdout;
	}

	lock_walminer_thread(2);
	va_start(args, fmt);
	vfprintf(temp, _(fmt), args);
	va_end(args);
	fputs("\n", temp);

	/* TODO(lchch) log输出效率提升*/
	fflush(temp);
	unlock_walminer_thread(2);
}

char*
get_dict_path(char *dict_path)
{
	Assert(dict_path);

	memset(dict_path, 0, MAXPGPATH);
	sprintf(dict_path, "%s/pg_walminer/wm_datadict/%s", DataDir, WALMINER_DICTIONARY_DEFAULTNAME);

	return dict_path;
}

char*
get_runtemp_path(char *runtemp_path)
{
	Assert(runtemp_path);

	memset(runtemp_path, 0, MAXPGPATH);
	sprintf(runtemp_path, "%s/pg_walminer/wm_image/", DataDir);

	return runtemp_path;
}

//wm_analyselog
char*
get_log_pach(char *log_path)
{
	Assert(log_path);

	memset(log_path, 0, MAXPGPATH);
	sprintf(log_path, "%s/pg_walminer/wm_analyselog/%s", DataDir, WALMINER_DICTIONARY_LOGFILE);

	return log_path;
}

char*
get_transaction_pach(char *log_path, char* filename)
{
	Assert(log_path);

	memset(log_path, 0, MAXPGPATH);
	sprintf(log_path, "%s/pg_walminer/wm_analyselog/%s", DataDir, filename);

	return log_path;
}

char*
get_failure_pach(char *failure_path)
{
	Assert(failure_path);

	memset(failure_path, 0, MAXPGPATH);
	sprintf(failure_path, "%s/pg_walminer/wm_analyselog/%s", DataDir, WALMINER_DICTIONARY_APPLY_FAILURE);

	return failure_path;
}

char*
get_failure_temp_pach(char *failure_path_temp)
{
	Assert(failure_path_temp);

	memset(failure_path_temp, 0, MAXPGPATH);
	sprintf(failure_path_temp, "%s/pg_walminer/wm_analyselog/%s", DataDir, WALMINER_DICTIONARY_APPLY_FAILURE_TEMP);

	return failure_path_temp;
}

char*
get_image_path(char *image_path)
{
	Assert(image_path);

	memset(image_path, 0, MAXPGPATH);
	sprintf(image_path, "%s/pg_walminer/wm_image/", DataDir);

	return image_path;
}


void
split_path_fname(const char *path, char **dir, char **fname)
{
	char	   *sep = NULL;
	int			length_dir = 0;
	int			length_fname = 0;
	

	/* split filepath into directory & filename */
#ifdef WIN32
	sep = strrchr(path, '\\');
	if(!sep)
		sep = strrchr(path, '/');
#else
	sep = strrchr(path, '/');
#endif

	/* directory path */
	if (sep != NULL)
	{	
		length_dir = sep - path;
		length_fname = strlen(sep + 1);

		*dir = walminer_malloc(length_dir + 1,0);
		memcpy(*dir, path, length_dir);
		*fname = walminer_malloc(length_fname + 1,0);
		memcpy(*fname, sep + 1, length_fname);

	}
	/* local directory */
	else
	{
		length_dir = strlen(path);
		*fname = walminer_malloc(length_dir + 1,0);
		*dir = NULL;
		memcpy(*fname, path, length_dir);
	}
}

bool
is_empt_str(char *str)
{
	bool	result = false;
	char 	*strptr;
	if(!str)
	{
		result = true;
		return result;
	}
	strptr = str;

	if(0 == strcmp("NULL",strptr))
		result = true;
	else if(0 == strcmp("null",strptr))
		result = true;
	else if(0 == strcmp("\"\"",strptr))
		result = true;
	else if(0 == strcmp("",strptr))
		result = true;
	return result;
}

int
scan_dir_get_filenum(char *scdir)
{
	int				filecount = 0;
	DIR				*pDir = NULL;
	char			dir[MAXPGPATH];
	struct stat 	statbuf;
	struct dirent*	ent = NULL;
	
	if(!scdir)
		return filecount;
	if (NULL == (pDir = opendir(scdir)))
		ereport(ERROR,
				(errcode(ERRCODE_SYSTEM_ERROR),
				errmsg("Open dir \"%s\" failed", scdir)));

	while (NULL != (ent = readdir(pDir)))
	{
#ifdef WIN32
		snprintf(dir, MAXPGPATH, "%s\\%s", scdir, ent->d_name);
#else
		snprintf(dir, MAXPGPATH, "%s/%s", scdir, ent->d_name);
#endif
		lstat(dir, &statbuf);
		if (!S_ISDIR(statbuf.st_mode))
		{
			if(0 == strcmp("..",ent->d_name) || 0 == strcmp(".",ent->d_name))
				continue;
			filecount++;
		}
	}
	closedir(pDir);

	return filecount;
}

int
scan_dir_get_filename(char *scdir,NameData *datafilename, bool sigfile)
{
	int				filecount = 0;
	int				filenamlength = 0;
	DIR				*pDir = NULL;
	char			dir[MAXPGPATH];
	struct stat 	statbuf;
	struct dirent*	ent = NULL;
	
	if(!scdir)
		return filecount;
	if (NULL == (pDir = opendir(scdir)))
		ereport(ERROR,
				(errcode(ERRCODE_SYSTEM_ERROR),
				errmsg("Open dir \"%s\" failed", scdir)));

	while (NULL != (ent = readdir(pDir)))
	{
#ifdef WIN32
		snprintf(dir, MAXPGPATH, "%s\\%s", scdir, ent->d_name);
#else	
		snprintf(dir, MAXPGPATH, "%s/%s", scdir, ent->d_name);
#endif
		lstat(dir, &statbuf);
		if (S_ISREG(statbuf.st_mode))
		{
			if(0 == strcmp("..",ent->d_name) || 0 == strcmp(".",ent->d_name))
				continue;
			filenamlength = strlen(ent->d_name);
			if(NAMEDATALEN <= filenamlength)
				ereport(ERROR,
				(errcode(ERRCODE_INVALID_SQL_STATEMENT_NAME),
				errmsg("Filename \"%s\" is too long",ent->d_name)));
			filecount++;
			if(sigfile && 1 < filecount)
				break;
			strncpy(datafilename[filecount - 1].data, ent->d_name, sizeof(NameData));
			
		}
	}
	closedir(pDir);

	return filecount;
}


void
drop_allfile_in_dir(char *dir)
{
	char		temp[MAXPGPATH] = {0};
	NameData	*filenamelist = NULL;
	int			filenum = 0,i = 0;

	if(!is_dir_exist(dir))
	{
		return;
	}

	filenum = scan_dir_get_filenum(dir);
	if(0 < filenum)
	{
		filenamelist = (NameData *)palloc0(filenum * sizeof(NameData));
		scan_dir_get_filename(dir, filenamelist,false);
		for(; i < filenum; i++)
		{
			memset(temp,0,MAXPGPATH);
			snprintf(temp, MAXPGPATH, "%s/%s", dir, filenamelist[i].data);
			remove(temp);
		}
	}
}

#ifndef PG_VERSION_10
int
get_wal_seg_size(char* path)
{
	FILE	*fp = NULL;
	PGAlignedXLogBlock buf;
	int		seg_size = 0;


	memset(&buf, 0, sizeof(PGAlignedXLogBlock));

	Assert(path);
	fp = fopen(path, "r");
	if(!fp)
	{
		elog(ERROR,"can not open file %s to read", path);
	}
	if (XLOG_BLCKSZ == fread(buf.data, 1, XLOG_BLCKSZ, fp))
	{
		XLogLongPageHeader longhdr = (XLogLongPageHeader) buf.data;

		seg_size = longhdr->xlp_seg_size;

		if (!IsValidWalSegSize(seg_size))
			elog(ERROR,"wrong walsegsize %d",seg_size);
	}
	else
	{
		int err = errno;
		if (err != 0)
			elog(ERROR,"could not read file \"%s\": %s", path, strerror(err));
		else
			elog(ERROR,"not enough data in file \"%s\"", path);
	}
	fclose(fp);
	return seg_size;
}
#endif

ReorderBufferChange *
get_change_space(void)
{
	ReorderBufferChange *change;

	change = (ReorderBufferChange *)walminer_malloc(sizeof(ReorderBufferChange), 0);

	return change;
}

ReorderBufferTupleBuf *
get_tuple_space(Size tuple_len)
{
	ReorderBufferTupleBuf *tuple;
	Size		alloc_len;

	alloc_len = tuple_len + SizeofHeapTupleHeader;

	tuple = (ReorderBufferTupleBuf *)
		walminer_malloc(sizeof(ReorderBufferTupleBuf) + MAXIMUM_ALIGNOF + alloc_len, 0);
	tuple->alloc_tuple_size = alloc_len;
	tuple->tuple.t_data = ReorderBufferTupleBufData(tuple);

	return tuple;
}

void
fix_path_end(char *path)
{
	int pathLength = 0;
	pathLength = strlen(path);
	if('/' == path[pathLength - 1])
		path[pathLength - 1] = 0;
}

bool
number_in_array(int *array, int len, int num)
{
	int		loop = 0;
	bool	result = false;

	for(; loop < len; loop++)
	{
		if(num == array[loop])
		{
			result = true;
			break;
		}
	}

	return result;
}

void
trim_space(char *str)
{
	int		len;
	int		i = 0;
	int		start_loc = 0;
	int		end_loc = 0;

	if(!str)
		return;

	len = strlen(str);

	for(; i < len; i++)
	{
		if(' ' != str[i] && '	' != str[i])
			break;
	}
	start_loc = i;

	for(i = len - 1; i >=0; i--)
	{
		if(' ' != str[i] && '	' != str[i])
			break;
	}
	end_loc = i;

	if(0 != start_loc && len != end_loc + 1)
		memmove(str, str + start_loc, end_loc - start_loc + 1);

	str[end_loc - start_loc + 1] = 0;
}
