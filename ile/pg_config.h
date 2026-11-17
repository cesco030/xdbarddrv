/*
 * pg_config.h for the IBM i ILE port of libpq (hand-written; replaces the
 * file configure would generate).  XDB_LINUX_EMU selects Linux type sizes
 * so the same feature set can be built and tested on Linux.
 */
#ifndef PG_CONFIG_H
#define PG_CONFIG_H

#ifdef __ILEC400__
/* makes <string.h> declare strdup() */
#ifndef __cplusplus__strings__
#define __cplusplus__strings__ 1
#endif
#endif

#define BLCKSZ 8192
#define XLOG_BLCKSZ 8192
#define RELSEG_SIZE 131072
#define DEF_PGPORT 5432
#define DEF_PGPORT_STR "5432"
#define DLSUFFIX ".so"
#define MEMSET_LOOP_LIMIT 1024
#define PACKAGE_BUGREPORT "pgsql-bugs@lists.postgresql.org"
#define PACKAGE_NAME "PostgreSQL"
#define PACKAGE_STRING "PostgreSQL 18.7"
#define PACKAGE_TARNAME "postgresql"
#define PACKAGE_URL "https://www.postgresql.org/"
#define PACKAGE_VERSION "18.7"
#define PG_KRB_SRVNAM "postgres"
#define PG_MAJORVERSION "18"
#define PG_MAJORVERSION_NUM 18
#define PG_MINORVERSION_NUM 7
#define PG_VERSION "18.7"
#define PG_VERSION_NUM 180007
#define PG_VERSION_STR "PostgreSQL 18.7 libpq, IBM i ILE port"
#define CONFIGURE_ARGS "ile-port"

/* headers and functions present on both ILE and Linux */
#define HAVE_INET_PTON 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define HAVE_SOCKLEN_T 1
#define HAVE_PTHREAD 1
#define STDC_HEADERS 1

/* declarations ILE lacks: PostgreSQL's own replacements are compiled in */
#define HAVE_DECL_FDATASYNC 0
#define HAVE_DECL_F_FULLFSYNC 0
#define HAVE_DECL_MEMSET_S 0
#define HAVE_DECL_POSIX_FADVISE 0
#define HAVE_DECL_PREADV 0
#define HAVE_DECL_PWRITEV 0
#define HAVE_DECL_STRCHRNUL 0
#define HAVE_DECL_TIMINGSAFE_BCMP 0
#ifdef XDB_LINUX_EMU
/* glibc declares these; keep its declarations, still build the replacements */
#define HAVE_DECL_STRLCAT 1
#define HAVE_DECL_STRLCPY 1
#define HAVE_DECL_STRNLEN 1
#define HAVE_DECL_STRSEP 1
#define HAVE_STRLCAT 1
#define HAVE_STRLCPY 1
#define HAVE_STRNLEN 1
#define HAVE_STRSEP 1
#define HAVE_EXPLICIT_BZERO 1
#define HAVE_INET_ATON 1
#define HAVE_STRERROR_R 1
#else
#define HAVE_DECL_STRLCAT 0
#define HAVE_DECL_STRLCPY 0
#define HAVE_DECL_STRNLEN 0
#define HAVE_DECL_STRSEP 0
#endif

#define pg_restrict

#ifdef XDB_LINUX_EMU
#define PG_C_PRINTF_ATTRIBUTE gnu_printf
#define ALIGNOF_DOUBLE 8
#define ALIGNOF_INT 4
#define ALIGNOF_INT64_T 8
#define ALIGNOF_LONG 8
#define ALIGNOF_SHORT 2
#define MAXIMUM_ALIGNOF 8
#define SIZEOF_LONG 8
#define SIZEOF_LONG_LONG 8
#define SIZEOF_OFF_T 8
#define SIZEOF_SIZE_T 8
#define SIZEOF_VOID_P 8
#else
/* ILE C, default data model: 16-byte pointers, 4-byte long and size_t */
/* IBM i is big-endian: without this pg_hton32() swaps and the startup packet is garbage */
#define WORDS_BIGENDIAN 1
#define ALIGNOF_DOUBLE 8
#define ALIGNOF_INT 4
#define ALIGNOF_INT64_T 8
#define ALIGNOF_LONG 4
#define ALIGNOF_SHORT 2
#define MAXIMUM_ALIGNOF 16
#define SIZEOF_LONG 4
#define SIZEOF_LONG_LONG 8
#define SIZEOF_OFF_T 8
#define SIZEOF_SIZE_T 4
#define SIZEOF_VOID_P 16
#endif

#endif							/* PG_CONFIG_H */
