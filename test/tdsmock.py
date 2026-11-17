#!/usr/bin/env python3
"""
tdsmock.py - a small TDS 7.1 server for testing the XDB SQL Server backend
where no SQL Server is available.  It was written from the [MS-TDS]
specification independently of xdbms.c and is itself checked against two
unrelated clients (FreeTDS tsql and python-tds), so that both directions of
the protocol are compared with third-party implementations.

SQL is executed by sqlite3, so only portable SQL works.  Column types come
from the CREATE TABLE statements seen on the connection (by column name),
NOT NULL / PRIMARY KEY columns use the fixed-length TDS types as the real
server does.

Parameters of the types image and ntext (a file sent by the driver's
GET_BLOB_FROM_FILE / GET_CLOB_FROM_FILE at TDS 7.1) are stored as blob and
text; xdb_md5(value) gives the MD5 of what arrived.  The mock answers as
SQL Server 2000 (version 8.00), which has no (max) types.

Conventions for failure tests:
  a statement containing  xdb_deadlock   fails with error 1205 and rolls
                                          the transaction back
  sp_cursoropen on text with /*nocursor*/ fails with error 16937

usage: tdsmock.py [port]     (login: sa / xdbpw)
"""
import datetime
import decimal
import hashlib
import re
import socket
import sqlite3
import struct
import sys
import threading
import uuid

COLL = bytes([0x09, 0x04, 0xD0, 0x00, 0x34])    # SQL_Latin1_General_CP1_CI_AS
TYPES = {}                                       # column name -> (kind, a, b, notnull)


def us(s):
    b = s.encode('utf-16-le')
    return struct.pack('<H', len(b) // 2) + b


def bv(s):
    b = s.encode('utf-16-le')
    return struct.pack('<B', len(b) // 2) + b


def done(tok, status=0, count=0):
    return struct.pack('<BHHI', tok, status, 0, count)


def error(num, cls, msg):
    body = struct.pack('<IBB', num, 1, cls) + us(msg) + bv('MOCK') + bv('') + struct.pack('<H', 1)
    return struct.pack('<BH', 0xAA, len(body)) + body


def envchange(kind, new, old=b''):
    if isinstance(new, str):
        body = struct.pack('<B', kind) + bv(new) + bv(old if isinstance(old, str) else '')
    else:
        body = struct.pack('<BB', kind, len(new)) + new + b'\x00'
    return struct.pack('<BH', 0xE3, len(body)) + body


# ---------------------------------------------------------------- type system

def numlen(prec):
    return 5 if prec <= 9 else 9 if prec <= 19 else 13 if prec <= 28 else 17


def typeinfo(t, colmeta=True):
    kind, a, b, nn = t
    if kind == 'int':
        return bytes([{1: 0x30, 2: 0x34, 4: 0x38, 8: 0x7F}[a]]) if nn else bytes([0x26, a])
    if kind == 'bit':
        return bytes([0x32]) if nn else bytes([0x68, 1])
    if kind == 'float':
        return bytes([0x3B if a == 4 else 0x3E]) if nn else bytes([0x6D, a])
    if kind == 'money':
        return bytes([0x7A if a == 4 else 0x3C]) if nn else bytes([0x6E, a])
    if kind == 'datetime':
        return bytes([0x3A if a == 4 else 0x3D]) if nn else bytes([0x6F, a])
    if kind == 'numeric':
        return bytes([0x6C, numlen(a), a, b])
    if kind == 'guid':
        return bytes([0x24, 16])
    if kind in ('varchar', 'char'):
        return bytes([0xA7 if kind == 'varchar' else 0xAF]) + struct.pack('<H', a) + COLL
    if kind in ('nvarchar', 'nchar'):
        return bytes([0xE7 if kind == 'nvarchar' else 0xEF]) + struct.pack('<H', a * 2) + COLL
    if kind in ('varbinary', 'binary'):
        return bytes([0xA5 if kind == 'varbinary' else 0xAD]) + struct.pack('<H', a)
    if kind in ('text', 'ntext'):
        r = bytes([0x23 if kind == 'text' else 0x63]) + struct.pack('<I', 0x7FFFFFFF) + COLL
        return r + (us('tbl') if colmeta else b'')
    if kind == 'image':
        return bytes([0x22]) + struct.pack('<I', 0x7FFFFFFF) + (us('tbl') if colmeta else b'')
    if kind == 'variant':
        return bytes([0x62]) + struct.pack('<I', 8009)
    raise ValueError(kind)


def dt_parts(v):
    s = str(v).replace('T', ' ')
    if len(s) == 8 and s.isdigit():
        s = '%s-%s-%s' % (s[:4], s[4:6], s[6:])
    if len(s) == 10:
        s += ' 00:00:00'
    fmt = '%Y-%m-%d %H:%M:%S.%f' if '.' in s else '%Y-%m-%d %H:%M:%S'
    d = datetime.datetime.strptime(s, fmt)
    days = (d.date() - datetime.date(1900, 1, 1)).days
    secs = d.hour * 3600 + d.minute * 60 + d.second + d.microsecond / 1e6
    return days, secs


def value(t, v):
    kind, a, b, nn = t
    fixed = nn and kind in ('int', 'bit', 'float', 'money', 'datetime')
    if v is None:
        if fixed:
            raise ValueError('NULL in a NOT NULL column')
        if kind in ('varchar', 'char', 'nvarchar', 'nchar', 'varbinary', 'binary'):
            return b'\xff\xff'
        if kind == 'variant':
            return struct.pack('<I', 0)
        return b'\x00'
    if kind == 'int':
        d = int(v).to_bytes(a, 'little', signed=(a != 1))
    elif kind == 'bit':
        d = bytes([1 if v else 0])
    elif kind == 'float':
        d = struct.pack('<f' if a == 4 else '<d', float(v))
    elif kind == 'money':
        n = int((decimal.Decimal(str(v)) * 10000).to_integral_value())
        d = struct.pack('<i', n) if a == 4 else struct.pack('<iI', n >> 32, n & 0xFFFFFFFF)
    elif kind == 'datetime':
        days, secs = dt_parts(v)
        d = struct.pack('<HH', days, int(secs // 60)) if a == 4 else struct.pack('<iI', days, int(round(secs * 300)))
    elif kind == 'numeric':
        q = decimal.Decimal(str(v)).quantize(decimal.Decimal(1).scaleb(-b))
        n = int(abs(q).scaleb(b))
        return bytes([numlen(a), 0 if q < 0 else 1]) + n.to_bytes(numlen(a) - 1, 'little')
    elif kind == 'guid':
        return bytes([16]) + uuid.UUID(str(v)).bytes_le
    elif kind in ('varchar', 'char'):
        d = str(v).encode('cp1252', 'replace')
        if kind == 'char':
            d = d.ljust(a)[:a]
        return struct.pack('<H', len(d)) + d
    elif kind in ('nvarchar', 'nchar'):
        s = str(v)
        if kind == 'nchar':
            s = s.ljust(a)[:a]
        d = s.encode('utf-16-le')
        return struct.pack('<H', len(d)) + d
    elif kind in ('varbinary', 'binary'):
        d = bytes(v)
        return struct.pack('<H', len(d)) + d
    elif kind in ('text', 'ntext', 'image'):
        d = bytes(v) if kind == 'image' else str(v).encode('cp1252' if kind == 'text' else 'utf-16-le', 'replace')
        return bytes([16]) + b'\x01' * 16 + b'\x00' * 8 + struct.pack('<I', len(d)) + d
    elif kind == 'variant':
        if isinstance(v, int):
            return struct.pack('<IBB', 6, 0x38, 0) + struct.pack('<i', v)
        d = str(v).encode('utf-16-le')
        return struct.pack('<IBB', 9 + len(d), 0xE7, 7) + COLL + struct.pack('<H', 8000) + d
    else:
        raise ValueError(kind)
    return d if fixed else bytes([len(d)]) + d


def col_type(name, vals):
    """declared type of a column name, or a type guessed from the values"""
    t = TYPES.get(name.lower())
    if t:
        if t[3] and any(v is None for v in vals):
            return t[:3] + (False,)
        return t
    vals = [v for v in vals if v is not None]
    if vals and all(isinstance(v, int) for v in vals):
        return ('int', 8 if any(abs(v) > 0x7FFFFFFF for v in vals) else 4, 0, False)
    if vals and all(isinstance(v, (int, float)) for v in vals):
        return ('float', 8, 0, False)
    if vals and all(isinstance(v, bytes) for v in vals):
        return ('varbinary', 8000, 0, False)
    return ('nvarchar', 4000, 0, False)


def learn(sql):
    m = re.match(r'\s*create\s+(?:temp\s+)?table\s+\S+\s*\((.*)\)\s*$', sql, re.I | re.S)
    if not m:
        return
    for part in re.split(r',(?![^(]*\))', m.group(1)):
        c = re.match(r'\s*(\w+)\s+(\w+)(?:\s*\((\d+)(?:\s*,\s*(\d+))?\))?(.*)', part, re.S)
        if not c:
            continue
        name, typ, a, b, rest = c.group(1).lower(), c.group(2).lower(), c.group(3), c.group(4), c.group(5).lower()
        nn = 'not null' in rest or 'primary key' in rest
        a = int(a) if a else 0
        b = int(b) if b else 0
        t = {
            'int': ('int', 4, 0), 'integer': ('int', 4, 0), 'smallint': ('int', 2, 0), 'tinyint': ('int', 1, 0),
            'bigint': ('int', 8, 0), 'bit': ('bit', 1, 0), 'float': ('float', 8, 0), 'real': ('float', 4, 0),
            'money': ('money', 8, 0), 'smallmoney': ('money', 4, 0), 'datetime': ('datetime', 8, 0),
            'smalldatetime': ('datetime', 4, 0), 'numeric': ('numeric', a or 18, b), 'decimal': ('numeric', a or 18, b),
            'uniqueidentifier': ('guid', 16, 0), 'varchar': ('varchar', a or 1, 0), 'char': ('char', a or 1, 0),
            'nvarchar': ('nvarchar', a or 1, 0), 'nchar': ('nchar', a or 1, 0), 'varbinary': ('varbinary', a or 1, 0),
            'binary': ('binary', a or 1, 0), 'text': ('text', 0, 0), 'ntext': ('ntext', 0, 0), 'image': ('image', 0, 0),
            'sql_variant': ('variant', 0, 0),
        }.get(typ)
        if t:
            TYPES[name] = t + (nn,)


# ------------------------------------------------------------------ the server

class Conn:
    def __init__(self, sock):
        self.s = sock
        self.db = sqlite3.connect(':memory:', isolation_level=None, check_same_thread=False)
        # for the tests of values too long to look at: xdb_md5(value)
        self.db.create_function('xdb_md5', 1, lambda v: None if v is None else hashlib.md5(
            v if isinstance(v, bytes) else str(v).encode('utf-8')).hexdigest())
        self.in_txn = False
        self.cursors = {}
        self.nextcur = 180150000
        self.log = []

    def read_msg(self):
        data = b''
        while True:
            h = self.recv(8)
            if h is None:
                return None, None
            typ, status, length = struct.unpack('>BBH', h[:4])
            body = self.recv(length - 8) if length > 8 else b''
            if body is None:
                return None, None
            data += body
            if status & 1:
                return typ, data

    def recv(self, n):
        b = b''
        while len(b) < n:
            c = self.s.recv(n - len(b))
            if not c:
                return None
            b += c
        return b

    def send_msg(self, payload, typ=4):
        psize = 4096 - 8
        pid = 1
        while True:
            chunk, payload = payload[:psize], payload[psize:]
            self.s.sendall(struct.pack('>BBHHBB', typ, 0 if payload else 1, len(chunk) + 8, 0, pid & 0xFF, 0) + chunk)
            pid += 1
            if not payload:
                break

    # ---- SQL execution through sqlite
    def run(self, sql, params=()):
        low = sql.strip().lower()
        if 'xdb_deadlock' in low:
            if self.in_txn:
                self.db.execute('ROLLBACK')
                self.in_txn = False
            raise TdsError(1205, 13, 'Transaction (Process ID 55) was deadlocked on lock resources and has been chosen as the deadlock victim.')
        if re.match(r'set\s', low):
            return None, [], 0
        if low == 'select @@trancount':
            return ['c'], [(1 if self.in_txn else 0,)], 1
        if low == 'begin transaction':
            if not self.in_txn:
                self.db.execute('BEGIN')
            self.in_txn = True
            return None, [], 0
        m = re.match(r'if @@trancount > 0 (commit|rollback) transaction', low)
        if m:
            if self.in_txn:
                self.db.execute(m.group(1).upper())
            self.in_txn = False
            return None, [], 0
        learn(sql)
        q = re.sub(r'@P(\d+)', r'?\1', sql)
        q = re.sub(r'create\s+table\s+#', 'create temp table tmp_', q, flags=re.I)
        q = q.replace('#', 'tmp_')
        try:
            cur = self.db.execute(q, params)
        except sqlite3.IntegrityError as x:
            raise TdsError(2627, 14, "Violation of PRIMARY KEY constraint. Cannot insert duplicate key (%s)." % x)
        except sqlite3.OperationalError as x:
            if 'no such table' in str(x):
                raise TdsError(208, 16, "Invalid object name '%s'." % str(x).split(':')[-1].strip())
            if 'no such column' in str(x):
                raise TdsError(207, 16, "Invalid column name '%s'." % str(x).split(':')[-1].strip())
            if 'syntax error' in str(x):
                raise TdsError(102, 15, "Incorrect syntax: %s" % x)
            raise TdsError(50000, 16, str(x))
        except sqlite3.Error as x:
            raise TdsError(50000, 16, str(x))
        if cur.description is None:
            return None, [], max(cur.rowcount, 0)
        rows = cur.fetchall()
        return [d[0] for d in cur.description], rows, len(rows)

    def resultset(self, names, rows, with_rows=True):
        types = [col_type(n, [r[i] for r in rows]) for i, n in enumerate(names)]
        out = struct.pack('<BH', 0x81, len(names))
        for n, t in zip(names, types):
            out += struct.pack('<HH', 0, 0x0009 if not t[3] else 0x0008) + typeinfo(t) + bv(n)
        if with_rows:
            for r in rows:
                out += b'\xd1' + b''.join(value(t, v) for t, v in zip(types, r))
        return out

    # ---- requests
    def prelogin(self, data):
        self.log.append('PRELOGIN ' + data.hex())
        body = bytes([0, 0, 21, 0, 6, 1, 0, 27, 0, 1, 2, 0, 28, 0, 1, 3, 0, 29, 0, 0, 0xFF,
                      8, 0, 7, 0xF7, 0, 0, 2, 0])
        self.send_msg(body)

    def login(self, data):
        def field(i):
            off, ln = struct.unpack_from('<HH', data, 36 + 4 * i)
            return data[off:off + 2 * ln]
        user = field(1).decode('utf-16-le')
        pw = bytes((((b ^ 0xA5) << 4) & 0xF0) | ((b ^ 0xA5) >> 4) for b in field(2)).decode('utf-16-le')
        db = field(8).decode('utf-16-le')
        ver = struct.unpack_from('<I', data, 4)[0]
        self.log.append('LOGIN user=%s db=%s tds=%08X app=%s' % (user, db, ver, field(3).decode('utf-16-le')))
        if user != 'sa' or pw != 'xdbpw':
            self.send_msg(error(18456, 14, "Login failed for user '%s'." % user) + done(0xFD, 2))
            return False
        ack = struct.pack('<B', 1) + bytes([0x71, 0, 0, 1]) + bv('Microsoft SQL Server') + bytes([8, 0, 7, 0xF7])
        out = envchange(1, db or 'master', 'master') + envchange(7, COLL)
        out += struct.pack('<BH', 0xAD, len(ack)) + ack + envchange(4, '4096', '4096') + done(0xFD)
        self.send_msg(out)
        return True

    def batch(self, data):
        sql = data.decode('utf-16-le')
        self.log.append('BATCH ' + sql)
        try:
            names, rows, n = self.run(sql)
        except TdsError as x:
            self.send_msg(error(x.num, x.cls, x.msg) + done(0xFD, 2))
            return
        if names is None:
            self.send_msg(done(0xFD, 0x10, n))
        else:
            self.send_msg(self.resultset(names, rows) + done(0xFD, 0x10, n))

    def rpc_params(self, data, pos):
        """-> list of (name, status, value)"""
        out = []
        while pos < len(data):
            nl = data[pos]
            name = data[pos + 1:pos + 1 + 2 * nl].decode('utf-16-le')
            pos += 1 + 2 * nl
            status = data[pos]
            t = data[pos + 1]
            pos += 2
            if t == 0x26:
                pos += 1
                ln = data[pos]
                pos += 1
                v = int.from_bytes(data[pos:pos + ln], 'little', signed=True) if ln else None
                pos += ln
            elif t == 0x6D:         # FLTN
                pos += 1
                ln = data[pos]
                pos += 1
                v = struct.unpack('<d' if ln == 8 else '<f', data[pos:pos + ln])[0] if ln else None
                pos += ln
            elif t in (0x6C, 0x6A):  # NUMERICN / DECIMALN
                scale = data[pos + 2]
                pos += 3
                ln = data[pos]
                pos += 1
                if ln:
                    v = decimal.Decimal(int.from_bytes(data[pos + 1:pos + ln], 'little')).scaleb(-scale)
                    if data[pos] == 0:
                        v = -v
                else:
                    v = None
                pos += ln
            elif t == 0x6F:         # DATETIMN
                pos += 1
                ln = data[pos]
                pos += 1
                if ln == 8:
                    days, ticks = struct.unpack_from('<iI', data, pos)
                    d = datetime.datetime(1900, 1, 1) + datetime.timedelta(days=days, milliseconds=round(ticks * 1000 / 300))
                    v = d.strftime('%Y-%m-%d %H:%M:%S.') + '%03d' % (d.microsecond // 1000)
                else:
                    v = None
                pos += ln
            elif t in (0x38, 0x34, 0x30, 0x7F):
                ln = {0x38: 4, 0x34: 2, 0x30: 1, 0x7F: 8}[t]
                v = int.from_bytes(data[pos:pos + ln], 'little', signed=True)
                pos += ln
            elif t in (0xE7, 0xEF, 0xA7, 0xAF):
                pos += 2 + 5
                ln = struct.unpack_from('<H', data, pos)[0]
                pos += 2
                if ln == 0xFFFF:
                    v = None
                else:
                    v = data[pos:pos + ln].decode('utf-16-le' if t in (0xE7, 0xEF) else 'cp1252')
                    pos += ln
            elif t in (0x63, 0x23):
                pos += 4 + 5
                ln = struct.unpack_from('<I', data, pos)[0]
                pos += 4
                if ln == 0xFFFFFFFF:
                    v = None
                else:
                    v = data[pos:pos + ln].decode('utf-16-le' if t == 0x63 else 'cp1252')
                    pos += ln
            elif t == 0x22:         # IMAGE: maximum length, length, bytes
                pos += 4
                ln = struct.unpack_from('<I', data, pos)[0]
                pos += 4
                if ln == 0xFFFFFFFF:
                    v = None
                else:
                    v = bytes(data[pos:pos + ln])
                    if len(v) != ln:
                        raise TdsError(8009, 16, 'The incoming RPC parameter is shorter than its length says.')
                    pos += ln
            else:
                raise TdsError(8016, 16, 'The incoming RPC parameter has an unsupported type 0x%02X.' % t)
            out.append((name, status, v))
        return out

    @staticmethod
    def typed(defs, vals):
        """converts parameter values to the declared types"""
        out = []
        decl = [d for d in re.findall(r'@\w+\s+([a-z]+(?:\([^)]*\))?)', (defs or '').lower())]
        if len(decl) != len(vals):
            raise TdsError(8178, 16, 'Parameterized query expects %d parameters, %d were supplied.' % (len(decl), len(vals)))
        for d, v in zip(decl, vals):
            if v is None:
                out.append(None)
            elif isinstance(v, (int, float, bytes)):
                out.append(v)
            elif isinstance(v, decimal.Decimal):
                out.append(float(v))
            elif d in ('int', 'bigint', 'smallint'):
                try:
                    out.append(int(v))
                except ValueError:
                    raise TdsError(245, 16, "Conversion failed when converting the value '%s' to data type int." % v)
            elif d == 'float':
                out.append(float(v))
            elif d.startswith('decimal') or d.startswith('numeric'):
                try:
                    out.append(float(decimal.Decimal(v)))
                except decimal.InvalidOperation:
                    raise TdsError(8114, 16, 'Error converting data type nvarchar to numeric.')
            elif d == 'datetime':
                try:
                    days, secs = dt_parts(v)
                except ValueError:
                    raise TdsError(241, 16, 'Conversion failed when converting datetime from character string.')
                t = datetime.datetime(1900, 1, 1) + datetime.timedelta(days=days, seconds=secs)
                out.append(t.strftime('%Y-%m-%d %H:%M:%S.') + '%03d' % (t.microsecond // 1000))
            elif d.startswith('varchar') or d == 'text':
                out.append(str(v).encode('cp1252', 'replace').decode('cp1252'))
            else:
                out.append(v)
        return out

    def rpc(self, data):
        nl = struct.unpack_from('<H', data, 0)[0]
        if nl == 0xFFFF:        # well-known procedure by number
            pid = struct.unpack_from('<H', data, 2)[0]
            name = {2: 'sp_cursoropen', 7: 'sp_cursorfetch', 9: 'sp_cursorclose', 10: 'sp_executesql',
                    11: 'sp_prepare', 15: 'sp_unprepare'}.get(pid, 'procid_%d' % pid)
            nl = 1
        else:
            name = data[2:2 + 2 * nl].decode('utf-16-le').lower()
        try:
            p = self.rpc_params(data, 2 + 2 * nl + 2)
            self.log.append('RPC %s %r' % (name, [(n, s, v) for n, s, v in p]))
            vals = [v for _, _, v in p]
            out = b''
            rv = []
            if name == 'sp_executesql':
                names, rows, n = self.run(vals[0], self.typed(vals[1] if len(vals) > 1 else None, vals[2:]))
                if names is not None:
                    out += self.resultset(names, rows)
                out += done(0xFF, 0x11, n)
            elif name == 'sp_prepare':
                defs = vals[1]
                np = len(re.findall(r'@\w+\s', defs)) if defs else 0
                if not re.match(r'\s*(/\*.*?\*/)?\s*\(?\s*(select|with)\b', vals[2], re.I):
                    self.db.execute('EXPLAIN ' + re.sub(r'@P(\d+)', r'?\1', vals[2]).replace('#', 'tmp_'), [None] * np)
                    names = None
                else:
                    names, rows, n = self.run(vals[2], [None] * np)
                    # types of a described statement come from the declarations only
                if names is not None and vals[3]:
                    out += self.resultset(names, [], with_rows=False)
                rv = [1]
            elif name == 'sp_unprepare':
                pass
            elif name == 'sp_cursoropen':
                if '/*nocursor*/' in vals[1]:
                    raise TdsError(16937, 16, 'A server cursor cannot be opened on the given statement or statements.')
                names, rows, n = self.run(vals[1], self.typed(vals[5] if len(vals) > 5 else None, vals[6:]))
                if names is None:
                    raise TdsError(16945, 16, 'The cursor was not declared.')
                self.nextcur += 1
                self.cursors[self.nextcur] = [names, rows, 0]
                out += self.resultset(names, rows, with_rows=False) + done(0xFF, 0x11, 0)
                rv = [self.nextcur, vals[2], vals[3], -1]
            elif name == 'sp_cursorfetch':
                c = self.cursors.get(vals[0])
                if c is None:
                    raise TdsError(16909, 16, 'The cursor identifier value provided (%s) is not valid.' % vals[0])
                part = c[1][c[2]:c[2] + vals[3]]
                types_from = c[1]
                names = c[0]
                types = [col_type(n, [r[i] for r in types_from]) for i, n in enumerate(names)]
                out += struct.pack('<BH', 0x81, len(names))
                for n, t in zip(names, types):
                    out += struct.pack('<HH', 0, 9) + typeinfo(t) + bv(n)
                for r in part:
                    out += b'\xd1' + b''.join(value(t, v) for t, v in zip(types, r))
                c[2] += len(part)
                out += done(0xFF, 0x11, len(part))
            elif name == 'sp_cursorclose':
                if self.cursors.pop(vals[0], None) is None:
                    raise TdsError(16909, 16, 'The cursor identifier value provided (%s) is not valid.' % vals[0])
            else:
                raise TdsError(2812, 16, "Could not find stored procedure '%s'." % name)
            out += struct.pack('<Bi', 0x79, 0)
            for i, v in enumerate(rv):
                out += struct.pack('<BH', 0xAC, i) + bv('') + struct.pack('<BHH', 1, 0, 0) + bytes([0x26, 4, 4]) + struct.pack('<i', v)
            self.send_msg(out + done(0xFE))
        except TdsError as x:
            self.log.append('  -> error %d' % x.num)
            self.send_msg(error(x.num, x.cls, x.msg) + done(0xFF, 3) + struct.pack('<Bi', 0x79, 1) + done(0xFE, 2))
        except sqlite3.Error as x:
            self.send_msg(error(50000, 16, str(x)) + done(0xFE, 2))

    def serve(self):
        try:
            while True:
                typ, data = self.read_msg()
                if typ is None:
                    break
                if typ == 0x12:
                    self.prelogin(data)
                elif typ == 0x10:
                    if not self.login(data):
                        break
                elif typ == 1:
                    self.batch(data)
                elif typ == 3:
                    self.rpc(data)
                elif typ == 6:      # attention: acknowledge
                    self.send_msg(done(0xFD, 0x20))
                else:
                    self.log.append('unknown packet type %d' % typ)
                    break
        finally:
            with open('/tmp/tdsmock.log', 'a') as f:
                f.write('\n'.join(self.log) + '\n---\n')
            self.s.close()


class TdsError(Exception):
    def __init__(self, num, cls, msg):
        Exception.__init__(self, msg)
        self.num, self.cls, self.msg = num, cls, msg


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 14330
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('127.0.0.1', port))
    srv.listen(8)
    while True:
        s, _ = srv.accept()
        threading.Thread(target=Conn(s).serve, daemon=True).start()


if __name__ == '__main__':
    main()
