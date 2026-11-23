#!/usr/bin/env python3
# mytypes.py - every data type of a MySQL or MariaDB server through the
# driver, against what the value is.
#
# usage: python3 mytypes.py <emu dir> [utf8] [-v]
#   the server: see xdbqa.py (XDB_MY_PORT, XDB_MY_DB, XDB_MY_USER).
#
# Every value is put into a column of its type in a temporary table (by the
# driver itself, as a literal) and read back twice: with a statement
# without markers and with one that has a marker.  The driver prepares
# both, so the server answers both in its binary protocol; they differ in
# how the statement reaches the server.  What the value must be is written
# here.
# Values Db2 cannot hold (the date 0000-00-00, a time of 838 hours) must
# arrive as "could not be converted", not as another value.
import sys

import xdbqa

EMU = sys.argv[1]
UTF8 = 'utf8' in sys.argv[2:]
VERBOSE = '-v' in sys.argv[2:]
T = xdbqa.Target('mysql')
T.profile(ccsid=1208 if UTF8 else 0)
ver, vtext = T.version()
maria = 'MariaDB' in vtext
frac = ver >= (5, 6, 4) if not maria else ver >= (5, 3)        # fractions of a second
jsontype = (not maria and ver >= (5, 7, 8)) or (maria and ver >= (10, 2))
vectype = (not maria and ver >= (9, 0)) or (maria and ver >= (11, 7))  # arrives as the hexadecimal text of its 4-byte floats

C = []      # (label, column type, literal, kind, want)


def add(typ, lit, kind, want, label=None):
    C.append((label or '%s %s' % (typ, lit), typ, lit, kind, want))


for typ, vals in (('tinyint', (0, 127, -128)), ('tinyint unsigned', (255,)), ('smallint', (32767, -32768)), ('smallint unsigned', (65535,)),
                  ('mediumint', (8388607, -8388608)), ('mediumint unsigned', (16777215,)), ('int', (2147483647, -2147483648)),
                  ('int unsigned', (4294967295,)), ('bigint', (9223372036854775807, -9223372036854775808)), ('year', (1901, 2155)),
                  ('bit(1)', (1, 0)), ('bit(8)', (255,)), ('bit(63)', (9223372036854775807,))):
    for v in vals:
        add(typ, str(v), 'int', v)
add('bigint unsigned', '18446744073709551615', 'dec', '18446744073709551615')
add('bit(64)', "b'" + '1' * 64 + "'", 'dec', '18446744073709551615', 'bit(64) all ones')
add('bit(64)', "b'101'", 'dec', '5', 'bit(64) five')
for typ, v in (('decimal(10,2)', '12.50'), ('decimal(10,2)', '-99999999.99'), ('decimal(30,0)', '9' * 30), ('decimal(31,10)', '123456789012345678901.0123456789'),
               ('decimal(30,30)', '0.' + '1234567890' * 3), ('decimal(5,0)', '0'), ('decimal(20,4) unsigned', '1234567890123456.7890')):
    add(typ, v, 'dec', v)
add('decimal(65,2)', '9' * 29 + '.25', 'dec', '9' * 29 + '.25', 'decimal(65,2) with 29 digits before the point')
add('decimal(65,2)', '9' * 40 + '.25', 'bad', 1, 'decimal(65,2) with 40 digits before the point')
for v in ('0', '1.5', '-1.5', '0.1', '0.30000000000000004', '1.7976931348623157e308', '2.2250738585072014e-308', '123456789012345680'):
    add('double', v, 'float', float(v))
for v in ('1.5', '0.1', '3.4028235e38', '16777217', '123456.79', '1e-10'):
    add('float', v, 'reals', float(v))
add('float(7,3)', '1234.567', 'reals', 1234.567)
add('double(12,4)', '12345678.1234', 'float', 12345678.1234)
for v in ('2026-01-31', '1000-01-01', '9999-12-31', '2000-02-29', '0001-01-01'):
    add('date', "'%s'" % v, 'date', v)
add('date', "'0000-00-00'", 'bad', 1, 'date zero')
add('date', "'2026-00-00'", 'bad', 1, 'date with month and day zero')
for v in ('00:00:00', '23:59:59', '13:45:10'):
    add('time', "'%s'" % v, 'time', v)
add('time', "'838:59:59'", 'bad', 1)
add('time', "'-12:30:00'", 'bad', 1)
add('time', "'24:00:00'", 'time', '24:00:00')
for v in ('2026-01-31 13:45:10', '1000-01-01 00:00:00', '9999-12-31 23:59:59'):
    add('datetime', "'%s'" % v, 'ts', v)
add('datetime', "'0000-00-00 00:00:00'", 'bad', 1, 'datetime zero')
add('timestamp', "'2026-01-31 13:45:10'", 'ts', '2026-01-31 13:45:10')
add('timestamp null', "'1970-01-02 00:00:01'", 'ts', '1970-01-02 00:00:01')
if frac:
    add('datetime(6)', "'2026-01-31 13:45:10.123456'", 'ts', '2026-01-31 13:45:10.123456')
    add('datetime(3)', "'2026-01-31 13:45:10.987'", 'ts', '2026-01-31 13:45:10.987')
    add('timestamp(6) null', "'2026-01-31 13:45:10.000001'", 'ts', '2026-01-31 13:45:10.000001')
    add('time(6)', "'13:45:10.987654'", 'time', '13:45:10')
add('char(10)', "'abc'", 'text', 'abc')
add('char(5)', "'Größ'", 'text', 'Größ')
add('varchar(50)', "'x'", 'text', 'x')
add('varchar(50)', "''", 'text', '')
add('varchar(20)', "'Città èéù €'", 'text', 'Città èéù €')
add('varchar(20) character set latin1', "'Città èéù'", 'text', 'Città èéù')
add('char(3) character set latin1', "'àèì'", 'text', 'àèì')
add('varchar(10) character set utf8mb4', "concat(char(0xD0AF using utf8mb4), char(0xE697A5 using utf8mb4), char(0xF09F9880 using utf8mb4), '!')", 'text',
    'Я日\U0001f600!' if UTF8 else '\x1a\x1a\x1a!', "a character outside the job's character set")
for typ in ('tinytext', 'text', 'mediumtext', 'longtext'):
    add(typ, "'short'", 'text', 'short')
add('text', "repeat('y', 600)", 'text', 'y' * 600, 'text of 600')
add('longtext', "repeat('z', 5000)", 'text', 'z' * 1000, 'longtext of 5000, cut at the text length 1000')
add('binary(4)', "0x0A0B", 'text', '0A0B0000')
add('varbinary(20)', "0x00FF10", 'text', '00FF10')
add('varbinary(20)', "''", 'text', '')
add('blob', "0x0102030405", 'text', '0102030405')
add('tinyblob', "0xFF", 'text', 'FF')
add('longblob', "repeat(char(65), 300)", 'text', '41' * 300, 'longblob of 300 bytes')
add("enum('sad','ok','happy')", "'ok'", 'text', 'ok')
add("set('a','b','c')", "'a,c'", 'text', 'a,c')
if jsontype:
    add('json', "'{\"a\": 1, \"b\": [true, null]}'", 'json', '{"a": 1, "b": [true, null]}')
if vectype:
    add('vector(3)', "vec_fromtext('[1,2,3]')" if maria else "string_to_vector('[1,2,3]')", 'text', '0000803F0000004000004040', 'vector(3) of 1, 2, 3')
add('point', "point(1, 2)" if (maria or ver >= (5, 6)) else "geomfromtext('POINT(1 2)')", 'text', '000000000101000000000000000000F03F0000000000000040')
for typ in ('tinyint', 'smallint', 'int', 'bigint', 'bigint unsigned', 'decimal(10,2)', 'double', 'float', 'date', 'time', 'datetime',
            'timestamp null', 'char(10)', 'varchar(50)', 'text', 'blob', 'varbinary(20)', 'year', 'bit(8)', "enum('a','b')"):
    C.append(('%s NULL' % typ, typ, 'null', 'null', None))

script = ['ISO *NONE', 'C', "X set session sql_mode = ''"]      # zero dates and the like are wanted here
for n, c in enumerate(C):
    script.append('X create temporary table xdb_qa_t%d (c %s)' % (n, c[1]))
    script.append('X insert into xdb_qa_t%d values (%s)' % (n, c[2]))
    script.append('Q select c from xdb_qa_t%d' % n)
    script.append('Q select c from xdb_qa_t%d where 1 = ? ## i:1' % n)
script.append('D')
out = T.run(EMU, script)
B = xdbqa.blocks(out)
Q = [b for b in B if b['cmd'].startswith('Q select c from')]
X = [b for b in B if b['cmd'].startswith('X create') or b['cmd'].startswith('X insert')]
if len(Q) != 2 * len(C) or len(X) != 2 * len(C):
    print('the harness answered %d + %d requests of %d + %d' % (len(Q), len(X), 2 * len(C), 2 * len(C)))
    print(out[-1500:])
    sys.exit(2)
bad = skipped = 0
for n, c in enumerate(C):
    label, typ, lit, kind, want = c
    setup = X[2 * n]['err'] or X[2 * n + 1]['err']
    if setup:
        skipped += 1
        if VERBOSE:
            print('skip %-62s the server does not take it: %s' % (label[:62], setup[:90]))
        continue
    for how, b in (('without a marker', Q[2 * n]), ('with a marker', Q[2 * n + 1])):
        what = None
        if b['err'] or not b['rows'] or not b['cols']:
            what = 'no value: %s' % (b['err'] or 'no row')
        else:
            got, col = b['rows'][0], b['cols'][0]
            if kind == 'null':
                ok = got == '<null>'
            elif kind == 'json':
                ok = got not in ('<null>', '<conv-error>') and xdbqa.text_of(got).replace(' ', '') == want.replace(' ', '')
            else:
                ok = xdbqa.same(kind, col, got, want)
            if not ok:
                what = 'got %s as %s, it is %s' % (got[:70], xdbqa.coltype(col), 'a value Db2 cannot hold' if kind == 'bad' else repr(want)[:70])
        if what:
            bad += 1
        if what or VERBOSE:
            print('%-4s %-62s %s: %s' % ('DIFF' if what else 'ok', label[:62], how, what or '%s as %s' % (b['rows'][0][:40], xdbqa.coltype(b['cols'][0]))))
print('%s %s, %d values, each read without and with a marker%s: %d differ%s' % (
    'MariaDB' if maria else 'MySQL', '.'.join(str(x) for x in ver), len(C), ', character data in UTF-8' if UTF8 else '', bad,
    ', %d not taken by this server' % skipped if skipped else ''))
sys.exit(1 if bad else 0)
