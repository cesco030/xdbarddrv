# QSH script: bind LIBPQ and build the smoke test.  usage: sh bind_ile.sh <kitdir> <lib>
K=$1; LIB=$2
mkdir -p $K/out
B=$K/out/bind.log
echo "start $(date)" > $B
system "CRTSRVPGM SRVPGM($LIB/LIBPQ) MODULE($LIB/PQ*) EXPORT(*SRCFILE) SRCSTMF('$K/build/libpq.bnd') ACTGRP(*CALLER) OPTION(*DUPPROC) TEXT('libpq 18.7 ILE port (PostgreSQL client)')" >> $B 2>&1 && echo "SRVPGM OK" >> $B
system "CRTCMOD MODULE($LIB/ZPQTEST) SRCSTMF('$K/test/pqtest.c') LOCALETYPE(*LOCALEUTF) TGTCCSID(1208) SYSIFCOPT(*IFS64IO) LANGLVL(*EXTENDED) INCDIR('$K/pg/src/interfaces/libpq' '$K/pg/src/include' '$K/ile') DBGVIEW(*NONE) OUTPUT(*NONE) REPLACE(*YES)" >> $B 2>&1 && echo "TESTMOD OK" >> $B
system "CRTPGM PGM($LIB/PQTEST) MODULE($LIB/ZPQTEST) BNDSRVPGM(($LIB/LIBPQ)) ACTGRP(*NEW) TEXT('libpq ILE smoke test')" >> $B 2>&1 && echo "TESTPGM OK" >> $B
echo "end $(date)" >> $B
