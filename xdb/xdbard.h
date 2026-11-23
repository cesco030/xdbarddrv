/* xdbard.h - entry points of the XDB application requester driver */
#ifndef XDBARD_H
#define XDBARD_H

/* the eight parameters of the SQL Client Integration exit program */
extern void xdb_ard_entry(int *level, char *in, int *inlen, char *fmtname,
						  char *sqlca, int *ccsid, char *out, int *outlen);

/* commitment control exit program parameters (QTNADDCR resource) */
extern void xdb_commit_exit(char *info, char *status, char *ret);
extern void xdb_fail_sqlca(char *sqlca, const char *msgid);

/*
 * The commitment resource of a connection is added by the program stub,
 * because that has to be done from the caller's activation group.  The kinds
 * of resource, from the one that protects a unit of work best:
 *   XDB_CMT_LAST   the last one asked: it commits on the server while the
 *                  rest of the unit of work can still be rolled back, so a
 *                  commit that fails there fails everywhere (one per
 *                  commitment definition);
 *   XDB_CMT_ASKED  asked whether it can commit before anything is committed,
 *                  and told the outcome afterwards;
 *   XDB_CMT_TOLD   only told the outcome (what the driver did before these
 *                  kinds existed): a commit that fails on the server cannot
 *                  stop the commit of the rest.
 * xdb_commit_wanted() returns 0, or the number of the resource this
 * connection needs now, with the best kind to try in *kind; the stub tries
 * that kind and then the lesser ones, reports every refusal with
 * xdb_commit_refused() and the end with xdb_commit_registered()
 * (XDB_CMT_NONE when the system took none).
 */
#define XDB_CMT_NONE 0
#define XDB_CMT_TOLD 1
#define XDB_CMT_ASKED 2
#define XDB_CMT_LAST 3
extern int	xdb_commit_wanted(const char *in, char *lib10, int *kind);
extern void xdb_commit_refused(const char *in, int kind, const char *msgid, const char *data);
extern void xdb_commit_registered(const char *in, int kind, int handle);

/*
 * Before a request is passed on: when it ends the connection outside a
 * commit or rollback, the handle of its commitment resource, which the stub
 * removes after the request (and reports with xdb_commit_removed()); else 0.
 */
extern int	xdb_commit_handle(const char *in, const char *fmtname);
extern void xdb_commit_removed(int handle, int ok, const char *msgid);

#endif
