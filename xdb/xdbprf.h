/*
 * xdbprf.h - XDB connection profiles kept in physical files.
 *
 *   XDBPRF   one record per relational database (RDB) name
 *   XDBPRO   driver specific options of a profile
 *   XDBVLDL  validation list holding the passwords (encrypted by the system)
 *
 * The files are in the library of the program that calls these functions.
 * All text is in the job CCSID, blank padded, exactly as in the records.
 */
#ifndef XDBPRF_H
#define XDBPRF_H

/* record of XDBPRF: keep in step with the DDS (character and zoned fields only) */
typedef struct
{
	char		rdb[18];		/* PRRDB    RDB name, the key */
	char		type[10];		/* PRTYPE   PG, MYSQL, MSSQL */
	char		sts;			/* PRSTS    A active, D disabled */
	char		text[50];		/* PRTEXT */
	char		host[120];		/* PRHOST */
	char		port[5];		/* PRPORT   0 = default of the database type */
	char		dbnm[60];		/* PRDBNM */
	char		user[60];		/* PRUSER */
	char		pwds;			/* PRPWDS   Y = a password is in XDBVLDL */
	char		tls[10];		/* PRTLS    *OFF *PREFER *REQUIRE *VERIFY */
	char		tmo[3];			/* PRTMO    connect timeout, seconds */
	char		ccsid[5];		/* PRCCSID  CCSID of character columns, 0 = job */
	char		txtl[5];		/* PRTXTL   length of unbounded text columns */
	char		nscl[2];		/* PRNSCL   scale of unconstrained numerics */
	char		trc;			/* PRTRC    Y = trace to /tmp/xdb_<rdb>.log */
	char		crtu[10];
	char		crtt[26];
	char		chgu[10];
	char		chgt[26];
} xdbprf_rec;

/* record of XDBPRO */
typedef struct
{
	char		rdb[18];		/* PORDB */
	char		key[20];		/* POKEY    keyword */
	char		kind;			/* POKIND   C = connection keyword, D = driver setting */
	char		val[200];		/* POVAL */
	char		chgu[10];
	char		chgt[26];
} xdbpro_rec;

/* library of the calling program, 10 characters blank padded */
extern void xdbprf_mylib(char *lib10);

/* 0 = found, 1 = no such profile, -1 = file not there or not readable */
extern int	xdbprf_read(const char *lib10, const char *rdb18, xdbprf_rec *out);

/* options of a profile; returns how many were stored in out (at most max) */
extern int	xdbprf_options(const char *lib10, const char *rdb18, xdbpro_rec *out, int max);

/* password in the validation list; 0 = done, otherwise errno */
extern int	xdbprf_setpwd(const char *lib10, const char *rdb18, const char *pwd, int len);
extern int	xdbprf_delpwd(const char *lib10, const char *rdb18);

/*
 * 1 = the user the job runs under now, or its group profile, or one of its
 * supplemental groups is named in list: profile names separated by blanks
 * (job CCSID, any case), or *ALL.  An empty list allows everybody.
 * who10 gets the user of the job, blank padded.
 */
extern int	xdbprf_user_in(const char *list, char *who10);

/*
 * "USER GROUP SUPPLEMENTAL..." in the job CCSID: the names the list of
 * allowed users is compared with.  Returns the length.  xdbprf_names does
 * the same for another user profile (name of 10, blank padded); both are
 * for the tests and are not exported by the service program.
 */
extern int	xdbprf_whoami(char *out, int outsize);
extern int	xdbprf_names(const char *prof10, char *out, int outsize);

/*
 * EXPERIMENT (large objects, see lobt_load in xdbard.c; only with
 * DEFINE(XDB_LOBTEST)): the content of data area QTEMP/XDBLOBT of the job
 * in the job CCSID, 0 = there is none.
 */
#ifdef XDB_LOBTEST
extern int	xdbprf_testspec(char *out, int outsize);
#endif

/*
 * Content of data area XDBVER in the library of the calling program (the
 * version a package was built as), job CCSID, without trailing blanks;
 * returns its length, 0 = there is none.
 */
extern int	xdbprf_version(char *out, int outsize);

/* returns the length and the CCSID the password was stored in, -1 = none */
extern int	xdbprf_getpwd(const char *lib10, const char *rdb18, char *out, int outsize, int *ccsid);

#endif
