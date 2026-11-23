#!/usr/bin/env python3
# rowlen.py - the lengths of character columns under a profile with CCSID 1208
# (UTF-8), and the row Db2 is told.
#
# usage: python3 rowlen.py <emu dir> pg|mysql|mssql [-v]
#   the server: see xdbqa.py (XDB_PG_PORT ..., XDB_MY_PORT ..., XDB_MS_PORT ...).
#
# With such a profile a column gets the room in bytes its longest value can
# take (four bytes a character from PostgreSQL and MySQL, three a unit from
# SQL Server), so that a value with accented letters is not cut.  Db2 takes
# a row of 32 K at the most from a driver and refuses a query whose columns
# add up to more (SQL0809), so the room is given only where the row keeps
# within that; otherwise every column has its number of characters, as in
# 0.9.9 and before.  The build of 2026-11-22 gave the room without looking
# at the row: a query with two columns of 12000 characters, which had been
# opened every day, was refused.  This test holds both sides: the lengths,
# and that nothing is cut where the room was given.
#
# No table is made: every query is a SELECT of literals.
import sys

import xdbqa

EMU, KIND = sys.argv[1], sys.argv[2]
VERBOSE = '-v' in sys.argv[3:]
T = xdbqa.Target(KIND)
PER = 3 if KIND == 'mssql' else 4       # bytes given for one character (unit) of a column


def col(n, value="'x'"):
    """an expression the server describes as a character column of n characters"""
    if KIND == 'pg':
        return "cast(%s as varchar(%d))" % (value, n)
    if KIND == 'mysql':
        return "cast(%s as char(%d))" % (value, n)
    return "cast(%s as varchar(%d))" % (value, n)


def rep(ch, n):
    if KIND == 'mssql':
        return "replicate(cast(N'%s' as nvarchar(max)), %d)" % (ch, n)
    return "repeat('%s', %d)" % (ch, n)


BIG = 8000                              # 8000 characters: 32000 or 24000 bytes, a column alone still has its room
FIT = (32700 - BIG * PER - 2 * 4) // PER    # what a second column may have for the row to keep within the limit
C = [
    # label, select list, the lengths of its character columns
    ('a narrow row: every column has the room for its longest value',
     "%s as a, %s as b, 7 as n" % (col(50), col(10)), [50 * PER, 10 * PER]),
    ('one long column alone', "%s as a" % col(BIG), [BIG * PER]),
    ('two columns that just keep within the row', "%s as a, %s as b" % (col(BIG), col(FIT)), [BIG * PER, FIT * PER]),
    ('two columns that are a little too long together: the number of characters',
     "%s as a, %s as b" % (col(BIG), col(FIT + 2)), [BIG, FIT + 2]),
    ('the query that was refused: two columns of %d characters and small ones' % (12000 if KIND != 'mssql' else 8000),
     "1 as id, %s as k, %s as t, %s as a, %s as b" % ((col(24), col(765)) + ((col(12000), col(12000)) if KIND != 'mssql'
                                                                                else (col(8000), col(8000)))),
     [24, 765] + ([12000, 12000] if KIND != 'mssql' else [8000, 8000])),
]
lines = ['ISO *NONE', 'C'] + ['Q select ' + c[1] for c in C]
# values: accented text is whole where the room was given
lines.append('Q select %s as a, 7 as n' % col(50, rep('è', 50)))
lines.append('D')
T.profile(ccsid=1208)
out = T.run(EMU, lines, ccsid='280')
Q = [b for b in xdbqa.blocks(out) if b['cmd'].startswith('Q ')]
bad = 0
for c, b in zip(C, Q):
    got = [x[2] for x in b['cols'] if x[1] in (448, 449, 452, 453)]
    total = sum(x[2] for x in b['cols'])
    ok = not b['err'] and got == c[2]
    if not ok:
        bad += 1
    if VERBOSE or not ok:
        print('%-4s %s: lengths %s, the row %d bytes%s' % ('ok' if ok else 'DIFF', c[0], got, total,
                                                       '' if ok else ' - expected %s %s' % (c[2], b['err'] or '')))
b = Q[len(C)]
val = str(b['rows'][0]) if b['rows'] else ''
ok = val.count('è') == 50 and not b['err']
if not ok:
    bad += 1
if VERBOSE or not ok:
    print('%-4s fifty accented letters in a column of fifty characters arrive whole: %d of them' % ('ok' if ok else 'DIFF', val.count('è')))
print('%s: %d checks of the lengths under CCSID 1208, %d differ' % (KIND, len(C) + 1, bad))
sys.exit(1 if bad else 0)
