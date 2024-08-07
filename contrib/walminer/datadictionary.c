/*
 * datadictionary.c
 */
#include "datadictionary.h"
#include "wm_utils.h"
#include "utils/timestamp.h"
#include "access/xlog.h"
#include "miscadmin.h"
#include "access/genam.h"
#include "storage/copydir.h"
#include "utils/snapmgr.h"
#include "access/heapam.h"
#include "utils/rel.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "catalog/pg_control.h"
#include "common/controldata_utils.h"
#include "storage/fd.h"

static void create_dictionary_file(char *target_path);
static void wal_load_relmap_file(RelMapFile *map, bool shared);
static void built_relmap(char *target_path);
static void free_scc(SingleCatalogCache *scc);
static void clean_scc(SingleCatalogCache *scc);
static void build_single_catalog_table(SingleCatalogCache  *scc, char *target_path);
static void load_catalog_table(WalminerDataDic *wdd, char *store_path);
static void load_singrel_map(FILE *fp, RelMapFile *map);
static void load_single_catalog_table(FILE *fp, int tabid, SingleCatalogCache *scc, int sigsize);
//static void debug_dict(void);
static void built_ddh(char *target_path);
static void built_catalog_table(char *target_path);
static void search_catalog_relation(void);
static char* scc_getnext(int search_tabid,CatalogTableStruct *stc);
static HTAB *create_tuple_desc_hash(void);
static HTAB *create_relpkey_hash(void);
static TupleDesc get_desc_by_reloid_real(Oid reloid);
static void put_tuple_desc_to_cache(Oid reloid, TupleDesc tupleDesc);
static void built_constraint(char *target_path);
static void load_constraint(FILE *fp, ConstrintInDic *cid);
static bool get_pkey_list_by_reloid_real(Oid reloid, int16 **pklist);

WalminerDataDic wdd;

CatalogTableStruct imp_catalog_table[WALMINER_IMPTSYSCLASS_MAX + 1] = {
	{WALMINER_IMPTSYSCLASS_PGCLASS, "pg_class", sizeof(FormData_pg_class) + sizeof(Oid)},
	{WALMINER_IMPTSYSCLASS_PGATTRIBUTE, "pg_attribute", sizeof(FormData_pg_attribute) + sizeof(Oid)},
	{WALMINER_IMPTSYSCLASS_PGDATABASE, "pg_database", sizeof(FormData_pg_database) + sizeof(Oid)},
	{WALMINER_IMPTSYSCLASS_PGNAMESPACE, "pg_namespace", sizeof(FormData_pg_namespace) + sizeof(Oid)},
	{WALMINER_IMPTSYSCLASS_PGTABLESPACE, "pg_tablespace", sizeof(FormData_pg_tablespace) + sizeof(Oid)},
	{WALMINER_IMPTSYSCLASS_PGAUTHID, "pg_authid", sizeof(FormData_pg_authid) + sizeof(Oid)},
	{WALMINER_IMPTSYSCLASS_PGTYPE, "pg_type", sizeof(FormData_pg_type) + sizeof(Oid)},
	{WALMINER_IMPTSYSCLASS_PGAUTH_MEMBERS, "pg_auth_members", sizeof(FormData_pg_auth_members) + sizeof(Oid)},

	{WALMINER_IMPTSYSCLASS_MAX, "", 0}
};

static void
avatar_dictionary(void)
{
	Form_pg_class		fpc = NULL;
	char 				*serchPtr = NULL;
	CatalogTableStruct	*cts = NULL;
	int					search_tabid = WALMINER_IMPTSYSCLASS_PGCLASS;
	List				*user_map_relfilenode = NULL;
	ListCell			*cell = NULL;

	user_map_relfilenode = wdd.user_map_relfilenode;
	cts = (CatalogTableStruct*)imp_catalog_table;

	foreach(cell, user_map_relfilenode)
	{
		MapRelfilenode *mr = NULL;
		bool			avatared = false;

		mr = lfirst(cell);
		while(NULL != (serchPtr = scc_getnext(search_tabid ,cts)))
		{
			fpc = (Form_pg_class)(serchPtr + sizeof(Oid));
			if(mr->node_in_database == fpc->relfilenode)
			{
				fpc->relfilenode = mr->node_in_wal;
				avatared = true;
				break;
			}
		}
		if(!avatared)
			elog(ERROR, "[avatar_dictionary]Can not find relfilenode %u", mr->node_in_database);
	}
	wdd.scc[search_tabid].cursor = wdd.scc[search_tabid].data;

}

static void
search_catalog_relation(void)
{
	Form_pg_class		fpc = NULL;
	char 				*serchPtr = NULL;
	
	int					search_tabid = 0;
	int					syscount = 0;
	char				*relname_source = NULL;
	char				*relname_opt = NULL;

	search_tabid = WALMINER_IMPTSYSCLASS_PGCLASS;
	
	while(NULL != (serchPtr = scc_getnext(search_tabid, imp_catalog_table)))
	{
		fpc = (Form_pg_class)(serchPtr + sizeof(Oid));
		if (fpc->relnamespace == PG_CATALOG_NAMESPACE &&
			fpc->relkind == RELKIND_RELATION)
		{
			relname_source = fpc->relname.data;
			relname_opt = wdd.crelation[syscount].classname.data;
			memcpy(relname_opt, relname_source, sizeof(NameData));
			syscount++;
		}
	}
	wdd.crnum = syscount;
	wdd.scc[search_tabid].cursor = wdd.scc[search_tabid].data;
}

/*
 * 预创建一个目标数据字典文件，如果目标文件已经存在那么报错。
 * 
 * 如果target_path为目录路径格式或者已经存在的目录，那么尝试在这个目录下创建一个WALMINER_DICTIONARY_DEFAULTNAME文件
 * 如果target_path为一个文件路径格式，那么尝试创建这个文件
 * 
 * 只允许绝对路径入参
 * 
 */

static void
create_dictionary_file(char *target_path)
{
	if(is_file_exist(target_path))
		elog(ERROR,"Dictionary file \"%s\" is already exist.", target_path);
	create_file(target_path);
}

void
build_dictionary(char *target_path)
{	
	int		path_kind = 0;

	char	final_path[MAXPGPATH] = {0};

	if(!is_absolute_path(target_path))
	{
		elog(ERROR, "Absolute path only.");
	}

	path_kind = path_judge(target_path);
	if(PATH_KIND_INVALID == path_kind || PATH_KIND_NULL == path_kind)
	{
		if(is_empt_str(target_path))
		{
			elog(ERROR, "Please enter a file path or directory.");
		}
		else	
			elog(ERROR,"File or directory \"%s\" access is denied or does not exists.",target_path);
	}
	else if(PATH_KIND_DIR == path_kind)
	{
		sprintf(final_path, "%s/%s", target_path, WALMINER_DICTIONARY_DEFAULTNAME);
	}
	else if(PATH_KIND_FILE == path_kind || PATH_KIND_SFILE == path_kind)
	{
		sprintf(final_path, "%s", target_path);
	}

	create_dictionary_file(final_path);
	built_ddh(final_path);
	built_relmap(final_path);
	built_catalog_table(final_path);
	built_constraint(final_path);
}

/*
 * 加载数据字典
 * 
 * load=1：此时为执行了walminer_load_dictionary()
 * load=2：此时为加载wal日志时向内存加载数据字典,此时如果数据字典不存在需要重新创建
 * load=3: 此时为执行解析时向内存加载数据字典
 * 
 */
void
load_dictionary(char *target_path, int load)
{
	int 	path_kind = 0;
	char	final_path[MAXPGPATH] = {0}; //使用入参路径计算出来的路径
	List	*temp_list = NULL;
	bool	temp_bool = false;

	if(wdd.loaded)
	{
		elog(ERROR, "dictionary already loaded.");
	}

	if(!is_absolute_path(target_path))
	{
		elog(ERROR, "Absolute path only.");
	}
	path_kind = path_judge(target_path);
	if (PATH_KIND_FILE == path_kind)
	{
		sprintf(final_path, "%s", target_path);
	}
	else if (PATH_KIND_DIR == path_kind)
	{
		sprintf(final_path, "%s/%s", target_path, WALMINER_DICTIONARY_DEFAULTNAME);
	}
	else
	{
		elog(ERROR, "wrong argument %s for dict file", target_path);
	}

	if(!is_file_exist(final_path) && WALMINER_LOADDICT_ADDWAL == load)
	{
		build_dictionary(final_path);
	}
	else if(!is_file_exist(final_path))
	{
		elog(ERROR, "Dictionary %s does not exists.", final_path);
	}
	temp_list = wdd.user_map_relfilenode;
	temp_bool = wdd.set_user_map;
	memset(&wdd, 0, sizeof(WalminerDataDic));
	wdd.user_map_relfilenode = temp_list;
	wdd.set_user_map = temp_bool;
	load_catalog_table(&wdd, final_path);
	
	wdd.loaded = true;
	//debug_dict();
}

static void
load_catalog_table(WalminerDataDic *wdd, char *store_path)
{
	FILE				*fp = NULL;
	int					loop = 0;
	
	fp = fopen(store_path,"rb");
	if(!fp)
	{
		elog(ERROR, "Can not load dict %s:%m", store_path);
	}
	fread(&wdd->ddh ,sizeof(DataDicHeader),1,fp);
	// TODO(lchch):checkbit
	load_singrel_map(fp, &wdd->shared_map);
	load_singrel_map(fp, &wdd->local_map);
	for(loop = 0; loop < WALMINER_IMPTSYSCLASS_MAX - 1; loop++)
	{
		load_single_catalog_table(fp, loop, &wdd->scc[loop], imp_catalog_table[loop].tuplesize);
	}
	load_constraint(fp, &wdd->cid); // 优化做成哈希
	search_catalog_relation();
	avatar_dictionary();
	fclose(fp);
}

/*
static void
debug_dict(void)
{
	int loop = 0;

	for(loop = 0; loop < WALMINER_IMPTSYSCLASS_MAX - 1; loop++)
	{
		SingleCatalogCache *scc = NULL;
		scc = &wdd.scc[loop];	
	}

}
*/

static void
load_single_catalog_table(FILE *fp, int tabid, SingleCatalogCache *scc, int sigsize)
{
	uint64 temp_check_bit = 0;

	if(!fp || !scc)
		return;
	
	fread(&scc->cth, sizeof(CatalogTableHead), 1, fp);
	temp_check_bit = scc->cth.checkbit1;
	scc->cth.checkbit1 = 0;
	cheCheckBit((char*)(&scc->cth), sizeof(CatalogTableHead), temp_check_bit);
	scc->cth.checkbit1 = temp_check_bit;
	

	add_space_for_cts(scc, sigsize * scc->cth.elemnum);
	
	fread(scc->data, sigsize, scc->cth.elemnum, fp);
	scc->usesize = sigsize * scc->cth.elemnum;
	cheCheckBit(scc->data, scc->usesize, scc->cth.checkbit2);
}

static void
load_singrel_map(FILE *fp, RelMapFile *map)
{
	pg_crc32c	crc;
	
	Assert(fp && map);

	if(sizeof(RelMapFile) != fread(map, 1, sizeof(RelMapFile), fp))
	{
		ereport(ERROR,
				(errcode(ERRCODE_SYSTEM_ERROR),
				errmsg("Fail to read from datadictionay")));
	}
	if (map->magic != RELMAPPER_FILEMAGIC ||
		map->num_mappings < 0 ||
		map->num_mappings > MAX_MAPPINGS)
		ereport(ERROR,
				(errmsg("relation mapping file in dic contains invalid data")));

	INIT_CRC32C(crc);
	COMP_CRC32C(crc, (char *) map, offsetof(RelMapFile, crc));
	FIN_CRC32C(crc);

	if (!EQ_CRC32C(crc, map->crc))
		ereport(ERROR,(errmsg("relation mapping file in dic contains incorrect checksum")));
}



static void
build_single_catalog_table(SingleCatalogCache  *scc, char *target_path)
{
	FILE				*fp = NULL;

	fp = fopen(target_path,"ab");
	if(!fp)
		elog(ERROR, "Can not create dictionary file \"%s\"", target_path);

	scc->cth.checkbit2 = proCheckBit((char*)(scc->data), scc->usesize);
	scc->cth.checkbit1 = proCheckBit((char*)(&scc->cth), sizeof(CatalogTableHead));
	fwrite(&scc->cth, sizeof(CatalogTableHead), 1, fp);
	fwrite(scc->data, scc->usesize, 1, fp);



	fclose(fp);
}

static void
built_catalog_table(char *target_path)
{
	int 				loop = 0;
	SingleCatalogCache  scc;

	memset(&scc, 0, sizeof(SingleCatalogCache));

	for(loop = 0; loop < WALMINER_IMPTSYSCLASS_MAX - 1; loop++)
	{
		char 				*relname = NULL;
		Oid					reloid = 0;

		relname = imp_catalog_table[loop].ct_name;

		get_relation_oid_by_relname(relname, &reloid, false);
		strncpy(scc.cth.relname.data, relname, sizeof(NameData));
		fetch_single_catalog(reloid, imp_catalog_table[loop].tuplesize, &scc);
		build_single_catalog_table(&scc, target_path);
		clean_scc(&scc);
	}
	free_scc(&scc);
}

/*
static void
test_output_constraint(ConstrintInDic *cid)
{
	int	i = 0;
	int j = 0;

	for(i = 0; i < cid->relnum; i++)
	{
		printf("\nrelid=%u, keys=%d:\n", cid->pkilist[i].reloid, cid->pkilist[i].keys);
		for(j = 0; j < cid->pkilist[i].keys; j++)
		{
			printf("%d,", cid->pkilist[i].keynum[j]);
		}
	}
}
*/

static void
built_constraint(char *target_path)
{
	FILE				*fp = NULL;
	ConstrintInDic		cid;

	memset(&cid, 0, sizeof(ConstrintInDic));
	fetch_relation_pkey(&cid);
	cid.relnum = cid.indnum;
	//test_output_constraint(&cid);

	fp = fopen(target_path,"ab");
	if(!fp)
		elog(ERROR, "Can not create dictionary file \"%s\"", target_path);

	fwrite(&cid, sizeof(ConstrintInDic), 1,fp);
	fwrite(cid.pkilist , sizeof(PKeyItem) * cid.relnum, 1,fp);
	

	fclose(fp);
}

static void
load_constraint(FILE *fp, ConstrintInDic *cid)
{
	int		size_data = 0;
	int		read_size = 0;

	read_size = fread(cid, 1, sizeof(ConstrintInDic), fp);
	if(read_size != sizeof(ConstrintInDic))
	{
		elog(ERROR, "Read wrong size 1 expect %ld get %d", sizeof(ConstrintInDic), read_size);
	}
	size_data = cid->relnum * sizeof(PKeyItem);
	cid->pkilist = (PKeyItem*)walminer_malloc(size_data, 0);

	read_size = fread(cid->pkilist, 1, size_data, fp);
	if(read_size != size_data)
	{
		elog(ERROR, "Read wrong size 2 expect %d get %d", size_data, read_size);
	}
	//test_output_constraint(cid);
}

static void
built_ddh(char *target_path)
{
	FILE			*fp = NULL;
	DataDicHeader	ddh;
#if ((defined PG_VERSION_15) || (defined PG_VERSION_16))
	ControlFileData *ControlFile = NULL;
	bool			crc_ok = true;
#endif
	memset(&ddh, 0,  sizeof(DataDicHeader));
	fp = fopen(target_path,"wb");
	if(!fp)
		ereport(ERROR,
				(errcode(ERRCODE_SYSTEM_ERROR),
				errmsg("Can not create dictionary file %s",target_path)));
#if ((defined PG_VERSION_15) || (defined PG_VERSION_16))
	ControlFile = get_controlfile(DataDir, &crc_ok);
	if (!crc_ok)
		ereport(ERROR,
				(errmsg("calculated CRC checksum does not match value stored in file")));
#endif
#if ((defined PG_VERSION_15) || (defined PG_VERSION_16))
	ddh.timeline = ControlFile->checkPointCopy.ThisTimeLineID;
#else
	ddh.timeline = ThisTimeLineID;
#endif
	
	ddh.dboid = MyDatabaseId;
	ddh.sysid = GetSystemIdentifier();;
	ddh.btime = GetCurrentTimestamp();

	ddh.checkbit = ddh.timeline + ddh.dboid + ddh.sysid + ddh.btime;

	fwrite(&ddh, sizeof(DataDicHeader), 1,fp);
	fclose(fp);
}



static void
wal_load_relmap_file(RelMapFile *map, bool shared)
{
	char		mapfilename[MAXPGPATH];
	pg_crc32c	crc;
	int			fd;

	if (shared)
	{
		snprintf(mapfilename, sizeof(mapfilename), "global/%s",
				 RELMAPPER_FILENAME);
	}
	else
	{
		snprintf(mapfilename, sizeof(mapfilename), "%s/%s",
				 DatabasePath, RELMAPPER_FILENAME);
	}

	/* Read data ... */
#ifdef PG_VERSION_10
	fd = OpenTransientFile(mapfilename, O_RDONLY | PG_BINARY, S_IRUSR | S_IWUSR);
#else
	fd = OpenTransientFile(mapfilename, O_RDONLY | PG_BINARY);
#endif
	if (fd < 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not open relation mapping file \"%s\": %m",
						mapfilename)));

	/*
	 * Note: we could take RelationMappingLock in shared mode here, but it
	 * seems unnecessary since our read() should be atomic against any
	 * concurrent updater's write().  If the file is updated shortly after we
	 * look, the sinval signaling mechanism will make us re-read it before we
	 * are able to access any relation that's affected by the change.
	 */
	if (read(fd, map, sizeof(RelMapFile)) != sizeof(RelMapFile))
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not read relation mapping file \"%s\": %m",
						mapfilename)));

	CloseTransientFile(fd);

	/* check for correct magic number, etc */
	if (map->magic != RELMAPPER_FILEMAGIC ||
		map->num_mappings < 0 ||
		map->num_mappings > MAX_MAPPINGS)
		ereport(FATAL,
				(errmsg("relation mapping file \"%s\" contains invalid data",
						mapfilename)));

	/* verify the CRC */
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, (char *) map, offsetof(RelMapFile, crc));
	FIN_CRC32C(crc);

	if (!EQ_CRC32C(crc, map->crc))
		ereport(FATAL,
				(errmsg("relation mapping file \"%s\" contains incorrect checksum",
						mapfilename)));
}

static void
built_relmap(char *target_path)
{
	FILE			*fp = NULL;
	RelMapFile 		shared_map;
	RelMapFile 		local_map;

	memset(&shared_map, 0, sizeof(RelMapFile));
	memset(&local_map, 0, sizeof(RelMapFile));

	walminer_debug("[built_ddh]target_path=%s", target_path);

	wal_load_relmap_file(&shared_map, true);
	wal_load_relmap_file(&local_map, false);
	fp = fopen(target_path,"ab");
	if(!fp)
		elog(ERROR, "built_relmap:Can not create dictionary file \"%s\"", target_path);
	if(sizeof(RelMapFile) != fwrite(&shared_map, 1, sizeof(RelMapFile), fp))
	{
		elog(ERROR, "built_relmap:Fail to write to %s", target_path);
	}
	if(sizeof(RelMapFile) != fwrite(&local_map, 1, sizeof(RelMapFile), fp))
	{
		elog(ERROR, "built_relmap:Fail to write to %s", target_path);
	}
	fclose(fp);
}

void
add_space_for_cts(SingleCatalogCache *scc, int stepsize)
{
	char *temp_space = NULL;
	int	 relstep = 0;

	relstep = (0 == stepsize)?WALMINER_DICTIONARY_CATALOG_ADD_SIZE:stepsize;
	
	temp_space = walminer_malloc(scc->totsize + relstep,0);
	if(!temp_space)
		elog(ERROR, "add_space_for_cts: out of memory");
	if(scc->data)
	{
		memcpy(temp_space, scc->data, scc->totsize);
		walminer_free(scc->data,0);
	}
	scc->totsize += relstep;
	scc->data = temp_space;
	scc->cursor = temp_space + scc->usesize;
	temp_space = NULL;
}


static void
clean_scc(SingleCatalogCache *scc)
{
	if(!scc)
		return;
	memset(&scc->cth, 0, sizeof(CatalogTableHead));

	if(!scc->data)
		return;
	memset(scc->data, 0, scc->totsize);
	scc->usesize = 0;
	scc->cth.elemnum = 0;
	scc->cursor = scc->data;
}

static void
free_scc(SingleCatalogCache *scc)
{
	if(!scc)
		return;
	if(!scc->data)
		return;
	walminer_free(scc->data,0);
	memset(scc, 0, sizeof(SingleCatalogCache));
}

void
free_dictionary(void)
{
	int	loop = 0;

	if(!wdd.loaded)
	{
		if(wdd.user_map_relfilenode)
			list_free(wdd.user_map_relfilenode);
		wdd.user_map_relfilenode = NULL;
		return;
	}
	
	for(loop = 0; loop < WALMINER_IMPTSYSCLASS_MAX - 1; loop++)
	{
		free_scc(&wdd.scc[loop]);
	}
	if(wdd.user_map_relfilenode)
		list_free(wdd.user_map_relfilenode);
	memset(&wdd, 0, sizeof(WalminerDataDic));
	wdd.loaded = false;
}



/* 
 * -----------------------------------------------------------------------
 * 以下代码为在解析过程中检索数据字典使用 
 */
static char*
walminer_get_form_by_oid(int search_tabid, Oid reloid)
{
	char				*fpg = NULL;
	char 				*serchPtr = NULL;
	char				*result = NULL;
#if (defined PG_VERSION_10)  || (defined PG_VERSION_11)
	int					*oidPtr = NULL;
#else
	int					oidvalue = 0;
#endif

	while(NULL != (serchPtr = scc_getnext(search_tabid,imp_catalog_table)))
	{
		fpg = serchPtr + sizeof(Oid);
#if (defined PG_VERSION_10)  || (defined PG_VERSION_11)
		oidPtr = (int *)serchPtr;
		if(reloid == *oidPtr)
#else
		oidvalue = *(int*)fpg;
		if(reloid == oidvalue)
#endif
		{
			result = fpg;
			break;
		}
	}
	wdd.scc[search_tabid].cursor = wdd.scc[search_tabid].data;
	return result;
}

static char*
scc_getnext(int search_tabid,CatalogTableStruct *stc)
{
	char	  *result = NULL;

	if(wdd.scc[search_tabid].cursor - wdd.scc[search_tabid].data < wdd.scc[search_tabid].usesize)
	{
		result = wdd.scc[search_tabid].cursor;
		wdd.scc[search_tabid].cursor += stc[search_tabid].tuplesize;
	}
	return result;
}


/* 
 * 在数据字典中根据relfilenode获取reloid，一些系统表会返回0,
 * 需要在在relmap中再次查找.
 * 
 * TODO 这个查找需要改为hash查找
 */
Oid
get_reloid_by_relfilenode(RelFileNode *rnode)
{
	Form_pg_class		fpc = NULL;
	char 				*serchPtr = NULL;
	Oid					result = 0;
	int					*oidPtr = NULL;
	CatalogTableStruct	*cts = NULL;
	int					search_tabid = WALMINER_IMPTSYSCLASS_PGCLASS;
	Oid 				relfilenode = 0;

	cts = (CatalogTableStruct*)imp_catalog_table;

	relfilenode = rnode->relNode;
	while(NULL != (serchPtr = scc_getnext(search_tabid ,cts)))
	{
		fpc = (Form_pg_class)(serchPtr + sizeof(Oid));
#if (defined PG_VERSION_10)  || (defined PG_VERSION_11)
		oidPtr = (int *)serchPtr;	
#else
		oidPtr = (int*)fpc;
#endif
		if(relfilenode == fpc->relfilenode)
		{
			result = *oidPtr;
			break;
		}
	}
	wdd.scc[search_tabid].cursor = wdd.scc[search_tabid].data;


	 if(0 == result)
    {
		bool        shared = false;

        shared = (rnode->spcNode == GLOBALTABLESPACE_OID);
        result = get_relid_by_relnode_via_map(rnode->relNode, shared);
    }

	Assert(result != 1);

    if(0 == result)
    {
		/* 在没有找到relfilenode的情况下，这个record需要丢弃，但是继续解析其他record */
		result = WALMINER_LOST_RECORD;
        //elog(ERROR, "Relfilenode %d can not be handled,maybe the datadictionary does not match.",
        //                                    rnode->relNode);
    }

	return result;
}

/* 
 * 在数据字典中根据relfilenode获取RelFileNode
 * 
 * TODO 这个查找需要改为hash查找
 */
bool
get_relnode_by_relfilenode(Oid relfilenode, RelFileNode *rnode_out)
{
	Form_pg_class		fpc = NULL;
	char 				*serchPtr = NULL;
	bool				result = false;
	CatalogTableStruct	*cts = NULL;
	int					search_tabid = WALMINER_IMPTSYSCLASS_PGCLASS;

	cts = (CatalogTableStruct*)imp_catalog_table;
	while(NULL != (serchPtr = scc_getnext(search_tabid ,cts)))
	{
		fpc = (Form_pg_class)(serchPtr + sizeof(Oid));
		if(relfilenode == fpc->relfilenode)
		{
			rnode_out->spcNode = fpc->reltablespace;
			if(0 == rnode_out->spcNode)
				rnode_out->spcNode = DEFAULTTABLESPACE_OID;
			rnode_out->relNode = relfilenode;
			rnode_out->dbNode = wdd.ddh.dboid;
			result = true;
			break;
		}
	}
	wdd.scc[search_tabid].cursor = wdd.scc[search_tabid].data;

	return result;
}

bool
get_relname_by_reloid(Oid reloid, NameData* relname)
{
	Form_pg_class	fpc = NULL;
	int				result = false;

	fpc = (Form_pg_class)walminer_get_form_by_oid(WALMINER_IMPTSYSCLASS_PGCLASS, reloid);
	if(fpc)
	{
		if(RELKIND_RELATION == fpc->relkind || RELKIND_TOASTVALUE == fpc->relkind)
		{
			/*get a result*/
			strncpy(relname->data, fpc->relname.data, sizeof(NameData));
			result = true;
		}
		else
			/*current relation not a table*/
			result = false;
	}
	return result;
}


bool
get_nspoid_by_reloid(Oid reloid, Oid *nspoid)
{
	Form_pg_class	fpc = NULL;
	bool			result = false;

	fpc = (Form_pg_class)walminer_get_form_by_oid(WALMINER_IMPTSYSCLASS_PGCLASS, reloid);
	if(fpc)
	{
		*nspoid = fpc->relnamespace;
		result = true;
	}
	return result;
}

bool
get_nsp_by_nspoid(Oid nspoid, NameData* nspname)
{

	Form_pg_namespace	fpn = NULL;
	bool				result = false;
					

	if(0 == nspoid)
		return result;
	
	fpn = (Form_pg_namespace)walminer_get_form_by_oid(WALMINER_IMPTSYSCLASS_PGNAMESPACE, nspoid);
	if(fpn)
	{
		strncpy(nspname->data, fpn->nspname.data, sizeof(NameData));
		result = true;
	}
	return result;
}

/*
 * 查找数据字典中的某一个表的主键列表
 */
static bool
get_pkey_list_by_reloid_real(Oid reloid, int16 **pklist)
{
	int		loop = 0;
	bool	result = false;

	/* 需要hash改进，优化效率 */
	for(; loop < wdd.cid.relnum; loop ++)
	{
		if(wdd.cid.pkilist[loop].reloid == reloid)
		{
			*pklist = wdd.cid.pkilist[loop].keynum;
			result = true;
		}
	}
	return result;
}


/* 
 * 在relmap中查找表的reloid
 * 
 */
Oid
get_relid_by_relnode_via_map(Oid filenode, bool shared)
{
	const RelMapFile *map;
	int32		i;
	
	/* If there are active updates, believe those over the main maps */
	if (shared)
	{
		map = &wdd.shared_map;
		for (i = 0; i < map->num_mappings; i++)
		{
			if (filenode == map->mappings[i].mapfilenode)
				return map->mappings[i].mapoid;
		}
		map = &wdd.shared_map;
		for (i = 0; i < map->num_mappings; i++)
		{
			if (filenode == map->mappings[i].mapfilenode)
				return map->mappings[i].mapoid;
		}
	}
	else
	{
		map = &wdd.local_map;
		for (i = 0; i < map->num_mappings; i++)
		{
			if (filenode == map->mappings[i].mapfilenode)
				return map->mappings[i].mapoid;
		}
		map = &wdd.local_map;
		for (i = 0; i < map->num_mappings; i++)
		{
			if (filenode == map->mappings[i].mapfilenode)
				return map->mappings[i].mapoid;
		}
	}

	return InvalidOid;
}

TupleDesc
get_desc_by_reloid_real(Oid reloid)
{
	TupleDesc			tupdesc = NULL;
	char 				*serchPtr = NULL;
	Form_pg_attribute	fpa = NULL;
	Form_pg_class		fpc = NULL;
	int					relnatts = 0;
	int					pg_class_tabid = 0;
	int					pg_attribute_tabid = 0;

	pg_class_tabid = WALMINER_IMPTSYSCLASS_PGCLASS;
	pg_attribute_tabid = WALMINER_IMPTSYSCLASS_PGATTRIBUTE;

	fpc = (Form_pg_class)walminer_get_form_by_oid(pg_class_tabid, reloid);
	if(!fpc)
		/*should not happen ,checked before*/
		elog(ERROR,"Did not find reloid %u in dictionary", reloid);
	else
	{
		relnatts = fpc->relnatts;
	}
#if (defined PG_VERSION_10)  || (defined PG_VERSION_11)
	tupdesc = CreateTemplateTupleDesc(relnatts, false);
#else
	tupdesc = CreateTemplateTupleDesc(relnatts);
#endif
	while(NULL != (serchPtr = scc_getnext(pg_attribute_tabid, imp_catalog_table)))
	{
		fpa = (Form_pg_attribute)(serchPtr + sizeof(Oid));
		if(reloid == fpa->attrelid && 0 < fpa->attnum)
		{
#ifndef PG_VERSION_10
			memcpy(&tupdesc->attrs[fpa->attnum - 1], fpa, sizeof(FormData_pg_attribute));
			tupdesc->attrs[fpa->attnum - 1].attnotnull = false;
			tupdesc->attrs[fpa->attnum - 1].atthasdef = false;
#else
			memcpy(tupdesc->attrs[fpa->attnum - 1], fpa, ATTRIBUTE_FIXED_PART_SIZE);
			tupdesc->attrs[fpa->attnum - 1]->attnotnull = false;
			tupdesc->attrs[fpa->attnum - 1]->atthasdef = false;
#endif
		}
	}
	wdd.scc[pg_attribute_tabid].cursor = wdd.scc[pg_attribute_tabid].data;
	return tupdesc;
}

int
get_relkind_by_reloid(Oid reloid)
{
	Form_pg_class		pgc = NULL;
	
	pgc =  (Form_pg_class)walminer_get_form_by_oid(WALMINER_IMPTSYSCLASS_PGCLASS, reloid);
	if(!pgc)
	{
		return -1;
	}
	return pgc->relkind;
}

/* 这个函数只支持非系统表的relfilenode获取 */
Oid
get_relfilenodeby_reloid(Oid reloid)
{
	Form_pg_class		pgc = NULL;
	
	pgc =  (Form_pg_class)walminer_get_form_by_oid(WALMINER_IMPTSYSCLASS_PGCLASS, reloid);
	if(!pgc)
	{
		return -1;
	}
	return pgc->relfilenode;
}


bool
get_typeoutput_fromdic(Oid type, Oid *typOutput, bool *typIsVarlena)
{
	Form_pg_type	pt;
	pt = (Form_pg_type)walminer_get_form_by_oid(WALMINER_IMPTSYSCLASS_PGTYPE, type);
	if(!pt)
		return false;

	*typOutput = pt->typoutput;
	*typIsVarlena = (!pt->typbyval) && (pt->typlen == -1);
	return true;
}

char*
get_typname_by_typoid(Oid type)
{
	Form_pg_type	pt;
	pt = (Form_pg_type)walminer_get_form_by_oid(WALMINER_IMPTSYSCLASS_PGTYPE, type);
	if(!pt)
		return NULL;
	return pt->typname.data;
}

bool
relkind_is_toastrel(int relkind)
{
	if(RELKIND_TOASTVALUE == relkind)
		return true;
	return false;
}

bool
relkind_is_normalrel(int relkind)
{
	if(RELKIND_RELATION == relkind)
		return true;
	return false;
}

bool
table_is_catalog_relation(char *tablename, Oid reloid)
{
	int loop;

	if(FirstNormalObjectId < reloid)
		return false;
	for(loop = 0; loop < wdd.crnum; loop++)
	{
		if(0 == strcmp(wdd.crelation[loop].classname.data,tablename))
		{
			return true;
		}
	}
	return false;
}


/* 此函数从数据库获取表，而不是从数据字典获取表 */
bool
get_reloid_by_relname(char* relname, Oid* reloid, Oid* relfilenode, bool getunloggedtable)
{
	bool				result = false;
	Relation			pgclass = NULL;
#if (defined PG_VERSION_10)  || (defined PG_VERSION_11)
	HeapScanDesc		scan = NULL;
#else
	SysScanDesc		scan = NULL;
#endif
	HeapTuple			tuple = NULL;
	Form_pg_class 		classForm = NULL;
#if (defined PG_VERSION_10)  || (defined PG_VERSION_11)
	pgclass = heap_open(RelationRelationId, AccessShareLock);
	scan = heap_beginscan_catalog(pgclass, 0, NULL);
#else
	pgclass = table_open(RelationRelationId, AccessShareLock);
	scan = systable_beginscan(pgclass, 0, false,
							   SnapshotSelf, 0, NULL);
#endif

#if (defined PG_VERSION_10)  || (defined PG_VERSION_11)
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
#else
	while ((tuple = systable_getnext(scan)) != NULL)
#endif
	{
		classForm = (Form_pg_class) GETSTRUCT(tuple);
		if(0 == strcmp(relname,classForm->relname.data))
		{
			if(getunloggedtable && RELPERSISTENCE_UNLOGGED != classForm->relpersistence)
				continue;
			/*if(!getunloggedtable && PG_CATALOG_NAMESPACE != classForm->relnamespace)
				continue;*/
#if (defined PG_VERSION_10)  || (defined PG_VERSION_11)
			if(reloid)
				*reloid = HeapTupleHeaderGetOid(tuple->t_data);
#else
			if(reloid)
				*reloid = classForm->oid;
#endif
			if(relfilenode)
			{
				*relfilenode = classForm->relfilenode;
			}
			result = true;
			break;
		}
	}
#if (defined PG_VERSION_10)  || (defined PG_VERSION_11)
	heap_endscan(scan);
	heap_close(pgclass, AccessShareLock);
#else
	systable_endscan(scan);
	table_close(pgclass, AccessShareLock);
#endif
	return result;
}

/* 此函数从数据库获取表，而不是从数据字典获取表 */
void
get_relnode_by_reloid(Oid reloid, RelFileNode* rnode)
{
	Relation	rel = NULL;

	rel = RelationIdGetRelation(reloid);
	if(!rel)
	{
		elog(ERROR, "could not open relation with OID %u", reloid);
	}
#if (defined PG_VERSION_16)
	memcpy(rnode, &rel->rd_locator, sizeof(RelFileNode));
#else
	memcpy(rnode, &rel->rd_node, sizeof(RelFileNode));
#endif
	RelationClose(rel);
}
/*
 * 此函数从数据库获取表，而不是从数据字典获取表 
 * 目前不支持系统表的映射
 * 不支持"模式.表名"的表名入参
 */
Oid
set_user_relfilenode_map(char *relname, Oid node_in_wal)
{
	Oid				node_in_database;
	bool			get_table = false;
	MapRelfilenode	*mr = NULL;

	mr = (MapRelfilenode*)walminer_malloc(sizeof(MapRelfilenode), 0);
	/* TODO(lchch)支持'模式.表名'的入参 */
	get_table = get_reloid_by_relname(relname, NULL, &node_in_database, false);
	if(!get_table)
		elog(ERROR, "Can not find relname %s in database", relname);

	mr->node_in_database = node_in_database;
	mr->node_in_wal = node_in_wal;

	wdd.user_map_relfilenode = lappend(wdd.user_map_relfilenode, mr);
	
	if(!wdd.loaded)
	/* 
	 * 如果没有加载数据字典，set_user_map会使在加载数据字典时拷贝保存
	 * user_map_relfilenode
	 */
		wdd.set_user_map = true;

	return mr->node_in_database;
}

static HTAB *
create_tuple_desc_hash(void)
{
	HASHCTL		ctl;

	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(TupleDescHashEntry);

	return hash_create("walminer TupleDesc hash", 128, &ctl, HASH_ELEM|HASH_BLOBS);
}

static HTAB *
create_relpkey_hash(void)
{
	HASHCTL		ctl;

	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(RelPkeyHashEntry);

	return hash_create("walminer RelPkey hash", 128, &ctl, HASH_ELEM|HASH_BLOBS);
}

/*
 * Returns index of matched ImageStore or -1 if not found
 */
static TupleDesc
get_tuple_desc_from_cache(Oid reloid)
{
	TupleDescHashEntry * tupleDescHashEntry;

	if (!walminer_decode_context->anapro.tupleDescHash)
		walminer_decode_context->anapro.tupleDescHash = create_tuple_desc_hash();


	tupleDescHashEntry = (TupleDescHashEntry *) hash_search(walminer_decode_context->anapro.tupleDescHash,&reloid, HASH_FIND, NULL);


	if(tupleDescHashEntry)
		return tupleDescHashEntry->tupleDesc;

	return (NULL);
}

static void
put_tuple_desc_to_cache(Oid reloid, TupleDesc tupleDesc)
{
	bool		found;
	TupleDescHashEntry * tupleDescHashEntry;

	if (!walminer_decode_context->anapro.tupleDescHash)
		walminer_decode_context->anapro.tupleDescHash = create_tuple_desc_hash();

	tupleDescHashEntry = (TupleDescHashEntry *)hash_search(walminer_decode_context->anapro.tupleDescHash, &reloid,
											   HASH_ENTER, &found);

	if (found)
	{
		ereport(ERROR,(errmsg("duplicated reloid %d in dictionary",reloid)));
	}

	tupleDescHashEntry->key = reloid;
	tupleDescHashEntry->tupleDesc = tupleDesc;
}

TupleDesc
get_desc_by_reloid(Oid reloid)
{
	TupleDesc result;

	result = get_tuple_desc_from_cache(reloid);
	if(result)
	{
		return result;
	}

	result = get_desc_by_reloid_real(reloid);
	put_tuple_desc_to_cache(reloid, result);

	return result;

}

/*
 * 从表的主键hash中查找，表，并返回RelPkeyHashEntry*
 */
static RelPkeyHashEntry*
get_relpkey_from_cache(Oid reloid)
{
	RelPkeyHashEntry* relHashEntry = NULL;

	if (!walminer_decode_context->anapro.relPkeyHash)
		walminer_decode_context->anapro.relPkeyHash = create_tuple_desc_hash();


	relHashEntry = (RelPkeyHashEntry *) hash_search(walminer_decode_context->anapro.relPkeyHash,&reloid, HASH_FIND, NULL);

	return relHashEntry;
}

static void
put_relpkey_to_cache(Oid reloid, int16 *pklist, bool haspkey)
{
	bool				found;
	RelPkeyHashEntry 	*relHashEntry = NULL;

	if (!walminer_decode_context->anapro.relPkeyHash)
		walminer_decode_context->anapro.relPkeyHash = create_relpkey_hash();

	walminer_debug("[put_relpkey_to_cache]reloid=%u, haspkey=%d", reloid, haspkey);
	relHashEntry = (RelPkeyHashEntry *)hash_search(walminer_decode_context->anapro.relPkeyHash, &reloid,
											   HASH_ENTER, &found);

	if (found)
	{
		ereport(ERROR,(errmsg("duplicated reloid %d for pkey in dictionary",reloid)));
	}

	relHashEntry->key = reloid;
	relHashEntry->haspk = haspkey;
	if(haspkey)
		relHashEntry->columnarray = pklist;
}

bool
get_pkey_list_by_reloid(Oid reloid, int16 **pklist)
{
	RelPkeyHashEntry *relHashEntry = NULL;

	if (!walminer_decode_context->anapro.relPkeyHash)
		walminer_decode_context->anapro.relPkeyHash = create_relpkey_hash();
	
	/* 尝试从hashentry中查找表的主键列表 */
	relHashEntry = get_relpkey_from_cache(reloid);
	
	/*
	 * relHashEntry为null说明，在relpkeyhash中未记录这个表，需要到主键缓存中
	 * 查找这个reloid并记录
	 */
	if(!relHashEntry)
	{
		bool	result = false;
		//查找
		result = get_pkey_list_by_reloid_real(reloid, pklist);
		//放入hash
		put_relpkey_to_cache(reloid, *pklist, result);

		return result;
	}
	
	if(!relHashEntry->haspk)
	{
		/* 从hash中查找出的结果，这个表没有主键 */
		return false;
	}
	else
	{
		/* 从hash中查找出的结果，这个表有主键，并返回主键列表 */
		*pklist = relHashEntry->columnarray;
		return true;
	}
}