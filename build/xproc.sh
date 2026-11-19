# QSH script: sample XDBPROC - stored procedures of a MySQL (or MariaDB) server from an RPG
# program: values in by parameter markers, a value out through a variable of the session, the
# rows of a procedure through a cursor on its CALL, and through a temporary table.
# usage: sh xproc.sh <kitdir> <lib> <rdb> <database> [<steps>]
#   <rdb>       a connection profile of a MySQL server with its RDB directory entry
#   <database>  the database to work in: the program creates three procedures named
#               xdb_demo_... there and drops them again, so name one made for trying things.
#               *TEST = the database of the connection, when its name has "test" in it.
#   <steps>     letters: I look, C create the procedures, R run them, D drop them
#               (default CRD)
#   result: /tmp/xdbp_<rdb>.out    log: <kitdir>/out/xproc.log
#   The program XDBPROC stays in <lib>.
K=$1; LIB=$2; RDB=$3; DB=$4; ST=${5:-CRD}
mkdir -p $K/out
L=$K/out/xproc.log
O=/tmp/xdbp_$RDB.out
MSG="^(SQL|CPF|CPD|MCH|RNS|RNX|RNQ)[0-9A-F]{4}"
echo "start $(date)" > $L
if test -z "$DB"; then echo "usage: sh xproc.sh <kitdir> <lib> <rdb> <database> [<steps>]" >> $L; cat $L; exit 1; fi
if system "CRTSQLRPGI OBJ($LIB/XDBPROC) SRCSTMF('$K/test/xdbproc.sqlrpgle') COMMIT(*CHG) OPTION(*SQL) RDB(*LOCAL) CVTCCSID(*JOB) DBGVIEW(*NONE) REPLACE(*YES) TEXT('XDB sample: stored procedures of a MySQL server')" > $K/out/XDBPROC.lst 2>&1; then
  echo "XDBPROC compiled" >> $L
  rm -f $O
  system "CALL PGM($LIB/XDBPROC) PARM('$RDB' '$DB' '$ST' '$O')" 2>&1 | grep -E "$MSG|XDB " | head -20 >> $L
  if test -s $O; then echo "$O written, $(wc -l < $O) lines" >> $L; else echo "NO OUTPUT for $RDB" >> $L; fi
else
  echo "XDBPROC COMPILE FAILED (see out/XDBPROC.lst)" >> $L
  grep -E "SQL[0-9][0-9][0-9][0-9] |RNF[0-9][0-9][0-9][0-9] |MSG ID|$MSG" $K/out/XDBPROC.lst | head -40 >> $L
fi
echo "end $(date)" >> $L
