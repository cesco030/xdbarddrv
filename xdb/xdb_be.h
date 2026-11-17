/*
 * xdb_be.h - backend interface of the XDB driver.
 *
 * The ARD core (xdbard.c) knows nothing about a particular database: it
 * rewrites parameter markers, converts host variables to UTF-8 text and
 * result values from UTF-8 text to IBM i formats.  A backend implements
 * this table of functions.  All strings crossing the interface are UTF-8.
 * Functions returning int give 0 for success and -1 for failure (with
 * xbe_err filled in).
 */
#ifndef XDB_BE_H
#define XDB_BE_H

/* column kinds reported by describe() */
enum
{
	XK_SMALLINT = 1, XK_INTEGER, XK_BIGINT, XK_DOUBLE, XK_DECIMAL,
	XK_DATE, XK_TIME, XK_TIMESTAMP, XK_CHAR, XK_VARCHAR, XK_BOOL
};

typedef struct
{
	char		name[64];
	int			kind;
	int			len;			/* characters for CHAR/VARCHAR, 0 = unbounded */
	int			prec;			/* DECIMAL precision, 0 = unbounded */
	int			scale;
} xbe_col;

typedef struct
{
	char		sqlstate[6];	/* standard SQLSTATE where one applies */
	int			native;			/* server error number */
	int			rolled_back;	/* the server rolled the whole transaction back;
								 * the backend has started a new one.  After
								 * txn(XT_COMMIT): the server has answered with
								 * an error, so it has not committed for sure */
	char		msg[512];
} xbe_err;

/* a batch of fetched rows: val[row * ncols + col], NULL pointer = SQL NULL */
typedef struct xbe_batch
{
	int			nrows;
	int			ncols;
	int			eof;			/* set by the backend: nothing follows this batch */
	char	  **val;
	void	   *owner;
	void		(*release) (struct xbe_batch *b);
} xbe_batch;

typedef struct
{
	const char *rdb;			/* section name in xdb.conf */
	const char *conninfo;		/* backend specific connection string */
	const char *user;			/* may be empty */
	const char *password;		/* may be empty */
	/* reads other keys of the section; returns 0 when found */
	int			(*conf) (const char *rdb, const char *key, char *out, int outsize);
} xbe_opts;

enum
{
	XT_BEGIN = 1, XT_COMMIT, XT_ROLLBACK, XT_SAVEPOINT, XT_ROLLBACK_TO
};

/*
 * Large values that travel between a stream file and the server, beside
 * Db2 (which passes at most 32K per value through the exit program): the
 * statement functions GET_BLOB_FROM_FILE, GET_CLOB_FROM_FILE,
 * PUT_BLOB_TO_FILE and PUT_CLOB_TO_FILE.  The core (xdblob.c) finds them in
 * the statement text and does the file work; a backend that can move such
 * values supplies the table below (xbe_driver.lob), with the SQL to put in
 * their place and the transfer itself.  Text is UTF-8 on this interface.
 */
enum
{
	XLOB_BLOB = 1, XLOB_CLOB
};

/* the file a value is read from */
typedef struct xlob_src
{
	void	   *ctx;
	/* up to len bytes; 0 = end of the file, -1 = error (e filled in) */
	int			(*read) (void *ctx, char *buf, int len, xbe_err *e);
	/* bytes read() delivers in all; -1 = only reading tells (text is converted on the way) */
	long long	(*size) (void *ctx);
	/* back to the first byte, to read the file once more */
	int			(*rewind) (void *ctx, xbe_err *e);
} xlob_src;

/* the file a value is written to; the backend names it (it comes with the row) */
typedef struct xlob_dst
{
	void	   *ctx;
	int			(*open) (void *ctx, const char *path, xbe_err *e);
	int			(*write) (void *ctx, const char *buf, int len, xbe_err *e);
	/* the value has been written to its end (a backend with out_to says so) */
	void		(*done) (void *ctx);
} xlob_dst;

#define XLOB_SLOTS 4			/* most parameter markers one file value may take */
#define XLOB_SQLIN 400			/* room for the text of in_sql */
#define XLOB_SQLPRE 1500		/* room for pre of out_sql */
#define XLOB_SQLMID 300			/* room for mid and for post */

typedef struct xbe_lob
{
	/*
	 * SQL that stands for the value of a file in a statement, with a ? for
	 * each parameter the backend wants for it; returns their number.
	 */
	int			(*in_sql) (void *c, int kind, char *out, int outsize);

	/*
	 * SQL around the two arguments of PUT_xLOB_TO_FILE(value, file name).
	 * Returns 0 for the order  pre value mid name post,  1 for  pre name
	 * mid value post,  -1 when the server cannot do it (e says why).  The
	 * result column carries whatever the backend needs to deliver the
	 * value.  stage = 0: nothing may be left on the server for a later
	 * request (no transaction is open).
	 */
	int			(*out_sql) (void *c, int kind, int stage, char *pre, char *mid, char *post,
							xbe_err *e);

	/*
	 * Result columns (from 0) of the statement described next that carry
	 * such values: they must come whole.
	 */
	void		(*sinks) (void *c, const int *cols, int n);

	/*
	 * One file for the statement that runs next: fills the values of the
	 * markers of in_sql (malloc'd text, NULL = SQL NULL).  A backend either
	 * moves the file to the server now, or keeps src and sends the file as
	 * a parameter when the statement is executed: src stays readable until
	 * in_done(), and the backend knows the parameter again by the address
	 * of the slot value it returned here.
	 */
	int			(*in) (void *c, int kind, xlob_src *src, char **slot, xbe_err *e);
	/* after that statement: ok = it ran; frees what in() left on the server */
	void		(*in_done) (void *c, int ok);

	/*
	 * Move the value in column value v of a fetched row to dst.  NULL in a
	 * backend that has out_to.
	 */
	int			(*out) (void *c, int kind, const char *v, xlob_dst *dst, xbe_err *e);

	/*
	 * For a backend that receives a row in one piece and so can deliver
	 * such a value only while it reads the row (NULL elsewhere).  Before
	 * each fetch of a statement with file columns the core names the
	 * destinations, one for each column given to sinks(), in that order;
	 * the fetch then brings one row, opens and writes the destinations and
	 * leaves in the batch, for a value it delivered, 24 bytes that it
	 * allocated (the core writes the size of the file there; NULL = null
	 * value, no file).
	 * A statement whose rows the server sends all at once cannot be served
	 * this way: open() refuses it.  Called with n = 0 after the fetch.
	 */
	void		(*out_to) (void *c, xlob_dst *dst, int n);
} xbe_lob;

/* receives one fact of xbe_driver.info */
typedef void (*xbe_info_add) (void *ctx, const char *area, const char *name, const char *value);

typedef struct xbe_driver
{
	const char *name;			/* "pg", "mysql", ... (backend = in xdb.conf) */
	const char *marker;			/* printf format of parameter n: "$%d", "?", "@P%d" */
	const char *product;		/* 3 character product id reported to Db2 */
	int			needs_savepoints;	/* a failed statement aborts the transaction */

	void	   *(*connect) (const xbe_opts *o, xbe_err *e);
	void		(*disconnect) (void *c);
	int			(*version) (void *c);	/* major * 100 + minor */

	/*
	 * Parameters: pv[i] is the value as UTF-8 text (NULL = SQL NULL), pk[i]
	 * the kind (XK_*) of the host variable it came from; pk may be NULL.
	 */
	/* run a statement that returns no rows; stmt (from describe) may be NULL */
	int			(*exec) (void *c, void *stmt, const char *sql, int np, char **pv,
						 const int *pk, long long *rows, xbe_err *e);
	/* parse sql and describe its result columns (*ncols = 0: not a query) */
	int			(*describe) (void *c, const char *sql, xbe_col **cols, int *ncols,
							 void **stmt, xbe_err *e);
	void		(*stmt_free) (void *c, void *stmt);

	/* open a forward-only cursor; hold = must survive a commit */
	int			(*open) (void *c, void *stmt, const char *sql, int np, char **pv,
						 const int *pk, int hold, void **cur, xbe_err *e);
	/* fetch up to maxrows; b->nrows = 0 means end of data */
	int			(*fetch) (void *c, void *cur, int maxrows, xbe_batch *b, xbe_err *e);
	void		(*close) (void *c, void *cur);

	int			(*txn) (void *c, int op, xbe_err *e);

	/*
	 * Is the server still there?  Called between statements on a connection
	 * that has been idle; waits at most timeout seconds.  0 = alive, -1 =
	 * the connection is unusable (the core then opens a new one with
	 * connect() and disconnects this one).  May be NULL.
	 */
	int			(*ping) (void *c, int timeout);

	/* values moved between stream files and the server; NULL = not supported */
	const xbe_lob *lob;

	/*
	 * What the backend knows about its server and its connection, for the
	 * statement XDB CONNECTION INFO: it calls add() once for each fact.
	 * area is "link" (the connection to the server), "server" (what the
	 * server said about itself) or "options" (settings only this backend
	 * has); name and value are UTF-8 text.  Only what is known already:
	 * nothing is asked of the server.  A backend that opens a lost
	 * connection again by itself also reports, in area "count", how often
	 * ("reconnects"), how often with a transaction open
	 * ("units_of_work_lost") and when last ("last_reconnect", seconds of
	 * time()); the core adds them to its own counters.  May be NULL.
	 */
	void		(*info) (void *c, xbe_info_add add, void *ctx);
} xbe_driver;

extern const xbe_driver xbe_pg;
extern const xbe_driver xbe_mysql;
extern const xbe_driver xbe_mssql;

/* ---- helpers shared by the backends that speak a wire protocol (xdbnet.c) */

/* value of key in a "key=value key=value" string, NULL when absent */
extern const char *xnet_kv(const char *info, const char *key, char *out, int outsize);
/* TCP connect with timeout (seconds, 0 = system default); socket or -1 */
extern int	xnet_connect(const char *host, const char *port, int timeout, xbe_err *e);

/* 1 = data can be read within timeout seconds, 0 = nothing came, -1 = error */
extern int	xnet_wait_readable(int fd, int timeout);

/*
 * Look at an idle socket without waiting: 1 = the peer has closed or reset
 * it.  With unsolicited = 1 bytes waiting on it count as closed too (a
 * server that speaks only when asked has just said goodbye); a TLS
 * connection may hold records of its own, so there pass 0.
 */
extern int	xnet_dead(int fd, int unsolicited);
/*
 * Password lookup in a file of host:port:database:user:password lines
 * (the PostgreSQL .pgpass format: * matches anything, \: and \\ escape).
 * Returns 0 when found.
 */
extern int	xnet_passfile(const char *path, const char *host, const char *port,
						  const char *db, const char *user, char *out, int outsize);
extern void xnet_err(xbe_err *e, const char *sqlstate, const char *fmt,...);

/* TLS on an already connected socket (OpenSSL on Linux, GSKit on IBM i) */
typedef struct xtls xtls;
extern xtls *xtls_start(int fd, const char *host, int verify, char *err, int errsize);

/* transport supplied by the caller: both return bytes moved, <= 0 on failure */
typedef struct
{
	void	   *ctx;
	int			(*rd) (void *ctx, void *buf, int len);
	int			(*wr) (void *ctx, const void *buf, int len);
} xtls_io;

/* max12 = 1 limits the handshake to TLS 1.2; alpn may be NULL */
extern xtls *xtls_start_io(int fd, const xtls_io *io, const char *host, int verify, int max12,
						   const char *alpn, char *err, int errsize);
/* protocol level and cipher of the session as text, "" when they cannot be told */
extern void xtls_info(xtls *t, char *out, int outsize);
extern int	xtls_read(xtls *t, void *buf, int len);
extern int	xtls_write(xtls *t, const void *buf, int len);
extern void xtls_end(xtls *t);

#endif
