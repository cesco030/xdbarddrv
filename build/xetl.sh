# QSH script: sample and test XDBETL - one RPG program that holds a PostgreSQL, a MySQL and a
# SQL Server connection and the local database at the same time, copies rows from each server
# into local tables in several ways with their times, and commits and rolls back on all of
# them with one statement.
# usage: sh xetl.sh <kitdir> <lib> <PostgreSQL rdb> <MySQL rdb> <SQL Server rdb> [<rows> [<database>]]
#   The three names are connection profiles with their RDB directory entries; *NONE leaves a
#   server out.  rows: how many rows each copy has, 1 to 100000, 20000 by default.
#   database: only for MySQL when its connection has no database: where the temporary table of
#   part 2 is kept; left out, the first database of the server that takes one.
#   On the servers the program makes one temporary table each (200 rows); the rows it copies
#   are made by the servers themselves (no table is read).
#   In <lib> the script makes, and removes at the end: table XDBETLT (target of the copies),
#   table XDBETLC with journal XDBETLJ and receiver XDBETLR1 (the local side of part 2).
#   The program XDBETL stays in <lib>.
#   result: /tmp/xdbetl.out    log: <kitdir>/out/xetl.log
#   Read the result: every local table must say "as sent"; after the COMMIT every table has
#   200 rows, after the ROLLBACK still 200.
K=$1; LIB=$2; PG=$3; MY=$4; MS=$5; ROWS=${6:-20000}; DB=$7
mkdir -p $K/out
L=$K/out/xetl.log
O=/tmp/xdbetl.out
MSG="^(SQL|CPF|CPD|MCH|RNS|RNX|RNQ)[0-9A-F]{4}"
echo "start $(date)" > $L
rm -f $O
COLS="ID integer not null, D date not null, AMT decimal(11, 2) not null, CODE char(10) not null, TXT varchar(40) not null"
system "RUNSQL SQL('create table $LIB.XDBETLT ($COLS) rcdfmt XDBETLR') COMMIT(*NONE) NAMING(*SQL)" >> $L 2>&1
system "CRTJRNRCV JRNRCV($LIB/XDBETLR1)" >> $L 2>&1
system "CRTJRN JRN($LIB/XDBETLJ) JRNRCV($LIB/XDBETLR1)" >> $L 2>&1
system "RUNSQL SQL('create table $LIB.XDBETLC (ID integer, TXT varchar(40))') COMMIT(*NONE) NAMING(*SQL)" >> $L 2>&1
system "STRJRNPF FILE($LIB/XDBETLC) JRN($LIB/XDBETLJ) IMAGES(*BOTH) OMTJRNE(*OPNCLO)" >> $L 2>&1
if system "CRTSQLRPGI OBJ($LIB/XDBETL) SRCSTMF('$K/test/xdbetl.sqlrpgle') COMMIT(*CHG) OPTION(*SQL) RDB(*LOCAL) CVTCCSID(*JOB) DBGVIEW(*NONE) REPLACE(*YES) TEXT('XDB sample: three servers and Db2, one program')" > $K/out/XDBETL.lst 2>&1; then
  echo "XDBETL compiled" >> $L
  system "CALL PGM($LIB/XDBETL) PARM('$PG' '$MY' '$MS' '$ROWS' '$LIB' '$O' '$DB')" 2>&1 | grep -E "$MSG|XDB " | head -40 >> $L
else
  echo "XDBETL COMPILE FAILED (see out/XDBETL.lst)" >> $L
  grep -E "SQL[0-9][0-9][0-9][0-9] |RNF[0-9][0-9][0-9][0-9] |MSG ID|$MSG" $K/out/XDBETL.lst | head -40 >> $L
fi
echo "--- removing the local tables and the journal" >> $L
system "ENDJRNPF FILE($LIB/XDBETLC)" >> $L 2>&1
system "DLTF FILE($LIB/XDBETLC)" >> $L 2>&1
system "DLTF FILE($LIB/XDBETLT)" >> $L 2>&1
system "DLTJRN JRN($LIB/XDBETLJ)" >> $L 2>&1
system "DLTJRNRCV JRNRCV($LIB/XDBETLR1) DLTOPT(*IGNINQMSG)" >> $L 2>&1
echo "end $(date)" >> $L
