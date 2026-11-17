/*
 * ile_shim.h - OS calls that take or return job-CCSID (EBCDIC) strings.
 *
 * libpq is compiled LOCALETYPE(*LOCALEUTF) TGTCCSID(1208): its strings are
 * UTF-8 and the C runtime handles that, but these system APIs do not
 * (probe p1 on IBM i 7.5: getaddrinfo, getpwuid, open, stat, gai_strerror).
 * Included at the end of port.h, after the system headers.
 */
#ifndef XDB_ILE_SHIM_H
#define XDB_ILE_SHIM_H

#include <stddef.h>
#include <stdlib.h>
#include <fcntl.h>
#include <pwd.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>

struct addrinfo;
struct sockaddr;
struct passwd;
struct stat;

extern int	ile_getaddrinfo(const char *node, const char *service,
							const struct addrinfo *hints, struct addrinfo **res);
extern int	ile_getnameinfo(const struct sockaddr *sa, int salen,
							char *host, int hostlen, char *serv, int servlen,
							int flags);
extern const char *ile_gai_strerror(int ecode);
extern int	ile_getpwuid_r(uid_t uid, struct passwd *pwd, char *buf,
						   size_t buflen, struct passwd **result);
extern int	ile_stat(const char *path, struct stat *st);
extern int	ile_open(const char *path, int oflag,...);
extern char *ile_getenv(const char *name);

/* conversion helpers, also used by the ARD and the RPG wrapper glue */
extern int	ile_conv(int from_ccsid, int to_ccsid, const char *in, size_t inlen,
					 char *out, size_t outsize);
extern int	ile_job_ccsid(void);

#ifndef XDB_ILE_SHIM_IMPL
#define getaddrinfo(a, b, c, d) ile_getaddrinfo(a, b, c, d)
#define getnameinfo(a, b, c, d, e, f, g) ile_getnameinfo(a, b, c, d, e, f, g)
#ifdef gai_strerror
#undef gai_strerror
#endif
#define gai_strerror(e) ile_gai_strerror(e)
#define getpwuid_r(a, b, c, d, e) ile_getpwuid_r(a, b, c, d, e)
#ifdef getenv
#undef getenv
#endif
#define getenv(n) ile_getenv(n)
#endif

/* stat() and open() call sites are renamed in the sources (ile_stat, ile_open) */

#ifdef __ILEC400__
/* things ILE C lacks or declares differently */
#include <stdio.h>
#include <signal.h>
#include <errno.h>
#ifndef XDB_ILE_SHIM_IMPL
#define popen(cmd, mode) ((FILE *) NULL)
#define pclose(f) (0)
#define send(s, b, l, f) (send)((s), (char *) (b), (l), (f))
#endif
#include <math.h>
#include <unistd.h>
#ifndef isnan
#define isnan(x) ((x) != (x))
#endif
#ifndef isinf
#define isinf(x) (!isnan(x) && isnan((x) - (x)))
#endif
#ifndef XDB_ILE_SHIM_IMPL
#define isatty(fd) (0)
#endif
#ifndef NI_MAXHOST
#define NI_MAXHOST 1025
#endif
#ifndef NI_MAXSERV
#define NI_MAXSERV 32
#endif
#ifndef SA_RESTART
#define SA_RESTART 0
#endif
#ifndef ENOEXEC
#define ENOEXEC 99901
#endif
/* no pointer-to-integer tricks with 16-byte pointers */
#undef MemSet
#define MemSet(start, val, len) memset(start, val, len)
#undef MemSetAligned
#define MemSetAligned(start, val, len) memset(start, val, len)
#define XDB_PTR_SEED(p) ((uint64) UINT64CONST(0x9E3779B97F4A7C15))
#else
#define XDB_PTR_SEED(p) ((uintptr_t) (p))
#endif

#endif							/* XDB_ILE_SHIM_H */
