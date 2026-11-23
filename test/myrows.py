#!/usr/bin/env python3
# myrows.py - many rows of every shape from a MySQL or MariaDB server, each
# value checked against what it must be.
#
# usage: python3 myrows.py <emu dir> [rows (5000)] [-v]
#   the server: see xdbqa.py (XDB_MY_PORT, XDB_MY_DB, XDB_MY_USER).
#
# One query makes the rows from their number alone (a table of numbers,
# xdb_qa_n, made for the run and dropped after it):
# integers, decimals, floating point, dates and times, text of every length
# from nothing to its limit, accented text, bytes, long values, NULLs in a
# pattern.  What every value must be is computed here from the number of
# the row.  The same rows are read with 1, 200 and 3000 rows asked of the
# server at a time (option fetch_rows), under commitment control and
# without, and with the profile asking for UTF-8.
import sys
from decimal import Decimal
from datetime import datetime, timedelta, date

import xdbqa

EMU = sys.argv[1]
args = [a for a in sys.argv[2:] if not a.startswith('-')]
N = int(args[0]) if args else 5000
VERBOSE = '-v' in sys.argv[2:]
T = xdbqa.Target('mysql')
ver, vtext = T.version()
maria = 'MariaDB' in vtext

SQL = ("select i, cast(i * 7919 % 100003 - 50000 as signed) as c_int, i * 1000000007 as c_big, "
       "cast(i as decimal(18,4)) * 0.3331 as c_dec, i / 3e0 as c_flt, "
       "timestamp('2020-01-01 00:00:00') + interval (i % 3000) day + interval (i * 1237 % 86400) second as c_ts, "
       "date('2021-03-04') + interval (i % 5000) day as c_date, sec_to_time(i * 7 % 86400) as c_time, "
       "repeat(char(65 + i % 26 using utf8), i % 201) as c_v, repeat(convert(char(224 + i % 30) using latin1), i % 101) as c_nv, "
       "if(i % 3 = 0, null, i) as c_n1, if(i % 5 = 0, null, cast(i as char)) as c_n2, "
       "if(i % 7 = 0, null, cast(i as decimal(9,2))) as c_n3, "
       "if(i % 50 = 0, repeat('L', 20000 + i), 'short') as c_long, "
       "(i % 2 = 0) as c_bool, cast(i % 128 as signed) as c_small, "
       "unhex(repeat(lpad(hex(1 + i % 255), 2, '0'), i % 41)) as c_ch "
       "from xdb_qa_n where i <= " + str(N) + " order by i")
NAMES = ['i', 'c_int', 'c_big', 'c_dec', 'c_flt', 'c_ts', 'c_date', 'c_time', 'c_v', 'c_nv', 'c_n1', 'c_n2', 'c_n3', 'c_long', 'c_bool',
         'c_small', 'c_ch']
KINDS = {'i': 'int', 'c_int': 'int', 'c_big': 'int', 'c_dec': 'dec', 'c_flt': 'float', 'c_ts': 'ts', 'c_date': 'date', 'c_time': 'time',
         'c_v': 'text', 'c_nv': 'text', 'c_n1': 'int', 'c_n2': 'text', 'c_n3': 'dec', 'c_long': 'text', 'c_bool': 'int', 'c_small': 'int',
         'c_ch': 'text'}


def expect(i):
    ts = datetime(2020, 1, 1) + timedelta(days=i % 3000, seconds=i * 1237 % 86400)
    sec = i * 7 % 86400
    lng = 'L' * min(20000 + i, 32000) if i % 50 == 0 else 'short'
    if len(lng) > 1000:
        lng = '%s... %d bytes' % (lng[:60], len(lng))       # as ardsh prints a long value
    return {
        'i': i, 'c_int': i * 7919 % 100003 - 50000, 'c_big': i * 1000000007,
        'c_dec': Decimal(i) * Decimal('0.3331'),
        'c_flt': i / 3,
        'c_ts': ts.strftime('%Y-%m-%d %H:%M:%S') + '.%06d' % ts.microsecond,
        'c_date': (date(2021, 3, 4) + timedelta(days=i % 5000)).isoformat(),
        'c_time': '%02d:%02d:%02d' % (sec // 3600, sec // 60 % 60, sec % 60),
        'c_v': chr(65 + i % 26) * (i % 201), 'c_nv': chr(224 + i % 30) * (i % 101),
        'c_n1': None if i % 3 == 0 else i, 'c_n2': None if i % 5 == 0 else str(i),
        'c_n3': None if i % 7 == 0 else Decimal(i),
        'c_long': lng, 'c_bool': 1 if i % 2 == 0 else 0, 'c_small': i % 128,
        'c_ch': ('%02X' % (1 + i % 255)) * (i % 41),
    }


def run(label, options, iso, ccsid=0):
    T.profile(driver=options, text_len=32000, ccsid=ccsid)
    out = T.run(EMU, ['ISO ' + iso, 'C', 'Q ' + SQL, 'D'], maxrows=1000000)
    B = [b for b in xdbqa.blocks(out) if b['cmd'].startswith('Q ')]
    bad = 0
    first = None
    if not B or B[0]['err'] or len(B[0]['rows']) != N:
        bad = 1
        first = (B[0]['err'] if B and B[0]['err'] else '%d rows arrived, %d were made' % (len(B[0]['rows']) if B else 0, N))
    if 'runtime error' in out or 'AddressSanitizer' in out:
        bad += 1
        first = first or 'a fault reported by the sanitizers'
    rows = B[0]['rows'] if B else []
    cols = {c[0].lower(): c for c in B[0]['cols']} if B else {}
    for n, row in enumerate(rows[:N]):
        vals = xdbqa.split(row)
        e = expect(n + 1)
        what = None
        if len(vals) != len(NAMES):
            what = 'the row has %d values' % len(vals)
        else:
            for k, name in enumerate(NAMES):
                if not xdbqa.same(KINDS[name], cols[name], vals[k], e[name]):
                    what = '%s: got %s, it is %s' % (name, vals[k][:70], str(e[name])[:70])
                    break
        if what:
            bad += 1
            first = first or 'row %d, %s' % (n + 1, what)
            if VERBOSE:
                print('   row %d, %s' % (n + 1, what))
    print('%-64s %s' % (label, 'ok, %d rows' % len(rows) if not bad else 'DIFF in %d places; first: %s' % (bad, first)))
    return bad


T.sql('drop table if exists xdb_qa_n')
T.sql('drop table if exists xdb_qa_d')
T.sql('create table xdb_qa_d (d int not null)')
T.sql('insert into xdb_qa_d values (0),(1),(2),(3),(4),(5),(6),(7),(8),(9)')
T.sql('create table xdb_qa_n (i int not null primary key)')
T.sql('insert into xdb_qa_n select 1 + a.d + b.d * 10 + c.d * 100 + e.d * 1000 + f.d * 10000 '
      'from xdb_qa_d a, xdb_qa_d b, xdb_qa_d c, xdb_qa_d e, xdb_qa_d f')
total = 0
name = '%s %s' % ('MariaDB' if maria else 'MySQL', '.'.join(str(x) for x in ver))
try:
    for iso in ('*CHG', '*NONE'):
        for fr in (0, 1, 3000):
            if fr == 1 and N > 2000:
                continue
            total += run('%s, %s, %s rows at a time' % (name, 'commitment control' if iso == '*CHG' else 'no commitment control',
                                                        fr or 'default (200)'), {'fetch_rows': fr} if fr else {}, iso)
    total += run('%s, character data in UTF-8' % name, {}, '*CHG', ccsid=1208)
finally:
    T._client = None
    T.sql('drop table if exists xdb_qa_n')
    T.sql('drop table if exists xdb_qa_d')
sys.exit(1 if total else 0)
