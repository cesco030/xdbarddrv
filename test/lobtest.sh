#!/bin/sh
# Linux test of the file functions (xdb/xdblob.c, PostgreSQL backend): values between stream
# files and the server, through the emulation of the driver.
#   usage: sh lobtest.sh <emu dir with ardsh>     needs python3 and the PostgreSQL of the other
#   tests (127.0.0.1:5433, user postgres, password xdbpw).  Compare with lobtest.expected.
# Option lob_direct is 64 here, so that files of a few hundred KB already take the staged way
# (large objects of the server, pieces of 1 MB).  Work files: /tmp/xdb_lobt.
# An error with a file is SQLCODE -452 with the reason code in binary form in SQLERRMC (as
# message SQL0452 wants it); the text with the name of the file goes to the job log.
# XDB_LOBT_TRACE=Y writes the trace of the driver to /tmp/xdb_LOBPG.log.
E=$1
D=/tmp/xdb_lobt
P=$D/profile.emu
mkdir -p $D
export XDB_EMU_JOBCCSID=280 XDB_PRF_EMU=$P XDB_TEST_RDB=LOBPG XDB_CONF=/dev/null XDB_SH_MAXROWS=20
export XDB_EMU_JOBLOG=1		# what the driver would write to the job log comes as "JOBLOG ..."
{
  echo "P|LOBPG|PG|A|127.0.0.1|5433|postgres|postgres|*OFF|10|0|1000|6|${XDB_LOBT_TRACE:-N}"
  echo "W|LOBPG|xdbpw"
  echo "O|LOBPG|lob_direct|D|64"
} > $P
python3 - $D <<'PY'
import random, sys, os
d = sys.argv[1]
random.seed(20261114)
def bin(name, n):
    open(os.path.join(d, name), 'wb').write(bytes(random.getrandbits(8) for _ in range(n)))
bin('small.bin', 1000)          # in the statement
bin('edge.bin', 65536)          # exactly lob_direct: still in the statement
bin('over.bin', 65537)          # one byte more: staged
bin('big.bin', 3300000)         # staged, several pieces
open(os.path.join(d, 'empty.bin'), 'wb').close()
line = 'Riga àèìòù € ñ ü — 日本語 Ω\n'
open(os.path.join(d, 'small.txt'), 'w', encoding='utf-8').write(line * 3)
open(os.path.join(d, 'big.txt'), 'w', encoding='utf-8').write(line * 60000)
open(os.path.join(d, 'latin.txt'), 'w', encoding='utf-8').write('Città è perché\n' * 4)
for f in os.listdir(d):
    if f.startswith('out') or f.startswith('app'):
        os.remove(os.path.join(d, f))
os.makedirs(os.path.join(d, 'dir.bin'), exist_ok=True)
PY
run() { # title, then the requests on standard input
  echo "== $1"
  $E/ardsh 2>&1 | grep -E "^(>|AR|JOBLOG|  row|  col|  rows|  total)" | cut -c1-170
}
same() { if cmp -s "$D/$1" "$D/$2"; then echo "   $2 = $1"; else echo "   $2 DIFFERS from $1"; fi; }

run "values from files, then back to files" <<EOF2
ISO *CHG
C
X create temp table xdb_doc (id int primary key, body bytea, note text)
X insert into xdb_doc (id, body) values(1, get_blob_from_file('$D/small.bin'))
P s1 insert into xdb_doc (id, body, note) values(?, get_blob_from_file(?), get_clob_from_file(?))
E s1 i:2|$D/edge.bin|$D/small.txt
E s1 i:3|$D/over.bin|$D/big.txt
E s1 i:4|$D/big.bin|$D/latin.txt
E s1 i:5|$D/empty.bin|$D/empty.bin
Q select id, octet_length(body) as bytes, md5(body) as body_md5, length(note) as chars, md5(note) as note_md5 from xdb_doc order by id
Q select id, put_blob_to_file(body, '$D/out' || id || '.bin'), put_clob_to_file(note, '$D/out' || id || '.txt') as text_bytes from xdb_doc order by id
Q select count(*) as large_objects_left from pg_largeobject_metadata
ROLLBACK
D
EOF2
same small.bin out1.bin; same edge.bin out2.bin; same over.bin out3.bin; same big.bin out4.bin; same empty.bin out5.bin
same small.txt out2.txt; same big.txt out3.txt; same latin.txt out4.txt; same empty.bin out5.txt
ls $D/out1.txt 2>/dev/null || echo "   out1.txt was not written (the value is null)"

run "file options, CCSID of a text file, name in a host variable, a long column beside it" <<EOF2
ISO *CHG
C
X create temp table xdb_doc (id int primary key, body bytea, note text)
X insert into xdb_doc values(1, get_blob_from_file('$D/over.bin'), get_clob_from_file('$D/latin.txt'))
O c1 select put_blob_to_file(body, ?, 8) as new_file, repeat('x', 3000) as long_text from xdb_doc ## $D/out_new.bin
F c1
K c1
O c1 select put_blob_to_file(body, ?, 8) as new_file from xdb_doc ## $D/out_new.bin
F c1
K c1
Q select put_clob_to_file(note, '$D/app.txt', 32) as first, put_clob_to_file(note, '$D/app2.txt', 32, 1252) as cp1252 from xdb_doc
Q select put_clob_to_file(note, '$D/app.txt', 32) as second, put_clob_to_file(note, '$D/app2.txt', 32, 1252) as cp1252 from xdb_doc
Q select put_clob_to_file(note || repeat('è', 70000), '$D/out_euro.txt', 16, 1252) as staged_cp1252 from xdb_doc
ROLLBACK
D
EOF2
same over.bin out_new.bin
echo "   app.txt has $(wc -c < $D/app.txt) bytes, app2.txt $(wc -c < $D/app2.txt), out_euro.txt $(wc -c < $D/out_euro.txt)"
echo "   app2.txt as CP1252: $(iconv -f CP1252 -t UTF-8 $D/app2.txt | head -1)"

run "more shapes: markers after the file, many rows, an aggregate, bad text, the way of interactive SQL" <<EOF2
ISO *CHG
C
X create temp table xdb_doc (id int primary key, body bytea, note text)
X insert into xdb_doc (id) values(1)
X insert into xdb_doc (id) values(2)
P s1 update xdb_doc set body = get_blob_from_file(?), note = ? where id = ?
E s1 $D/over.bin|first|i:1
E s1 $D/small.bin|second|i:2
Q select id, octet_length(body) as bytes, note from xdb_doc order by id
Q select put_clob_to_file(i::text || chr(10), '$D/app_many.txt', 32) as n, repeat('y', 900) as filler from generate_series(1, 300) as g(i)
Q select put_clob_to_file(string_agg(i::text, ','), '$D/out_agg.txt') as agg_bytes, count(*) as n from generate_series(1, 30000) as g(i)
X insert into xdb_doc (id, note) values(3, get_clob_from_file('$D/over.bin'))
Q select count(*) as rows_after_bad_text, (select count(*) from pg_largeobject_metadata) as large_objects_left from xdb_doc
R select id, put_blob_to_file(body, '$D/out r ' || id || ' è.bin') as "bytes written" from xdb_doc order by id
ROLLBACK
D
EOF2
same over.bin "out r 1 è.bin"; same small.bin "out r 2 è.bin"
echo "   app_many.txt: $(wc -l < $D/app_many.txt) lines, in order: $(if seq 1 300 | cmp -s - $D/app_many.txt; then echo yes; else echo NO; fi)"
echo "   out_agg.txt: $(wc -c < $D/out_agg.txt) bytes, ends with $(tail -c 12 $D/out_agg.txt)"

run "what goes wrong: files" <<EOF2
ISO *CHG
C
X create temp table xdb_doc (id int primary key, body bytea, note text)
X insert into xdb_doc (id, body) values(1, get_blob_from_file('$D/nofile.bin'))
X insert into xdb_doc (id, body) values(1, get_blob_from_file('$D/dir.bin'))
X insert into xdb_doc (id, body) values(1, get_blob_from_file(''))
P s1 insert into xdb_doc (id, body) values(?, get_blob_from_file(?))
E s1 i:1|n:
E s1 i:1
X insert into xdb_doc (id, body) values(1, get_blob_from_file('$D/over.bin'))
X insert into xdb_doc (id, body) values(1, get_blob_from_file('$D/big.bin'))
Q select id, octet_length(body) as bytes from xdb_doc
Q select put_blob_to_file(body, '$D/nodir/x.bin') from xdb_doc
Q select put_blob_to_file(body, '$D/dir.bin') from xdb_doc
Q select put_blob_to_file(body, null) from xdb_doc
Q select put_blob_to_file(body, '$D/out_x.bin', 8), put_blob_to_file(body, '$D/nodir/y.bin') from xdb_doc
Q select count(*) as large_objects_left from pg_largeobject_metadata
ROLLBACK
D
EOF2
same over.bin out_x.bin
echo "   (out_x.bin is the first column of a row whose second column failed: complete, so it stays)"

run "what goes wrong: the statement" <<EOF2
ISO *CHG
C
X create temp table xdb_doc (id int primary key, body bytea, note text)
X insert into xdb_doc (id, note) values(1, 'x')
X select put_clob_to_file(note, '$D/out_i.txt') from xdb_doc
X update xdb_doc set note = put_clob_to_file(note, '$D/out_i.txt')
Q select *, put_clob_to_file(note, '$D/out_i.txt') from xdb_doc
Q select id from xdb_doc where body = get_blob_from_file('$D/small.bin')
X insert into xdb_doc (id, body) values(2, put_blob_to_file(body, 'x'))
X insert into xdb_doc select 9, put_blob_to_file(body, '$D/out_i.bin') from xdb_doc
Q select put_blob_to_file(note, '$D/out_i.bin') from xdb_doc
ROLLBACK
D
EOF2

run "no unit of work (isolation *NONE): small values only on the way out" <<EOF2
ISO *NONE
C
X create temp table xdb_doc (id int primary key, body bytea)
X insert into xdb_doc values(1, get_blob_from_file('$D/small.bin'))
X insert into xdb_doc values(2, get_blob_from_file('$D/big.bin'))
Q select id, octet_length(body) as bytes, md5(body) as body_md5 from xdb_doc order by id
Q select id, put_blob_to_file(body, '$D/out_n' || id || '.bin') from xdb_doc order by id
Q select count(*) as large_objects_left from pg_largeobject_metadata
X drop table xdb_doc
D
EOF2
same small.bin out_n1.bin
ls $D/out_n2.bin 2>/dev/null || echo "   out_n2.bin was not left behind"
echo "== large objects left on the server after everything: $(PGPASSWORD=xdbpw psql -h 127.0.0.1 -p 5433 -U postgres -Atc 'select count(*) from pg_largeobject_metadata')"
