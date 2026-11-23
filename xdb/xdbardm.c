/* XDBARD *PGM - ARD exit program stub: forwards to service program XDB. */
#include <string.h>
#include <except.h>
#include "xdbard.h"

#pragma linkage(QTNADDCR, OS)
void		QTNADDCR(int *, char *, char *, char *, char *, void *, void *);
#pragma linkage(QTNRMVCR, OS)
void		QTNRMVCR(int *, void *);

typedef struct
{
	int			provided;
	int			avail;
	char		id[7];
	char		rsv;
	char		data[64];
}			errcode;

/*
 * Adds one commitment resource of the given kind, whose exit program is
 * XDBCMT.  1 = added, 0 = refused (ec says why).
 */
static int
add_resource(char *in, char *lib, int gen, int kind, int *handle, errcode *ec)
{
	_Packed struct
	{
		int			len;
		char		jrn[10];
		char		jrnlib[10];
		char		protocol;
		char		classify;
		char		prepare;
		char		rollback;
		char		ipl;
		char		lastagent;
		char		normalsave;
	}			opt;
	char		pgm[20];
	char		info[80];
	char		name[10];

	/* the name the system uses for the resource in its messages: XDB and the start of the RDB name */
	memcpy(name, "XDB", 3);
	memcpy(name + 3, in + 8, 7);
	memset(&opt, 0, sizeof opt);
	opt.len = 31;
	memcpy(opt.jrn, "*NONE     ", 10);
	memset(opt.jrnlib, ' ', 10);
	/*
	 * one-phase: called once, with the outcome.  two-phase: either the last
	 * agent, called once to commit before the others are committed, or
	 * called to prepare and then with the outcome.
	 */
	opt.protocol = kind == XDB_CMT_TOLD ? '1' : '2';
	opt.classify = 'N';
	opt.prepare = kind == XDB_CMT_ASKED ? 'Y' : 'N';
	opt.rollback = 'N';
	opt.ipl = 'N';
	opt.lastagent = kind == XDB_CMT_LAST ? 'Y' : 'N';
	opt.normalsave = 'Y';
	memcpy(pgm, "XDBCMT    ", 10);
	memcpy(pgm + 10, lib, 10);
	memset(info, 0, sizeof info);
	memcpy(info, in + 4, 22);	/* activation group number + RDB name */
	memcpy(info + 22, &gen, 4);	/* which connection of that name */
	memset(ec, 0, sizeof *ec);
	ec->provided = sizeof *ec;
	*handle = 0;
	QTNADDCR(handle, name, pgm, info, "N", ec, &opt);
	return ec->avail == 0;
}

/* the best kind of resource the system takes, from the one asked for downwards */
static void
add_commit_resource(char *in, char *lib, int gen, int kind)
{
	errcode		ec;
	int			handle;

	for (; kind >= XDB_CMT_TOLD; kind--)
	{
		if (add_resource(in, lib, gen, kind, &handle, &ec))
		{
			xdb_commit_registered(in, kind, handle);
			return;
		}
		xdb_commit_refused(in, kind, ec.id, ec.data);
	}
	xdb_commit_registered(in, XDB_CMT_NONE, 0);
}

int
main(int argc, char *argv[])
{
	char		lib[10];
	int			gen,
				kind,
				gone;
	volatile _INTRPT_Hndlr_Parms_T ex;

	if (argc < 9)
		return 1;
	/*
	 * An exception that escapes the driver ends the request, not the job.
	 * Its message stays in the job log, with the module, the procedure and
	 * the statement it came from: that is what tells where the driver went
	 * wrong.
	 */
#pragma exception_handler(fail, ex, 0, _C2_ALL, _CTLA_HANDLE)
	gone = xdb_commit_handle(argv[2], argv[4]);
	xdb_ard_entry((int *) argv[1], argv[2], (int *) argv[3], argv[4],
				  argv[5], (int *) argv[6], argv[7], (int *) argv[8]);
	if (gone)
	{
		/* the connection has ended: its resource has no part in later commits */
		errcode		ec;

		memset(&ec, 0, sizeof ec);
		ec.provided = sizeof ec;
		QTNRMVCR(&gone, &ec);
		xdb_commit_removed(gone, ec.avail == 0, ec.id);
	}
	gen = xdb_commit_wanted(argv[2], lib, &kind);
	if (gen)
		add_commit_resource(argv[2], lib, gen, kind);
#pragma disable_handler
	return 0;
fail:
	*(int *) argv[8] = 0;
	xdb_fail_sqlca(argv[5], (const char *) ex.Msg_Id);
	return 0;
}
