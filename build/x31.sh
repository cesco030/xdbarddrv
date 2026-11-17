# QSH script: one large value between a stream file and a PostgreSQL database (XDBT31), to
# see that the size does not matter to the job: above option lob_direct the value passes
# through a large object of the server in pieces of 1 MB.
# usage: sh x31.sh <kitdir> <lib> <rdb> <rows> [<file>]
#   rows: the value has 16 bytes per row (1310720 = 20 MB, 6553600 = 100 MB).  The server
#   makes it, so its memory has to hold a few times that.  Twice the size is written to and
#   deleted from the large objects of the server in the unit of work, which is rolled back.
#   file: default /tmp/xdbt31.bin (it stays).   result: /tmp/xdbt31.out   log: <kitdir>/out/x31.log
K=$1; LIB=$2; RDB=$3; ROWS=$4; F=${5:-/tmp/xdbt31.bin}
mkdir -p $K/out
L=$K/out/x31.log
O=/tmp/xdbt31.out
echo start > $L
rm -f $O $F
if ! system "CRTSQLRPGI OBJ($LIB/XDBT31) SRCSTMF('$K/test/xdbt31.sqlrpgle') COMMIT(*CHG) OPTION(*SQL) RDB(*LOCAL) CVTCCSID(*JOB) DBGVIEW(*NONE) REPLACE(*YES) TEXT('XDB test: one large value, file and server')" > $K/out/XDBT31.lst 2>&1; then
  echo "XDBT31 COMPILE FAILED (see out/XDBT31.lst)" >> $L
  grep -E "SQL[0-9][0-9][0-9][0-9] |RNF[0-9][0-9][0-9][0-9] " $K/out/XDBT31.lst | head -25 >> $L
  echo end >> $L; exit 1
fi
echo "XDBT31 compiled" >> $L
system "CALL PGM($LIB/XDBT31) PARM('$RDB' '$F' '$ROWS' '$O')" 2>&1 | grep -E "^(SQL|CPF|CPD|MCH|RNX|RNQ)[0-9A-F]{4}" | head -20 >> $L
echo "   file: $(ls -l $F | awk '{print $5}') bytes, CCSID $(attr $F CCSID)" >> $O
echo end >> $L
