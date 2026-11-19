# QSH script: tool XDBRUN - the statements of a stream file through the driver, as a program
# would send them, and what came back: SQLCODE and SQLSTATE of every step, the columns Db2
# was told, the rows as text.  For trying a statement of the server before it goes into a
# program, and for showing what the driver does with it.
# usage: sh xrun.sh <kitdir> <lib> <rdb> <script file>
#   <rdb>          a connection profile with its RDB directory entry
#   <script file>  one statement a line:
#                    X <statement>          EXECUTE IMMEDIATE
#                    X <statement> ## a|b   PREPARE, EXECUTE with the values as parameters
#                    Q <query> [## a|b]     PREPARE, DESCRIBE, OPEN, FETCH (60 rows at most)
#                    D                      DESCRIBE of the statement of the last Q once more
#                    C   COMMIT             R   ROLLBACK
#                    # a comment            = a line for the output
#                  The statements are in the language of the server; the program runs under
#                  commitment control and ends with a ROLLBACK, so a script that is to keep
#                  what it did says C.
#   result: /tmp/xrun_<rdb>.out    log: <kitdir>/out/xrun.log
#   The program XDBRUN stays in <lib>.
K=$1; LIB=$2; RDB=$3; S=$4
mkdir -p $K/out
L=$K/out/xrun.log
O=/tmp/xrun_$RDB.out
I=/tmp/xrun_$RDB.sql
MSG="^(SQL|CPF|CPD|MCH|RNS|RNX|RNQ)[0-9A-F]{4}"
echo "start $(date)" > $L
if test -z "$S" || test ! -f "$S"; then echo "usage: sh xrun.sh <kitdir> <lib> <rdb> <script file>" >> $L; cat $L; exit 1; fi
if system "CRTSQLRPGI OBJ($LIB/XDBRUN) SRCSTMF('$K/test/xdbrun.sqlrpgle') COMMIT(*CHG) OPTION(*SQL) RDB(*LOCAL) CVTCCSID(*JOB) DBGVIEW(*NONE) REPLACE(*YES) TEXT('XDB tool: statements of a file via the driver')" > $K/out/XDBRUN.lst 2>&1; then
  echo "XDBRUN compiled" >> $L
  rm -f $O $I
  cp "$S" $I
  system "CALL PGM($LIB/XDBRUN) PARM('$RDB' '$I' '$O')" 2>&1 | grep -E "$MSG|XDB " | head -30 >> $L
  if test -s $O; then echo "$O written, $(wc -l < $O) lines" >> $L; else echo "NO OUTPUT for $RDB" >> $L; fi
  rm -f $I
else
  echo "XDBRUN COMPILE FAILED (see out/XDBRUN.lst)" >> $L
  grep -E "SQL[0-9][0-9][0-9][0-9] |RNF[0-9][0-9][0-9][0-9] |MSG ID|$MSG" $K/out/XDBRUN.lst | head -40 >> $L
fi
echo "end $(date)" >> $L
