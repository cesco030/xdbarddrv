#!/usr/bin/env python3
# myparams.py - what a host variable becomes on a MySQL or MariaDB server,
# checked against the server itself.
#
# usage: python3 myparams.py <emu dir> [-v]
#   the server: see xdbqa.py (XDB_MY_PORT, XDB_MY_DB, XDB_MY_USER).
#
# For every type of column (TARGETS) there is a table xdb_qa_p<n> (id, src,
# c).  Every host variable (PARAMS) is inserted into each of them twice: by
# the driver as a parameter (src 'P'), and by PyMySQL as a literal of the
# kind the host variable is (src 'L'): a number as a number, a date as a
# date, text as text.  The server must have kept the same in both rows, or
# have refused both.  The rows are read back by PyMySQL, in hexadecimal.
#
# A second part looks the rows up again with the same host variables in a
# WHERE clause, and a third uses them in expressions: both must give what
# the literal gives.
import struct
import sys

import xdbqa

EMU = sys.argv[1]
VERBOSE = '-v' in sys.argv[2:]
T = xdbqa.Target('mysql')
ver, vtext = T.version()

maria = 'MariaDB' in vtext
frac = ver >= (5, 6, 4) if not maria else ver >= (5, 3)
TARGETS = ['tinyint', 'smallint', 'int', 'bigint', 'bigint unsigned', 'decimal(18,4)', 'decimal(38,10)', 'decimal(65,30)', 'float', 'double',
           'bit(8)', 'bit(64)', 'year', 'date', 'time', 'datetime', 'timestamp null', 'char(10)', 'varchar(50)', 'varchar(50) character set latin1',
           'text', 'longtext', 'varbinary(50)', 'blob', "enum('1','2','abc','true')", "set('1','2','abc')"]
if frac:
    TARGETS += ['datetime(6)', 'time(6)']
if (not maria and ver >= (5, 7, 8)) or (maria and ver >= (10, 2)):
    TARGETS.append('json')

P = []      # (label, host variable for ardsh, the value written out in full, the kind of literal: n number, s text, d date, t time, z timestamp)


def add(label, spec, text, kind='s'):
    P.append((label, spec, text, kind))


for v in (0, 1, -1, 2147483647, -2147483648):
    add('INTEGER %d' % v, 'i:%d' % v, str(v), 'n')
for v in (32767, -32768):
    add('SMALLINT %d' % v, '@s:%d' % v, str(v), 'n')
for v in (255, 9223372036854775807, -9223372036854775808, 9007199254740993):
    add('BIGINT %d' % v, '@b:%d' % v, str(v), 'n')
for v in ('0', '1.5', '-1.5', '0.1', '1.7976931348623157e308', '2.2250738585072014e-308', '123456789012345680', '1e-5',
          '0.30000000000000004'):
    add('DOUBLE %s' % v, '@f:%s' % v, xdbqa.float_text(float(v)), 'f')
for v in ('1.5', '0.1', '3.4028235e38'):
    w = struct.unpack('f', struct.pack('f', float(v)))[0]      # the value a REAL variable holds, as the double it is
    add('REAL %s' % v, '@r:%s' % v, xdbqa.float_text(w, True), 'f')
for spec, v, full in (('p15,4', '12.5', '12.5000'), ('p15,4', '-99999999999.9999', '-99999999999.9999'), ('p15,4', '0', '0.0000'),
                      ('p5,2', '-0.01', '-0.01'), ('p31,0', '9' * 31, '9' * 31), ('p31,31', '0.' + '1234567890' * 3 + '1', '0.' + '1234567890' * 3 + '1'),
                      ('p31,5', '-' + '9' * 26 + '.99999', '-' + '9' * 26 + '.99999'), ('z7,2', '12345.67', '12345.67'), ('z7,2', '-0.5', '-0.50'),
                      ('p63,2', '1.5', '1.50'), ('p63,0', '9' * 63, '9' * 63), ('p9,0', '3', '3')):
    add('%s(%s) %s' % ('DECIMAL' if spec[0] == 'p' else 'NUMERIC', spec[1:], v[:24]), '@%s:%s' % (spec, v), full, 'n')
# the blanks at the end of a CHAR variable are not sent (option trim_char)
add('CHAR(10) abc', '@c10:abc', 'abc')
add('CHAR(5) with accents', '@c5:Città', 'Città')
add('CHAR(3) blank', '@c3: ', '')
for lab, v in (('abc', 'abc'), ('empty', ''), ('trailing blanks', 'ab  '), ('accents and the euro', 'Città è già 5 €'),
               ('a quote', "O'Brien"), ('a backslash', 'a\\b'), ('a number', '42'), ('a decimal number', '12.50'), ('true', 'true'),
               ('a date', '2026-01-31'), ('a timestamp', '2026-01-31 13:45:10.123'), ('a time', '13:45:10'),
               ('a uuid', '6F9619FF-8B86-D011-B42D-00C04FC964FF'), ('json', '{"a": [1, 2], "b": "x"}'), ('an interval', '1 day 02:03:04'),
               ('a word', 'abc'), ('a year', '2026'),
               ('8000 characters', 'x' * 8000), ('12000 characters', 'z' * 12000)):
    add('VARCHAR %s' % lab, '@v:%s' % v, v)
for lab, v in (('abc', 'abc'), ('Cyrillic, euro, kanji', 'Я€日'), ('outside the basic plane', '\U0001f600!'),
               ('4000 characters', 'я' * 4000)):
    add('VARGRAPHIC %s' % lab, '@g:%s' % v, v)
for v in ('2026-01-31', '0001-01-01', '9999-12-31', '2000-02-29'):
    add('DATE %s' % v, '@D:%s' % v, v, 'd')
for v in ('00:00:00', '23:59:59', '13:45:10', '24:00:00'):
    add('TIME %s' % v, '@T:%s' % v, v, 't')
for v in ('2026-01-31-13.45.10.123456', '2026-01-31-13.45.10.000000', '0001-01-01-00.00.00.000000', '9999-12-31-23.59.59.999999',
          '2026-12-31-23.59.59.999999'):
    add('TIMESTAMP %s' % v, '@Z:%s' % v, v[:10] + ' ' + v[11:].replace('.', ':', 2), 'z')
add('NULL VARCHAR', 'n:', None)
for lab, spec in (('SMALLINT', '@s'), ('BIGINT', '@b'), ('DOUBLE', '@f'), ('DECIMAL', '@p15,4'), ('CHAR', '@c10'), ('VARGRAPHIC', '@g'),
                  ('DATE', '@D'), ('TIME', '@T'), ('TIMESTAMP', '@Z')):
    add('NULL %s' % lab, '%s:@null' % spec, None)


def family(typ):
    t = typ.split('(')[0].split(' ')[0]
    if t in ('tinyint', 'smallint', 'int', 'bigint', 'decimal', 'float', 'double', 'bit'):
        return 'num'
    if t in ('date', 'datetime', 'timestamp'):
        return 'date'
    if t in ('time', 'year', 'json'):
        return t
    return 'text'


def sensible(p, typ, search):
    """Is this host variable into (or compared with) this type of column something the server treats the same for a
    parameter and for a literal?  MySQL has corners where it does not, whatever a client sends: a floating point
    parameter becomes a BIT or a YEAR by cutting where the literal is rounded; a number is written into a character
    column with other digits; text the column's character set lacks is refused as a parameter and replaced as a
    literal; and comparisons between unrelated types (a number with a year, text with a date, a date with a time)
    follow different rules for the two.  Those are left out: they say nothing about the driver."""
    kind, fam = p[3], family(typ)
    if not search:
        if kind == 'f' and (fam in ('year',) or typ.startswith('bit') or fam == 'text' or fam == 'json'):
            return False
        if kind == 'n' and fam in ('json',) and '.' in (p[2] or ''):
            return False
        if 'latin1' in typ and p[2] and any(ord(ch) > 255 for ch in p[2]):
            return False
        return True
    if kind in ('n', 'f'):
        return fam == 'num'
    if kind == 'd':
        return fam in ('date', 'text')
    if kind == 't':
        return fam in ('time', 'text')
    if kind == 'z':
        return fam in ('date', 'text')
    return fam == 'text'


def lit(p):
    text, kind = p[2], p[3]
    if text is None:
        return 'null'
    if kind in ('n', 'f'):
        return text if (kind == 'n' or 'e' in text or '.' in text) else text + 'e0'     # a DOUBLE is an approximate number
    q = "'" + text.replace('\\', '\\\\').replace("'", "''") + "'"
    return q        # a date, a time and a timestamp go as their text, like character data


T.sql("set session sql_mode = ''")
for t in range(len(TARGETS)):
    T.sql('drop table if exists xdb_qa_p%d' % t)
    T.sql('create table xdb_qa_p%d (id int not null, src char(1) not null, c %s null, primary key (id, src))' % (t, TARGETS[t]))
try:
    lit_err = {}
    for t in range(len(TARGETS)):
        for i, p in enumerate(P):
            try:
                T.sql("insert into xdb_qa_p%d (id, src, c) values (%d, 'L', %s)" % (t, i, lit(p).replace('%', '%%')), ())
            except Exception as e:
                lit_err[(t, i)] = str(e).split('\n')[0][:110]
    script = ['ISO *NONE', 'C', "X set session sql_mode = ''"]
    order = []
    for t in range(len(TARGETS)):
        script.append("P s%d insert into xdb_qa_p%d (id, src, c) values (?, 'P', ?)" % (t, t))
        for i, p in enumerate(P):
            script.append('E s%d i:%d|%s' % (t, i, p[1]))
            order.append((t, i))
    # the second part: each value looks for its own row
    look = []
    for t in range(len(TARGETS)):
        if TARGETS[t] == 'json':
            continue        # compared as documents, not as values
        for i, p in enumerate(P):
            if p[2] is not None and sensible(p, TARGETS[t], True):
                script.append("Q select count(*) as n, cast(sum(case when id = %d then 1 else 0 end) as signed) as me from xdb_qa_p%d where src = 'L' and c = ? ## %s" % (i, t, p[1]))
                look.append((t, i))
    script.append('D')
    out = T.run(EMU, script)
    B = xdbqa.blocks(out)
    E = [b for b in B if b['cmd'].startswith('E s')]
    Q = [b for b in B if b['cmd'].startswith('Q select count')]
    if len(E) != len(order) or len(Q) != len(look):
        print('the harness answered %d + %d requests of %d + %d' % (len(E), len(Q), len(order), len(look)))
        print(out[-1500:])
        sys.exit(2)
    par_err = {k: b['err'] for k, b in zip(order, E) if b['err']}

    T._client = None
    T.sql("set session sql_mode = ''")
    bad = same = refused = left_out = 0
    rows = {}
    texts = {}
    for t in range(len(TARGETS)):
        rows[t] = {}
        for r in T.sql('select id, src, c is null, hex(c), left(c, 60) from xdb_qa_p%d' % t):
            rows[t][(r[0], r[1].strip())] = (r[2], r[3])
            texts[(t, r[0], r[1].strip())] = r[4]
        for i, p in enumerate(P):
            a, b = rows[t].get((i, 'P')), rows[t].get((i, 'L'))
            k = (t, i)
            what = None
            if not sensible(p, TARGETS[t], False):
                left_out += 1
                continue
            if a is None and b is None:
                refused += 1
                if k not in par_err or k not in lit_err:
                    what = 'no row and no error (%s / %s)' % (par_err.get(k), lit_err.get(k))
            elif a is None:
                what = 'the server takes the literal (%s), the parameter failed: %s' % (str(texts.get((t, i, 'L')))[:40], par_err.get(k, 'no error told'))
            elif b is None:
                what = 'the parameter was taken (%s), the literal fails: %s' % (str(texts.get((t, i, 'P')))[:40], lit_err.get(k, '?'))
            elif a != b:
                what = 'parameter %s, literal %s' % (str(texts.get((t, i, 'P')))[:60], str(texts.get((t, i, 'L')))[:60])
            else:
                same += 1
            if what:
                bad += 1
            if what or VERBOSE:
                print('%-4s %-34s -> %-24s %s' % ('DIFF' if what else 'ok', p[0][:34], TARGETS[t][:24], what or (str(texts.get((t, i, 'P')))[:60] if a else 'refused by both')))
    # the searches: a host variable in a WHERE clause must find what the same value finds as a literal
    found = 0
    for (t, i), b in zip(look, Q):
        p = P[i]
        try:
            r = T.sql("select count(*), cast(sum(case when id = %d then 1 else 0 end) as signed) from xdb_qa_p%d where src = 'L' and c = %s" % (i, t, lit(p).replace('%', '%%')), ())[0]
            want = (str(r[0]), '<null>' if r[1] is None else str(r[1]))
        except Exception as e:
            want = None
        what = None
        if b['err']:
            if want is not None:
                what = 'the search with the literal gives %s, with the parameter it failed: %s' % (want, b['err'][:110])
        else:
            got = tuple(xdbqa.split(b['rows'][0])) if b['rows'] else ('?', '?')
            if want is None:
                what = 'the search with the literal fails, with the parameter it gives %s' % (got,)
            elif got != want:
                what = 'count and own row with the parameter %s, with the literal %s' % (got, want)
            else:
                found += 1
        if what:
            bad += 1
            print('DIFF %-34s  =  %-24s %s' % (p[0][:34], TARGETS[t][:24], what))
    print('%s %s: %d host variables into %d column types: %d the same as the literal, %d refused by both; '
          '%d searches the same as with the literal; %d differ (%d combinations left out, see sensible())' % (
              'MariaDB' if maria else 'MySQL', '.'.join(str(x) for x in ver), len(P), len(TARGETS), same, refused, found, bad, left_out))
finally:
    T._client = None
    for t in range(len(TARGETS)):
        T.sql('drop table if exists xdb_qa_p%d' % t)
sys.exit(1 if bad else 0)
