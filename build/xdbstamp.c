/* XDBSTAMP - sets the fields "created by user" and "system created on" of an object
 * description (Change Object Description API, QLICOBJD, keys 19 and 18).
 *
 * An object keeps the name of the user and of the system that created it, and a save
 * file carries both to every system it is restored on.  dist.sh calls this program for
 * each object of a package, so that a package does not name the place it was built at.
 *
 *   CALL XDBSTAMP PARM('<library>' '<object>' '<type>' '<user>' '<system>')
 *     <type>    *PGM, *SRVPGM, *FILE, *CMD, *PNLGRP, *DTAARA ...
 *     <user>    up to 10 characters, not *IBM
 *     <system>  up to 8 characters
 *   Writes one line when the change fails (message id of the API) and ends with 1.
 *   Needs *OBJMGT to the object (and *OBJOPR for a file) and *EXECUTE to the library;
 *   the object must not be in use.
 *
 *   CRTBNDC PGM(lib/XDBSTAMP) SRCSTMF('.../build/xdbstamp.c') TGTCCSID(*JOB)
 */
#include <stdio.h>
#include <string.h>

#pragma linkage(QLICOBJD, OS)
void QLICOBJD(char *returnedLibrary, char *objectAndLibrary, char *objectType,
              void *changedInformation, void *errorCode);

/* a parameter of a CL CALL is not ended by a null: take it up to the first blank */
static void field(char *dst, const char *src, int n)
{
	int i;

	memset(dst, ' ', n);
	for (i = 0; i < n && src[i] != '\0' && src[i] != ' '; i++)
		dst[i] = src[i];
}

int main(int argc, char **argv)
{
	char returned[10], object[20], type[10];
	struct {                    /* two variable length records, each on a 4 byte boundary */
		int  records;
		int  key1, len1; char system[8];
		int  key2, len2; char user[12];
	} chg;
	struct { int provided, available; char msgid[7]; char rsv; char data[100]; } err;

	if (argc < 6) {
		printf("XDBSTAMP: five parameters: library object type user system\n");
		return 2;
	}
	field(object, argv[2], 10);
	field(object + 10, argv[1], 10);
	field(type, argv[3], 10);
	chg.records = 2;
	chg.key1 = 18; chg.len1 = 8;
	field(chg.system, argv[5], 8);
	chg.key2 = 19; chg.len2 = 10;
	field(chg.user, argv[4], 12);
	memset(chg.user + 10, ' ', 2);
	memset(&err, 0, sizeof err);
	err.provided = sizeof err;
	QLICOBJD(returned, object, type, &chg, &err);
	if (err.available > 0) {
		printf("XDBSTAMP %.10s %.10s %.10s: %.7s\n", object + 10, object, type, err.msgid);
		return 1;
	}
	return 0;
}
