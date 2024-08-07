/*
 * datadictionary.h
 * 
 * 
 */

#ifndef DATA_DICTIONARY_H
#define DATA_DICTIONARY_H

#include "postgres.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_index.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_shdescription.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_database.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_language.h"
#include "access/tupdesc.h"
#include "port/pg_crc32c.h"
#include "access/xlog_internal.h"
#include "replication/reorderbuffer.h"
#include "wm_compatible.h"

#if (defined PG_VERSION_12)  || (defined PG_VERSION_13)  || (defined PG_VERSION_14) || (defined PG_VERSION_15) || (defined PG_VERSION_16)
#include "access/table.h"
#endif

#define WALMINER_DICTIONARY_CATALOG_ADD_SIZE	(200 * 1024)
#define	WALMINER_DICTIONARY_PRIMARY_KEY_MAX		20

#define	WALMINER_DICTIONARY_LOGFILE				"walminer.log"
#define WALMINER_DICTIONARY_DEFAULTNAME			"dictionary.d"
#define WALMINER_DICTIONARY_WALLIST				"walfile.list"
#define RELMAPPER_FILENAME						"pg_filenode.map"
#define	WALMINER_DICTIONARY_APPLY_FAILURE		"apply_failure"
#define	WALMINER_DICTIONARY_APPLY_FAILURE_TEMP	"apply_failure_temp"

#define	WALMINER_LOADDICT_MOVE					1
#define	WALMINER_LOADDICT_ADDWAL				2
#define WALMINER_LOADDICT_ANALYSE				3

#define WALMINER_LOST_RECORD					1

#if (defined PG_VERSION_16)
#define MAX_MAPPINGS			64
#else
#define MAX_MAPPINGS			62
#endif
#define RELMAPPER_FILEMAGIC		0x592717	/* version ID value */

#define	WALMINER_SYSCLASS_MAX	80

/*
 * 定义重要系统表集
 */
typedef enum ICType
{
	WALMINER_IMPTSYSCLASS_PGCLASS = 0,
	WALMINER_IMPTSYSCLASS_PGATTRIBUTE,
	WALMINER_IMPTSYSCLASS_PGDATABASE,
	WALMINER_IMPTSYSCLASS_PGNAMESPACE,
	WALMINER_IMPTSYSCLASS_PGTABLESPACE,
	WALMINER_IMPTSYSCLASS_PGAUTHID,
	WALMINER_IMPTSYSCLASS_PGTYPE,
	WALMINER_IMPTSYSCLASS_PGAUTH_MEMBERS,

	/*保持为最后一个*/
	WALMINER_IMPTSYSCLASS_MAX
} ICType;

/*
 * CatalogTable为每个系统表的描述结构体
 */
typedef struct CatalogTableStruct
{
	const int   ct_no; 			//walminer为每个所需系统表的编号
	char		*ct_name;		//系统表表名
	int			tuplesize;		//这个系统表Form_pg_xxxx结构体的size
} CatalogTableStruct;

/*
 * 此结构用来记录所有的系统表
 */
typedef struct CatalogRelation{
	NameData 	classname;
	Oid			reloid;
}CatalogRelation;


/*
 * 从src/backend/utils/cache/relmapper.c复制的结构
 */
typedef struct RelMapping
{
	Oid			mapoid;			/* OID of a catalog */
	Oid			mapfilenode;	/* its filenode number */
} RelMapping;

/*
 * 从src/backend/utils/cache/relmapper.c复制的结构
 */
typedef struct RelMapFile
{
	int32		magic;			/* always RELMAPPER_FILEMAGIC */
	int32		num_mappings;	/* number of valid RelMapping entries */
	RelMapping	mappings[MAX_MAPPINGS];
	pg_crc32c	crc;			/* CRC of all above */
#ifndef PG_VERSION_16
	int32		pad;			/* to make the struct size be 512 exactly */
#endif
} RelMapFile;



typedef struct CatalogTableHead{
	NameData relname;
	int		 elemnum;	//这个系统表有的元组的个数
	uint64	 checkbit1; //for head
	uint64	 checkbit2; //for data
}CatalogTableHead;

/*
 * 导出系统表时，需要将一个系统表的全部信息存储到这个结构中，然后保存到文件中
 */
typedef struct SingleCatalogCache{
	CatalogTableHead	cth;	
	int64				usesize;
	int64				totsize;
	char*				data;
	char*				cursor;
}SingleCatalogCache;

/*
 * 数据字典数据结构
 */

typedef struct DataDicHeader{
	uint64					sysid;
	Oid						dboid;
	TimeLineID				timeline;
	TimestampTz				btime;
	int						checkbit;
}DataDicHeader;

typedef struct MapRelfilenode
{
	Oid			node_in_database;
	Oid			node_in_wal;
}MapRelfilenode;

typedef struct PKeyItem
{
	Oid				reloid;
	int				keys;
	int16			keynum[WALMINER_DICTIONARY_PRIMARY_KEY_MAX];
}PKeyItem;

typedef	struct	ConstrintInDic
{
	int			relnum;		/* 最终有的表的个数 */
	int			malnum;		/* 生成数据字典时，已经申请的数量 */
	int			indnum;		/* 生成数据字典时，正在使用的位置 */
	PKeyItem	*pkilist;
}ConstrintInDic;

typedef struct WalminerDataDic{
	DataDicHeader			ddh;
	RelMapFile				shared_map;
	RelMapFile				local_map;
	SingleCatalogCache		scc[WALMINER_IMPTSYSCLASS_MAX];
	CatalogRelation			crelation[WALMINER_SYSCLASS_MAX];
	ConstrintInDic			cid;

	List					*user_map_relfilenode;
	bool					set_user_map;
	
	int						crnum;
	bool					loaded;
}WalminerDataDic;

typedef struct WalFile	
{			
	char			filepath[MAXPGPATH];
	TimeLineID		tl;
	XLogSegNo		segno;
	bool			valid;
}WalFile;

typedef struct TupleDescHashEntry
{
	Oid				key;
	TupleDesc		tupleDesc;
} TupleDescHashEntry;

typedef struct RelPkeyHashEntry
{
	Oid				key;
	bool			haspk;
	int16			*columnarray;
} RelPkeyHashEntry;

extern WalminerDataDic wdd;
extern List	*wal_file_list;

extern void build_dictionary(char *pach);
extern bool get_relation_oid_by_relname(char* relname, Oid* reloid, bool gettemptable);
extern bool fetch_single_catalog(Oid reloid, int datasgsize, SingleCatalogCache *scc);
extern void load_dictionary(char *target_path, int load);
extern void add_space_for_cts(SingleCatalogCache *scc, int stepsize);
extern TupleDesc makeOutputXlogDesc(void);
extern TupleDesc get_desc_by_reloid(Oid reloid);
extern bool table_is_catalog_relation(char *tablename, Oid reloid);
extern bool relkind_is_toastrel(int relkind);
extern bool relkind_is_normalrel(int relkind);
extern int get_relkind_by_reloid(Oid reloid);
extern Oid get_relfilenodeby_reloid(Oid reloid);
extern bool get_typeoutput_fromdic(Oid type, Oid *typOutput, bool *typIsVarlena);
extern bool fetch_relation_pkey(ConstrintInDic *cid);
char* get_typname_by_typoid(Oid type);

extern int add_wal(char *path);
extern void free_wallist(void);
extern void free_dictionary(void);

extern void go_through_list(List *list);
extern WalFile* get_last_valid_wal(List *list);
extern int remove_wal_file(char *path);


extern Oid get_reloid_by_relfilenode(RelFileNode *rnode);
extern bool get_relnode_by_relfilenode(Oid relfilenode, RelFileNode *rnode_out);
extern Oid get_relid_by_relnode_via_map(Oid filenode, bool shared);
extern bool get_relname_by_reloid(Oid reloid, NameData* relname);
extern void get_relnode_by_reloid(Oid reloid, RelFileNode* rnode);
extern bool get_nspoid_by_reloid(Oid reloid, Oid *nspoid);
extern bool get_nsp_by_nspoid(Oid nspoid, NameData* nspname);
extern bool get_pkey_list_by_reloid(Oid reloid, int16 **pklist);

extern void flush_page(int index, char* page);
extern bool get_reloid_by_relname(char* relname, Oid* reloid, Oid* relfilenode, bool gettemptable);
extern Oid set_user_relfilenode_map(char *relname, Oid node_in_wal);

#endif