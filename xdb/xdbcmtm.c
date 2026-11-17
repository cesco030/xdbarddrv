/* XDBCMT *PGM - commitment control exit program stub for the XDB resource. */
#include "xdbard.h"

int
main(int argc, char *argv[])
{
	if (argc < 4)
		return 1;
	xdb_commit_exit(argv[1], argv[2], argv[3]);
	return 0;
}
