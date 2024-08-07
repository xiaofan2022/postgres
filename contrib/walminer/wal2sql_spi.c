/*-------------------------------------------------------------------------
 *
 * IDENTIFICATION
 *	  wal2sql_spi.c
 *
 * 
 *-------------------------------------------------------------------------
 */
#include "walminer_decode.h"
#include "wm_utils.h"
#include "executor/spi.h"
#include "utils/memutils.h"
#include "wal2sql_spi.h"


void
walminer_spi_connect(void)
{
	int	ret = 0;
	ret = SPI_connect();
	if(SPI_ERROR_CONNECT == ret)
	{
		elog(ERROR, "Can not connect via SPI");
	}
	walminer_debug("[walminer_spi_connect]");
}

void
walminer_spi_finish(void)
{
	int	ret = 0;
	ret = SPI_finish();
	if(SPI_OK_FINISH != ret)
	{
		elog(ERROR, "Can not finish connect");
	}
	walminer_debug("[walminer_spi_finish]");
}

/*
 * DECODE_SQL_KIND_BEGIN
 * DECODE_SQL_KIND_COMMIT
 * DECODE_SQL_KIND_ABORT
 */
bool
walminer_spi_tx(int kind, MemoryContext	oldcontext, ResourceOwner oldowner)
{
	bool			result = true;

	walminer_debug("[walminer_spi_tx]%d", kind);

	if(DECODE_SQL_KIND_BEGIN == kind)
	{
		//elog(WARNING, "BEGIN;");
		BeginInternalSubTransaction(NULL);
		MemoryContextSwitchTo(oldcontext);
		//CurrentResourceOwner = oldowner;
	}
	else if(DECODE_SQL_KIND_COMMIT == kind)
	{
		//elog(WARNING, "COMMIT;");
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
	}
	else if(DECODE_SQL_KIND_ABORT == kind)
	{
		//elog(WARNING, "ABORT;");
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
	}
	walminer_debug("[walminer_spi_tx]result=%d", result);

	return result;
}


/*
 * kind:
 * 1->DECODE_SQL_KIND_INSERT
 * 2->DECODE_SQL_KIND_UPDATE
 * 3->DECODE_SQL_KIND_DELETE
 */
bool
walminer_spi_execute(char *command, int kind)
{
	bool			result = true;

	PG_TRY();
	{
		SPI_execute(command, false, 0);
	}
	PG_CATCH();
	{
		result = false;
	}
	PG_END_TRY();
	return result;
}

void
walminer_init_failure_file(void)
{
	char	failure_path[MAXPGPATH] = {0};

	get_failure_pach(failure_path);

	wdecoder.failurefp = fopen(failure_path, "w+");
	if(!wdecoder.failurefp)
	{
		elog(ERROR, "Can not open file %s to write", failure_path);
	}
	walminer_debug("[walminer_init_failure_file]");
}

void
walminer_init_failure_temp_file(void)
{
	char	failure_path_temp[MAXPGPATH] = {0};

	get_failure_temp_pach(failure_path_temp);

	wdecoder.failurefp_temp = fopen(failure_path_temp, "w+");
	if(!wdecoder.failurefp_temp)
	{
		elog(ERROR, "Can not open file %s to write", failure_path_temp);
	}
	walminer_debug("[walminer_init_failure_temp_file]");
}


/*
 * 将任何SQL先暂存入临时文件，在事务失败时，从此处拷贝失败信息到失败文件
 */
void
walminer_record_in_temp(char *record)
{
	fputs(record, wdecoder.failurefp_temp);
	fputc('\n', wdecoder.failurefp_temp);
	walminer_debug("[walminer_record_in_temp]%s", record);
}

/*
 * 在一个事务完成之后，需要根据事务提交的状态，处理临时输出的SQL语句
 * 如果事务提交，那么清空临时文件，如果事务失败，那么将临时文件中的内
 * 容转移到失败SQL文件中。
 */
void
walminer_handle_failure_transaction(bool commited)
{
	if(!commited)
	{
		int		readbyte = 0;
		char 	buf[1024] = {0};
		fseek(wdecoder.failurefp_temp, 0, SEEK_SET);

		while(0 != (readbyte = fread(buf, 1, 1024, wdecoder.failurefp_temp)))
		{
			fwrite(buf, 1, readbyte, wdecoder.failurefp);
		}
	}
	fflush(wdecoder.failurefp);
	fclose(wdecoder.failurefp_temp);
	walminer_init_failure_temp_file();
	walminer_debug("[walminer_handle_failure_transaction]%d", commited);
}