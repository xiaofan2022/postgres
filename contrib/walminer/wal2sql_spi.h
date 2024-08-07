/*-------------------------------------------------------------------------
 *
 * IDENTIFICATION
 *	  wal2sql_spi.h
 *
 * 
 *-------------------------------------------------------------------------
 */
#ifndef WALMINER_SPI_H
#define WALMINER_SPI_H

#include "utils/resowner.h"

void walminer_spi_connect(void);
void walminer_spi_finish(void);
bool walminer_spi_execute(char *command, int kind);
bool walminer_spi_tx(int kind, MemoryContext oldcontext, ResourceOwner oldowner);
void walminer_record_in_temp(char *record);
void walminer_handle_failure_transaction(bool commited);

#endif