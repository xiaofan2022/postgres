# contrib/walminer/Makefile

MODULE_big	= walminer
OBJS		= walminer.o wm_utils.o datadictionary.o fetchcatalogtable.o wallist.o walreader.o \
				walminer_decode.o imagemanage.o wal2sql.o walminer_contents.o walminer_thread.o \
				wal2sql_spi.o wal2sql_ddl.o pagecollect.o wm_compatible.o


EXTENSION = walminer
DATA = walminer--3.0.sql

REGRESS_OPTS = --temp-config=$(top_srcdir)/contrib/walminer/walminer.conf
REGRESS = wal2sql_base wal2sql_subxid wal2sql_lsn wal2sql_time wal2sql_xid wal2sql_missimage wal2sql_front_search \
			wal2sql_toast wal2sql_spill wal2sql_singletable wal2sql_type wal2sql_avatar \
			wal2sql_self_apply wal2sql_ddl pc_base
#REGRESS = wal2sql_ddl
EXTRA_INSTALL= contrib/dblink

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)

ifeq ($(MAJORVERSION), 16)
 PG_CPPFLAGS = -DPG_VERSION_16
else ifeq ($(MAJORVERSION), 15)
 PG_CPPFLAGS = -DPG_VERSION_15
else ifeq ($(MAJORVERSION), 14)
 PG_CPPFLAGS = -DPG_VERSION_14
else ifeq ($(MAJORVERSION), 13)
 PG_CPPFLAGS = -DPG_VERSION_13
else ifeq ($(MAJORVERSION), 12)
 PG_CPPFLAGS = -DPG_VERSION_12
 REGRESS_OPTS = --temp-config=$(top_srcdir)/contrib/walminer/walminer_12.conf
else ifeq ($(MAJORVERSION), 11)
 PG_CPPFLAGS = -DPG_VERSION_11
 REGRESS_OPTS = --temp-config=$(top_srcdir)/contrib/walminer/walminer_12.conf
else ifeq ($(MAJORVERSION), 10)
 PG_CPPFLAGS = -DPG_VERSION_10
 REGRESS_OPTS = --temp-config=$(top_srcdir)/contrib/walminer/walminer_12.conf
else
 $(error unsupport PG version)
endif
include $(PGXS)

else

subdir = contrib/walminer
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

ifeq ($(MAJORVERSION), 16)
 PG_CPPFLAGS = -DPG_VERSION_16
else ifeq ($(MAJORVERSION), 15)
 PG_CPPFLAGS = -DPG_VERSION_15
else ifeq ($(MAJORVERSION), 14)
 PG_CPPFLAGS = -DPG_VERSION_14
else ifeq ($(MAJORVERSION), 13)
 PG_CPPFLAGS = -DPG_VERSION_13
else ifeq ($(MAJORVERSION), 12)
 PG_CPPFLAGS = -DPG_VERSION_12
 REGRESS_OPTS = --temp-config=$(top_srcdir)/contrib/walminer/walminer_12.conf
else ifeq ($(MAJORVERSION), 11)
 PG_CPPFLAGS = -DPG_VERSION_11
 REGRESS_OPTS = --temp-config=$(top_srcdir)/contrib/walminer/walminer_12.conf
else ifeq ($(MAJORVERSION), 10)
 PG_CPPFLAGS = -DPG_VERSION_10
 REGRESS_OPTS = --temp-config=$(top_srcdir)/contrib/walminer/walminer_12.conf
else
 $(error unsupport PG version)
endif

include $(top_srcdir)/contrib/contrib-global.mk
endif
