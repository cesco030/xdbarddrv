# QSH script: build the XDB driver objects.  usage: sh build_xdb.sh <kitdir> <lib> [<ref> [lobtest]]
# <ref> is how the programs name the library of the service programs they are bound to:
#   left out = <lib> (a build that is used where it is built), *LIBL = for a package (the Setup
#   display of WRKXDBPRF binds them to the library they are restored into).
# ALWLIBUPD(*YES) on every object lets that library be changed later with UPDPGM / UPDSRVPGM.
# lobtest: compiles the large object experiment in (DEFINE(XDB_LOBTEST), see lobt_load in
#   xdbard.c). Never for a library that is used for work or for a package.
# Needs the LIBPQ service program in <lib> (build_ile.sh + bind_ile.sh).
K=$1; LIB=$2; R=$3
if test -z "$R"; then R=$LIB; fi
DEF=""
if test "$4" = "lobtest"; then DEF="DEFINE(XDB_LOBTEST)"; fi
mkdir -p $K/out
B=$K/out/xdb.log
echo "start $(date)" > $B
if test -n "$DEF"; then echo "LARGE OBJECT EXPERIMENT COMPILED IN ($DEF): not a build for work" >> $B; fi
UTF="LOCALETYPE(*LOCALEUTF) TGTCCSID(1208) SYSIFCOPT(*IFS64IO) LANGLVL(*EXTENDED)"
INC="INCDIR('$K/xdb' '$K/pg/src/interfaces/libpq' '$K/pg/src/include' '$K/ile')"
MODS=""
for pair in XDBARDC:xdbard XDBLOB:xdblob XDBPG:xdbpg XDBMY:xdbmy XDBMS:xdbms XDBNET:xdbnet XDBTLS:xdbtls; do
  m=${pair%%:*}; f=${pair##*:}
  if system "CRTCMOD MODULE($LIB/$m) SRCSTMF('$K/xdb/$f.c') $UTF $INC $DEF OPTIMIZE(10) DBGVIEW(*NONE) OUTPUT(*PRINT) OPTION(*NOSHOWINC) FLAG(30) REPLACE(*YES)" > $K/out/$m.lst 2>&1; then
    echo "$m OK" >> $B; rm -f $K/out/$m.lst
  else
    echo "$m FAILED (see out/$m.lst)" >> $B
  fi
  MODS="$MODS $LIB/$m"
done
# profile and password access: plain compile (job CCSID), see xdbprfc.c
if system "CRTCMOD MODULE($LIB/XDBPRFC) SRCSTMF('$K/xdb/xdbprfc.c') TGTCCSID(*JOB) INCDIR('$K/xdb') $DEF DBGVIEW(*NONE) OUTPUT(*PRINT) OPTION(*NOSHOWINC) REPLACE(*YES)" > $K/out/XDBPRFC.lst 2>&1; then
  echo "XDBPRFC OK" >> $B; rm -f $K/out/XDBPRFC.lst
else
  echo "XDBPRFC FAILED (see out/XDBPRFC.lst)" >> $B
fi
MODS="$MODS $LIB/XDBPRFC"
# USRPRF(*OWNER): the driver reads the passwords in validation list XDBVLDL with its owner's authority
# ACTGRP(*CALLER): Db2 calls XDBARD in the default activation group, so the driver and its
# connections live there for the whole job. In a named activation group of its own it was
# deleted by RCLACTGRP ACTGRP(*ELIGIBLE) under the open connections (MCH3402 in XDBARD, SQL0969).
system "CRTSRVPGM SRVPGM($LIB/XDB) MODULE($MODS) EXPORT(*SRCFILE) SRCSTMF('$K/build/xdb.bnd') BNDSRVPGM(($R/LIBPQ) (QSYS/QSOSSLSR)) ACTGRP(*CALLER) ALWLIBUPD(*YES) USRPRF(*OWNER) TEXT('XDB - external DB driver (PG, MySQL, SQL Server)')" >> $B 2>&1 && echo "XDB SRVPGM OK" >> $B
# a replaced object keeps its old USRPRF value, so set it explicitly
system "CHGSRVPGM SRVPGM($LIB/XDB) USRPRF(*OWNER)" >> $B 2>&1 && echo "XDB SRVPGM adopts its owner (USRPRF *OWNER)" >> $B
system "CRTCMOD MODULE($LIB/XDBARDM) SRCSTMF('$K/xdb/xdbardm.c') $INC DBGVIEW(*NONE) OUTPUT(*PRINT) REPLACE(*YES)" > $K/out/XDBARDM.lst 2>&1 && echo "XDBARDM OK" >> $B
system "CRTPGM PGM($LIB/XDBARD) MODULE($LIB/XDBARDM) BNDSRVPGM(($R/XDB)) ACTGRP(*CALLER) ALWLIBUPD(*YES) TEXT('XDB - ARD exit program (RDB entries *ARDPGM)')" >> $B 2>&1 && echo "XDBARD PGM OK" >> $B
system "CRTCMOD MODULE($LIB/XDBCMTM) SRCSTMF('$K/xdb/xdbcmtm.c') $INC DBGVIEW(*NONE) OUTPUT(*PRINT) REPLACE(*YES)" > $K/out/XDBCMTM.lst 2>&1 && echo "XDBCMTM OK" >> $B
n=0
until system "CRTPGM PGM($LIB/XDBCMT) MODULE($LIB/XDBCMTM) BNDSRVPGM(($R/XDB)) ACTGRP(*CALLER) ALWLIBUPD(*YES) TEXT('XDB - commitment control exit program')" >> $B 2>&1; do
  n=$((n+1)); [ $n -ge 3 ] && { echo "XDBCMT PGM FAILED (in use? sign the jobs using XDB off and rerun)" >> $B; break; }
  sleep 10
done
[ $n -lt 3 ] && echo "XDBCMT PGM OK" >> $B
echo "end $(date)" >> $B
