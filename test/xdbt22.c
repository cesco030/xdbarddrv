/*
 * XDBT22 - test of the "allowed users" check of XDB (module XDBPRFC).
 *
 *   CALL XDBT22 PARM('<output file>' ['<user profile>' ...])
 *
 * Writes the names the job counts as (user, group profile, supplemental
 * groups) and the answer of xdbprf_user_in() for a set of lists.  For every
 * user profile given it also writes the names read for that profile, so that
 * the reading of group and supplemental groups can be compared with what the
 * system shows (DSPUSRPRF, QSYS2.USER_INFO).  Nothing is changed.
 *
 * CRTCMOD ... TGTCCSID(*JOB) SYSIFCOPT(*IFSIO), CRTPGM with module XDBPRFC.
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "xdbprf.h"

static FILE *f;

static void
ask(const char *label, const char *list, int expected)
{
	char		who[11];
	int			r = xdbprf_user_in(list, who);

	who[10] = '\0';
	fprintf(f, "%-34s list [%s] -> %s%s\n", label, list, r ? "allowed" : "refused",
			expected < 0 ? "" : r == expected ? "   ok" : "   WRONG");
}

int
main(int argc, char *argv[])
{
	char		me[300];
	char		user[16],
				group[16],
				lower[16],
				list[200];
	int			i;

	if (argc < 2 || (f = fopen(argv[1], "w")) == NULL)
		return 1;
	xdbprf_whoami(me, sizeof me);
	fprintf(f, "the job counts as: %s\n", me);
	user[0] = group[0] = '\0';
	sscanf(me, "%10s %10s", user, group);
	for (i = 0; user[i]; i++)
		lower[i] = (char) tolower((unsigned char) user[i]);
	lower[i] = '\0';

	ask("no list", "", 1);
	ask("blanks only", "    ", 1);
	ask("*ALL", "*ALL", 1);
	ask("*all in lower case", "*all", 1);
	ask("the user of the job", user, 1);
	ask("the user in lower case", lower, 1);
	ask("another name", "XDBNOSUCH1", 0);
	ask("two other names", "XDBNOSUCH1 XDBNOSUCH2", 0);
	snprintf(list, sizeof list, "XDBNOSUCH1  %s", user);
	ask("another name, then the user", list, 1);
	snprintf(list, sizeof list, "XDBNOSUCH1 *ALL");
	ask("another name, then *ALL", list, 1);
	snprintf(list, sizeof list, "%sX", user);
	ask("the user with a letter more", list, 0);
	if (strlen(user) > 1)
	{
		snprintf(list, sizeof list, "%.*s", (int) strlen(user) - 1, user);
		ask("the user with a letter less", list, 0);
	}
	ask("a name longer than 10", "XDBNOSUCHNAME1", 0);
	if (group[0])
	{
		ask("the group profile of the user", group, 1);
		snprintf(list, sizeof list, "XDBNOSUCH1 XDBNOSUCH2 %s", group);
		ask("two others, then the group", list, 1);
	}
	else
		fprintf(f, "the user has no group profile: the group cases are not run\n");

	for (i = 2; i < argc; i++)
	{
		char		p[10];
		int			k;

		memset(p, ' ', 10);
		for (k = 0; k < 10 && argv[i][k] != '\0' && argv[i][k] != ' '; k++)
			p[k] = argv[i][k];
		xdbprf_names(p, me, sizeof me);
		fprintf(f, "names read for profile %d: %s\n", i - 1, me[0] ? me : "(no answer)");
	}
	fclose(f);
	return 0;
}
