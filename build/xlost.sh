# QSH script: sample and test XDBLOST - what COMMIT answers when the connection to a server
# ended inside a unit of work that also changed the local database.  The program makes a
# session end by itself (the server is told to close a session that waits 2 seconds) and then
# commits; every case says RIGHT or WRONG.
# usage: sh xlost.sh <kitdir> <lib> <PostgreSQL rdb> <MySQL rdb> [<database>]
#   The names are connection profiles with their RDB directory entries; *NONE leaves a server
#   out.  With both, the program runs three times: each server alone, then both together.
#   database: only for MySQL when its connection has no database: where the temporary table
#   is kept; left out, the first database of the server that takes one.
#   On the servers the program makes one temporary table each and sets two settings of its own
#   session; nothing stays.
#   In <lib> the script makes, and removes at the end: table XDBLOSTC with journal XDBLOSTJ
#   and receiver XDBLOSTR1.  The program XDBLOST stays in <lib>.
#   result: /tmp/xdblost.out    log: <kitdir>/out/xlost.log    It takes about two minutes.
K=$1; LIB=$2; PG=$3; MY=$4; DB=$5
mkdir -p $K/out
L=$K/out/xlost.log
O=/tmp/xdblost.out
MSG="^(SQL|CPF|CPD|MCH|RNS|RNX|RNQ)[0-9A-F]{4}"
echo "start $(date)" > $L
rm -f $O
system "CRTJRNRCV JRNRCV($LIB/XDBLOSTR1)" >> $L 2>&1
system "CRTJRN JRN($LIB/XDBLOSTJ) JRNRCV($LIB/XDBLOSTR1)" >> $L 2>&1
system "RUNSQL SQL('create table $LIB.XDBLOSTC (ID integer, TXT varchar(40))') COMMIT(*NONE) NAMING(*SQL)" >> $L 2>&1
system "STRJRNPF FILE($LIB/XDBLOSTC) JRN($LIB/XDBLOSTJ) IMAGES(*BOTH) OMTJRNE(*OPNCLO)" >> $L 2>&1
run() {
  echo "--- CALL XDBLOST $1 $2" >> $L
  system "CALL PGM($LIB/XDBLOST) PARM('$1' '$2' '$LIB' '$O' '$DB')" 2>&1 | grep -E "$MSG|XDB " | head -60 >> $L
}
if system "CRTSQLRPGI OBJ($LIB/XDBLOST) SRCSTMF('$K/test/xdblost.sqlrpgle') COMMIT(*CHG) OPTION(*SQL) RDB(*LOCAL) CVTCCSID(*JOB) DBGVIEW(*NONE) REPLACE(*YES) TEXT('XDB sample: a connection is lost before COMMIT')" > $K/out/XDBLOST.lst 2>&1; then
  echo "XDBLOST compiled" >> $L
  if test "$PG" != "*NONE"; then run "$PG" "*NONE"; fi
  if test "$MY" != "*NONE"; then run "*NONE" "$MY"; fi
  if test "$PG" != "*NONE" && test "$MY" != "*NONE"; then run "$PG" "$MY"; fi
else
  echo "XDBLOST COMPILE FAILED (see out/XDBLOST.lst)" >> $L
  grep -E "SQL[0-9][0-9][0-9][0-9] |RNF[0-9][0-9][0-9][0-9] |MSG ID|$MSG" $K/out/XDBLOST.lst | head -40 >> $L
fi
echo "--- removing the local table and the journal" >> $L
system "ENDJRNPF FILE($LIB/XDBLOSTC)" >> $L 2>&1
system "DLTF FILE($LIB/XDBLOSTC)" >> $L 2>&1
system "DLTJRN JRN($LIB/XDBLOSTJ)" >> $L 2>&1
system "DLTJRNRCV JRNRCV($LIB/XDBLOSTR1) DLTOPT(*IGNINQMSG)" >> $L 2>&1
echo "end $(date)" >> $L
