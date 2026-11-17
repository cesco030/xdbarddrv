/* pg_config_os.h for IBM i ILE (and its Linux emulation build). */
#ifndef XDB_PG_CONFIG_OS_H
#define XDB_PG_CONFIG_OS_H
#ifdef XDB_LINUX_EMU
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#endif
#endif
