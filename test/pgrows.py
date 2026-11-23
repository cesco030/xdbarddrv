#!/usr/bin/env python3
# pgrows.py - many rows of every shape from a PostgreSQL server, each value
# checked against what it must be.
#
# usage: python3 pgrows.py <emu dir> [rows (5000)] [-v]
#   the server: see xdbqa.py (XDB_PG_PORT, XDB_PG_DB, XDB_PG_USER).
#
# One query makes the rows from their number alone (generate_series):
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
T = xdbqa.Target('pg')
ver, vtext = T.version()

SQL = ("select i, cast(i * 7919 % 100003 - 50000 as integer) as c_int, cast(i as bigint) * 1000000007 as c_big, "
       "cast(cast(i as numeric(18,4)) / 7 as numeric(18,6)) as c_dec, cast(i as double precision) / 3 as c_flt, "
       "timestamp '2020-01-01 00:00:00' + (i % 3000) * interval '1 day' + (i * 1237 % 86400000) * interval '0.001 second' as c_ts, "
       "date '2021-03-04' + (i % 5000) as c_date, time '00:00:00' + (i * 7 % 86400) * interval '1 second' as c_time, "
       "repeat(chr(65 + i % 26), i % 201) as c_v, cast(repeat(chr(224 + i % 30), i % 101) as varchar(100)) as c_nv, "
       "case when i % 3 = 0 then null else i end as c_n1, case when i % 5 = 0 then null else cast(i as varchar(10)) end as c_n2, "
       "case when i % 7 = 0 then null else cast(i as numeric(9,2)) end as c_n3, "
       "case when i % 50 = 0 then repeat('L', 20000 + i) else 'short' end as c_long, "
       "(i % 2 = 0) as c_bool, cast(i % 32768 as smallint) as c_small, "
       "cast(case when i % 11 = 0 then null else repeat('n', i % 17) end as char(16)) as c_ch "
       "from generate_series(1, " + str(N) + ") as g(i) order by i")
NAMES = ['i', 'c_int', 'c_big', 'c_dec', 'c_flt', 'c_ts', 'c_date', 'c_time', 'c_v', 'c_nv', 'c_n1', 'c_n2', 'c_n3', 'c_long', 'c_bool',
         'c_small', 'c_ch']
KINDS = {'i': 'int', 'c_int': 'int', 'c_big': 'int', 'c_dec': 'dec', 'c_flt': 'float', 'c_ts': 'ts', 'c_date': 'date', 'c_time': 'time',
         'c_v': 'text', 'c_nv': 'text', 'c_n1': 'int', 'c_n2': 'text', 'c_n3': 'dec', 'c_long': 'text', 'c_bool': 'int', 'c_small': 'int',
         'c_ch': 'text'}


def expect(i):
    ts = datetime(2020, 1, 1) + timedelta(days=i % 3000, milliseconds=i * 1237 % 86400000)
    sec = i * 7 % 86400
    lng = 'L' * min(20000 + i, 32000) if i % 50 == 0 else 'short'
    if len(lng) > 1000:
        lng = '%s... %d bytes' % (lng[:60], len(lng))       # as ardsh prints a long value
    return {
        'i': i, 'c_int': i * 7919 % 100003 - 50000, 'c_big': i * 1000000007,
        'c_dec': (Decimal(i) / Decimal(7)).quantize(Decimal('0.000001')),
        'c_flt': i / 3,
        'c_ts': ts.strftime('%Y-%m-%d %H:%M:%S') + '.%06d' % ts.microsecond,
        'c_date': (date(2021, 3, 4) + timedelta(days=i % 5000)).isoformat(),
        'c_time': '%02d:%02d:%02d' % (sec // 3600, sec // 60 % 60, sec % 60),
        'c_v': chr(65 + i % 26) * (i % 201), 'c_nv': chr(224 + i % 30) * (i % 101),
        'c_n1': None if i % 3 == 0 else i, 'c_n2': None if i % 5 == 0 else str(i),
        'c_n3': None if i % 7 == 0 else Decimal(i),
        'c_long': lng, 'c_bool': 1 if i % 2 == 0 else 0, 'c_small': i % 32768,
        'c_ch': None if i % 11 == 0 else 'n' * (i % 17),
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


total = 0
for iso in ('*CHG', '*NONE'):
    for fr in (0, 1, 3000):
        if fr == 1 and N > 2000:
            continue
        total += run('PostgreSQL %d.%d, %s, %s rows at a time' % (ver[0], ver[1], 'commitment control' if iso == '*CHG' else 'no commitment control',
                                                                  fr or 'default (200)'), {'fetch_rows': fr} if fr else {}, iso)
total += run('PostgreSQL %d.%d, character data in UTF-8' % ver, {}, '*CHG', ccsid=1208)
sys.exit(1 if total else 0)
