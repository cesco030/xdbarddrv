/*
 * ile_chklocale.c - replaces src/port/chklocale.c.
 * ILE has no locale_t/newlocale; the client encoding is always UTF8 here
 * (it is only consulted for client_encoding=auto).
 */
#include "c.h"
#include "mb/pg_wchar.h"

int
pg_get_encoding_from_locale(const char *ctype, bool write_message)
{
	return PG_UTF8;
}
