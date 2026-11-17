#!/bin/sh
# Linux test of the file functions with the SQL Server backend (xdb/xdblob.c, xdb/xdbms.c):
# values between stream files and the server, through the emulation of the driver.
#   usage: sh lobtest_ms.sh <emu dir with ardsh>     needs python3, iconv and
#     - Babelfish for PostgreSQL (test/babelfish_build.sh): TDS on 127.0.0.1:14333, login
#       babelfish_user / xdbpw.  It is used at TDS 7.4 and at TDS 7.1;
#     - test/tdsmock.py on 127.0.0.1:14330 (login sa / xdbpw), which answers as SQL Server 2000.
#   Compare with lobtest_ms.expected.
# From TDS 7.2 on a file travels as a (max) parameter in chunks, at 7.1 as image or ntext with
# its length in front; a file column is written while its row is read from the line.
# Babelfish refuses image parameters (real servers take them): bytes at TDS 7.1 are therefore
# sent to the mock only, which python-tds has shown to read them as that client writes them.
# Babelfish 5.6 also loses its server process (in sp_cursoropen) when a cursor with (max)
# values was closed before its last row and more cursors follow on the connection: every fetch
# that is meant to fail has a connection of its own here, and the table is a real one.
# Work files: /tmp/xdb_lobs.  XDB_LOBT_TRACE=Y writes the traces to /tmp/xdb_LOBMS.log ...
E=$1
D=/tmp/xdb_lobs
P=$D/profile.emu
mkdir -p $D
export XDB_EMU_JOBCCSID=280 XDB_PRF_EMU=$P XDB_CONF=/dev/null XDB_SH_MAXROWS=20
export XDB_EMU_JOBLOG=1		# what the driver would write to the job log comes as "JOBLOG ..."
T=${XDB_LOBT_TRACE:-N}
{
  echo "P|LOBMS|MSSQL|A|127.0.0.1|14333|master|babelfish_user|*OFF|10|0|1000|6|$T"
  echo "W|LOBMS|xdbpw"
  echo "P|LOBM1|MSSQL|A|127.0.0.1|14333|master|babelfish_user|*OFF|10|0|1000|6|$T"
  echo "W|LOBM1|xdbpw"
  echo "O|LOBM1|tds|C|7.1"
  echo "P|LOBM0|MSSQL|A|127.0.0.1|14330|testdb|sa|*OFF|10|0|1000|6|$T"
  echo "W|LOBM0|xdbpw"
} > $P
python3 - $D <<'PY'
import random, sys, os
d = sys.argv[1]
random.seed(20261116)
def bin(name, n):
    open(os.path.join(d, name), 'wb').write(random.randbytes(n))
bin('small.bin', 1000)
bin('piece.bin', 32768)             # exactly one piece
bin('over.bin', 32769)              # one byte more
bin('big.bin', 20 * 1024 * 1024 + 123)
open(os.path.join(d, 'empty.bin'), 'wb').close()
line = 'Riga àèìòù € ñ ü — 日本語 Ω 😀 𝄞\n'
open(os.path.join(d, 'small.txt'), 'w', encoding='utf-8').write(line * 3)
open(os.path.join(d, 'big.txt'), 'w', encoding='utf-8').write('x' + line * 60000)   # pieces cut characters
open(os.path.join(d, 'latin.txt'), 'w', encoding='utf-8').write('Città è perché\n' * 4)
open(os.path.join(d, 'bad.txt'), 'wb').write(b'abc\xff\xfedef')
for f in os.listdir(d):
    if f.startswith('out') or f.startswith('app'):
        os.remove(os.path.join(d, f))
os.makedirs(os.path.join(d, 'dir.bin'), exist_ok=True)
PY
run() { # connection, title, then the requests on standard input
  echo "== $1: $2"
  XDB_TEST_RDB=$1 $E/ardsh 2>&1 | grep -E "^(>|AR|JOBLOG|  row|  col|  rows|  total)" | cut -c1-170
}
same() { if cmp -s "$D/$1" "$D/$2"; then echo "   $2 = $1"; else echo "   $2 DIFFERS from $1"; fi; }
md5() { echo "   md5 of $1: $(md5sum < $D/$1 | cut -c1-32 | tr a-f A-F)"; }
md5u() { echo "   md5 of $1 as UTF-16: $(iconv -f UTF-8 -t UTF-16LE $D/$1 | md5sum | cut -c1-32 | tr a-f A-F)"; }
MD5B="hashbytes('MD5', body)"
MD5N="hashbytes('MD5', cast(note as varbinary(max)))"
ID="cast(id as varchar(10))"

run LOBMS "TDS 7.4: values from files, then back to files" <<EOF2
ISO *CHG
C
X create table #xdb_doc (id int primary key, body varbinary(max), note nvarchar(max))
X insert into #xdb_doc (id, body) values(1, get_blob_from_file('$D/small.bin'))
P s1 insert into #xdb_doc (id, body, note) values(?, get_blob_from_file(?), get_clob_from_file(?))
E s1 i:2|$D/piece.bin|$D/small.txt
E s1 i:3|$D/over.bin|$D/big.txt
E s1 i:4|$D/big.bin|$D/latin.txt
E s1 i:5|$D/empty.bin|$D/empty.bin
Q select id, datalength(body) as bytes, $MD5B as body_md5, len(note) as chars, $MD5N as note_md5 from #xdb_doc order by id
Q select id, put_blob_to_file(body, '$D/out' + $ID + '.bin'), put_clob_to_file(note, '$D/out' + $ID + '.txt') as text_bytes from #xdb_doc order by id
ROLLBACK
D
EOF2
md5 small.bin; md5 piece.bin; md5 over.bin; md5 big.bin; md5u small.txt; md5u big.txt; md5u latin.txt
same small.bin out1.bin; same piece.bin out2.bin; same over.bin out3.bin; same big.bin out4.bin; same empty.bin out5.bin
same small.txt out2.txt; same big.txt out3.txt; same latin.txt out4.txt; same empty.bin out5.txt
ls $D/out1.txt 2>/dev/null || echo "   out1.txt was not written (the value is null)"

run LOBMS "TDS 7.4: file options, CCSID of a text file, names and values in host variables, many rows" <<EOF2
ISO *CHG
C
X create table #xdb_doc (id int primary key, body varbinary(max), note nvarchar(max))
X insert into #xdb_doc values(1, get_blob_from_file('$D/over.bin'), get_clob_from_file('$D/latin.txt'))
O c1 select put_blob_to_file(body, ?, 8) as new_file, replicate('x', 3000) as long_text from #xdb_doc ## $D/out_new.bin
F c1
K c1
Q select put_clob_to_file(note, '$D/app.txt', 32) as first, put_clob_to_file(note, '$D/app2.txt', 32, 1252) as cp1252 from #xdb_doc
Q select put_clob_to_file(note, '$D/app.txt', 32) as second, put_clob_to_file(note, '$D/app2.txt', 32, 1252) as cp1252 from #xdb_doc
Q select put_clob_to_file(note + replicate(cast(nchar(232) as nvarchar(max)), 70000), '$D/out_euro.txt', 16, 1252) as long_cp1252 from #xdb_doc
Q select put_blob_to_file(?, ? + '.bin') as from_markers, ? as third ## bytes of a host variable|$D/out_marker|3
X create table #n (i int primary key)
X declare @i int; set @i = 1; while @i <= 300 begin insert into #n values(@i); set @i = @i + 1 end
Q select put_clob_to_file(cast(i as varchar(10)) + char(10), '$D/app_many.txt', 32) as n, replicate('y', 900) as filler from #n order by i
P s1 update #xdb_doc set body = get_blob_from_file(?), note = ? where id = ?
E s1 $D/small.bin|second|i:1
Q select id, datalength(body) as bytes, note from #xdb_doc
R select id, put_blob_to_file(body, '$D/out r ' + $ID + ' ' + nchar(232) + '.bin') as "bytes written" from #xdb_doc order by id
ROLLBACK
D
ISO *NONE
C
X create table #xdb_doc2 (id int primary key, body varbinary(max))
X insert into #xdb_doc2 (id, body) values(1, get_blob_from_file('$D/over.bin'))
Q select put_blob_to_file(body, '$D/out_none.bin') as without_commit from #xdb_doc2
X drop table #xdb_doc2
D
EOF2
run LOBMS "TDS 7.4: option 8 and a file that exists" <<EOF2
ISO *NONE
C
O c1 select put_blob_to_file(?, ?, 8) as new_file ## abc|$D/out_new.bin
F c1
K c1
D
EOF2
same over.bin out_new.bin; same small.bin "out r 1 è.bin"; same over.bin out_none.bin
echo "   app.txt has $(wc -c < $D/app.txt) bytes, app2.txt $(wc -c < $D/app2.txt), out_euro.txt $(wc -c < $D/out_euro.txt)"
echo "   app2.txt as CP1252: $(iconv -f CP1252 -t UTF-8 $D/app2.txt | head -1)"
echo "   out_marker.bin: $(cat $D/out_marker.bin)"
echo "   app_many.txt: $(wc -l < $D/app_many.txt) lines, in order: $(if seq 1 300 | cmp -s - $D/app_many.txt; then echo yes; else echo NO; fi)"

run LOBMS "TDS 7.4: what goes wrong with a file that is read, and with the statement" <<EOF2
ISO *CHG
C
X if object_id('xdb_lobt') is not null drop table xdb_lobt
X create table xdb_lobt (id int primary key, body varbinary(max), note nvarchar(max))
X insert into xdb_lobt (id, body) values(1, get_blob_from_file('$D/nofile.bin'))
X insert into xdb_lobt (id, body) values(1, get_blob_from_file('$D/dir.bin'))
X insert into xdb_lobt (id, body) values(1, get_blob_from_file(''))
X insert into xdb_lobt (id, note) values(8, get_clob_from_file('$D/bad.txt'))
P s1 insert into xdb_lobt (id, body) values(?, get_blob_from_file(?))
E s1 i:1|$D/over.bin
E s1 i:1|$D/small.bin
E s1 i:2|$D/small.bin
Q select id, datalength(body) as bytes, $MD5B as body_md5 from xdb_lobt order by id
X select put_clob_to_file(note, '$D/out_i.txt') from xdb_lobt
Q select put_blob_to_file(body, '$D/out_u.bin') from xdb_lobt union all select body from xdb_lobt
Q select id from xdb_lobt where body = get_blob_from_file('$D/small.bin')
COMMIT
D
EOF2
md5 over.bin; md5 small.bin
ls $D/out_i.txt $D/out_u.bin 2>/dev/null | sed 's/^/   left behind: /'

bad() { # title, the query that fails when its row is fetched
  run LOBMS "TDS 7.4: what goes wrong with a file that is written: $1" <<EOF2
ISO *CHG
C
$2
ROLLBACK
D
EOF2
}
bad "no such directory" "Q select put_blob_to_file(body, '$D/nodir/x.bin') from xdb_lobt where id = 1"
bad "a directory" "Q select put_blob_to_file(body, '$D/dir.bin') from xdb_lobt where id = 1"
bad "no name" "Q select put_blob_to_file(body, null) from xdb_lobt where id = 1"
bad "a name too long" "Q select put_blob_to_file(body, replicate('a', 2000)) from xdb_lobt where id = 1"
bad "the second file of a row" "Q select put_blob_to_file(body, '$D/out_x.bin', 8), put_blob_to_file(body, '$D/nodir/y.bin') from xdb_lobt where id = 1"
bad "the file exists (it is the one the test before wrote)" "Q select put_blob_to_file(body, '$D/out_x.bin', 8) as new_file from xdb_lobt where id = 1"
same over.bin out_x.bin
echo "   (out_x.bin is the first column of a row whose second column failed: complete, so it stays)"
run LOBMS "TDS 7.4: the connections after those work; the table of the test is dropped" <<EOF2
ISO *CHG
C
Q select id, put_blob_to_file(body, '$D/out_y' + $ID + '.bin') as b from xdb_lobt order by id
X drop table xdb_lobt
COMMIT
D
EOF2
same over.bin out_y1.bin; same small.bin out_y2.bin

run LOBM1 "TDS 7.1: text as ntext to the server, values back the way that level sends them" <<EOF2
ISO *CHG
C
X create table #xdb_doc (id int primary key, body varbinary(max), note nvarchar(max))
P s1 insert into #xdb_doc (id, note) values(?, get_clob_from_file(?))
E s1 i:2|$D/small.txt
E s1 i:3|$D/big.txt
E s1 i:5|$D/empty.bin
X update #xdb_doc set body = cast(note as varbinary(max)) where id = 3
X insert into #xdb_doc (id, body) values(6, cast(replicate(cast('0123456789' as varchar(max)), 300000) as varbinary(max)))
Q select id, datalength(body) as bytes, len(note) as chars, $MD5N as note_md5 from #xdb_doc order by id
Q select id, put_blob_to_file(body, '$D/out71_' + $ID + '.bin'), put_clob_to_file(note, '$D/out71_' + $ID + '.txt') as text_bytes from #xdb_doc order by id
Q select put_blob_to_file(body, '$D/out71_again.bin') as once_more from #xdb_doc where id = 6
Q select put_blob_to_file(body, '$D/nodir/x.bin') from #xdb_doc where id = 6
ROLLBACK
D
EOF2
md5u small.txt; md5u big.txt
same small.txt out71_2.txt; same big.txt out71_3.txt; same empty.bin out71_5.txt
iconv -f UTF-8 -t UTF-16LE $D/big.txt | cmp -s - $D/out71_3.bin && echo "   out71_3.bin = big.txt as UTF-16" || echo "   out71_3.bin DIFFERS from big.txt as UTF-16"
python3 - $D <<'PY'
import sys, os
for f in ('out71_6.bin', 'out71_again.bin'):
    d = open(os.path.join(sys.argv[1], f), 'rb').read()
    print('   %s: %d bytes, the digits repeated: %s' % (f, len(d), d == b'0123456789' * 300000))
PY

run LOBM0 "the mock as SQL Server 2000 (TDS 7.1): bytes as image, text as ntext; no way back" <<EOF2
ISO *CHG
C
X create table xdb_doc (id int primary key, body image, note ntext)
X insert into xdb_doc (id, body) values(1, get_blob_from_file('$D/small.bin'))
P s1 insert into xdb_doc (id, body, note) values(?, get_blob_from_file(?), get_clob_from_file(?))
E s1 i:2|$D/piece.bin|$D/small.txt
E s1 i:3|$D/over.bin|$D/big.txt
E s1 i:4|$D/big.bin|$D/latin.txt
E s1 i:5|$D/empty.bin|$D/empty.bin
E s1 i:5|$D/small.bin|$D/small.txt
Q select id, length(body) as bytes, xdb_md5(body) as body_md5, length(note) as chars, xdb_md5(note) as note_md5 from xdb_doc order by id
Q select put_blob_to_file(body, '$D/out_2000.bin') from xdb_doc
X drop table xdb_doc
ROLLBACK
D
EOF2
md5 small.bin; md5 piece.bin; md5 over.bin; md5 big.bin; md5 small.txt; md5 big.txt; md5 latin.txt
echo "   (the mock shows them in lower case)"
echo "   files in $D beginning with out: $(ls $D | grep -c '^out')"
