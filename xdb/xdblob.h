/*
 * xdblob.h - file functions of the XDB driver: large values between stream
 * files and the server (see xdblob.c).
 */
#ifndef XDBLOB_H
#define XDBLOB_H

#include "xdb_be.h"

#define XLOB_MAXIN 16			/* GET_xLOB_FROM_FILE in one statement */
#define XLOB_MAXOUT 16			/* PUT_xLOB_TO_FILE in one statement */
#define XLOB_PATHMAX 1024

/* file options, the values of SQL file reference variables (SQFCRT, SQFOVR, SQFAPP) */
#define XLOB_NEW 8
#define XLOB_OVERWRITE 16
#define XLOB_APPEND 32

typedef struct
{
	int			kind;			/* XLOB_BLOB, XLOB_CLOB */
	int			path_user;		/* host variable (from 0) that holds the name; -1 = a literal */
	char	   *path;			/* the literal */
	int			nslots;			/* markers the backend took for the value */
} xlob_in;

typedef struct
{
	int			kind;
	int			col;			/* result column (from 0) */
	int			option;			/* XLOB_NEW, XLOB_OVERWRITE, XLOB_APPEND */
	int			ccsid;			/* CLOB: CCSID of the file */
} xlob_out;

/* a parameter marker of the rewritten statement: a host variable, or part of a file value */
typedef struct
{
	short		user;			/* host variable (from 0), -1 = file value */
	short		file;
	short		slot;
} xlob_mark;

typedef struct xlob_plan
{
	int			nin;
	int			nout;
	xlob_in		in[XLOB_MAXIN];
	xlob_out	out[XLOB_MAXOUT];
	int			nmark;
	xlob_mark  *mark;			/* in the order of the statement text */
	int			nuser;			/* host variables the program passes */
} xlob_plan;

/*
 * Looks for the file functions in a statement (UTF-8, parameter markers
 * still as Db2 wrote them).  0 = none: *out and *plan are NULL.  1 = *out
 * is the statement for the backend (malloc'd), *plan says what to do when
 * it runs.  -1 = the functions are used wrongly, or the backend cannot
 * move files: e says why.
 */
/*
 * The reading of statement text that the core and this module share.
 * xlob_text() says which text is read from now on and how (XTF_* of the
 * backend); xlob_run_end(p) gives the end of the literal, quoted name or
 * comment that begins at p, or p when none begins there.
 */
extern void xlob_text(const char *start, int flags);
extern const char *xlob_run_end(const char *p);

extern int	xlob_parse(const char *sql, const xbe_driver *be, void *bc, int stage,
					   char **out, xlob_plan **plan, xbe_err *e);
extern void xlob_plan_free(xlob_plan *p);

/* a stream file opened for one transfer */
typedef struct
{
	int			fd;
	int			kind;
	int			option;
	int			ccsid;
	int			isopen;
	int			existed;		/* the file was there before */
	long long	before;			/* its size then */
	long long	moved;			/* bytes read so far */
	char		path[XLOB_PATHMAX];
	unsigned char carry[4];		/* UTF-8 character cut by the end of a write */
	int			ncarry;
	int			complete;		/* writing: the backend has delivered the whole value */
	int			u8need;			/* reading text: bytes the character begun still needs */
	unsigned char u8lo,
				u8hi;			/* ... and what the next of them may be */
} xlob_file;

/* open a file for reading; src then reads it (text: as UTF-8) */
extern int	xlob_src_open(xlob_file *f, const char *path, int kind, xlob_src *src, xbe_err *e);
extern void xlob_src_close(xlob_file *f);

/* prepare a file for writing; it is opened when the backend calls dst->open */
extern void xlob_dst_init(xlob_file *f, int kind, int option, int ccsid, xlob_dst *dst);
/*
 * ok = 1: close, *bytes = bytes the file grew by.  ok = 0: the transfer
 * failed; a new or replaced file is removed, an extended one cut back.
 */
extern int	xlob_dst_close(xlob_file *f, int ok, long long *bytes, xbe_err *e);

#endif
