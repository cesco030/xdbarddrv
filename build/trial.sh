# QSH script: trial of a package - restore the objects save file into another library and run
# the setup there in batch (program XDBT19 of <pgmlib>, the batch driver of module XDBSETUP).
#   usage: sh trial.sh <kitdir> <objects.savf> <savedlib> <triallib> <pgmlib> <mode> [<owner>]
#     mode RESTORE  restore and show the checks, change nothing
#          SETUP    run what is to do, except the owner profile and the ownership
#          OWNER    as SETUP, and give driver and password list to <owner> (must exist)
#          QDFTOWN  give the restored objects to the default owner QDFTOWN, as a restore does
#                   on a system that does not have the profile that built the package; run
#                   between RESTORE and SETUP, so that the setup has that line to do
#          CLEAN    delete the objects of the trial from <triallib>
# Run it in a job that does NOT have <triallib> in its library list: that is the point of the
# test with the driver programs.
#   log: <kitdir>/out/trial.log, setup output /tmp/xdbt19.out
K=$1; F=$2; BL=$3; T=$4; P=$5; MODE=$6; OWN=$7
if test -z "$OWN"; then OWN=" "; fi
L=$K/out/trial.log
X=$K/out/trial.tmp
echo "trial $MODE in $T, $(date)" > $L
t1() { # label, command
  if system "$2" > $X 2>&1; then echo "$1 -> OK" >> $L; return 0; fi
  echo "$1 -> FAILED" >> $L
  grep -E "CPF[0-9A-F]{4}|CPD[0-9A-F]{4}|CPC[0-9A-F]{4}" $X | head -5 >> $L
  return 1
}
bindings() {
  for o in XDBARD XDBCMT; do
    system "DSPPGM PGM($T/$o) DETAIL(*SRVPGM)" > $X 2>&1
    echo "  $1: $o -> $(grep -E ' XDB ' $X | sed 's/  */ /g' | cut -c1-22)" >> $L
  done
  system "DSPSRVPGM SRVPGM($T/XDB) DETAIL(*SRVPGM)" > $X 2>&1
  echo "  $1: XDB -> $(grep -E ' LIBPQ ' $X | sed 's/  */ /g' | cut -c1-22)" >> $L
}
activation() { # the driver programs return at once when called without parameters: the call
  # only shows whether their service programs are found from a job without <triallib> in its list
  for o in XDBARD XDBCMT; do
    if system "CALL PGM($T/$o)" > $X 2>&1; then echo "  $1: CALL $T/$o -> starts" >> $L
    else echo "  $1: CALL $T/$o -> does not start: $(grep -E 'MCH[0-9]{4}|CPF[0-9A-F]{4}|CPD[0-9A-F]{4}|CEE[0-9]{4}' $X | head -1 | cut -c1-110)" >> $L; fi
  done
}
case $MODE in
RESTORE)
  t1 "save file $T/XDBSAVF" "CRTSAVF FILE($T/XDBSAVF) TEXT('XDB package (trial)')"
  t1 "copy the stream file into it" "CPYFRMSTMF FROMSTMF('$F') TOMBR('/QSYS.LIB/$T.LIB/XDBSAVF.FILE') MBROPT(*REPLACE) CVTDTA(*NONE)" || exit 1
  t1 "restore into $T" "RSTOBJ OBJ(*ALL) SAVLIB($BL) DEV(*SAVF) SAVF($T/XDBSAVF) RSTLIB($T)"
  grep -E "CPC37[0-9A-F]{2}|CPF37[0-9A-F]{2}" $X >> $L
  echo "  licence texts: XDBLICENSE members $(ls /QSYS.LIB/$T.LIB/XDBLICENSE.FILE 2>/dev/null | tr '\n' ' ')" >> $L
  bindings "as restored"
  activation "as restored"
  rm -f /tmp/xdbt19.out
  system "CALL PGM($P/XDBT19) PARM('$T' '$OWN' 'C' '/tmp/xdbt19.out')" > $X 2>&1; echo "checks rc=$?" >> $L
  ;;
SETUP|OWNER)
  M=A; if test "$MODE" = "OWNER"; then M=O; fi
  rm -f /tmp/xdbt19.out
  system "CALL PGM($P/XDBT19) PARM('$T' '$OWN' '$M' '/tmp/xdbt19.out')" > $X 2>&1; echo "setup rc=$?" >> $L
  grep -E "CPF[0-9A-F]{4}|MCH[0-9]{4}|RNX[0-9]{4}" $X | head -8 >> $L
  bindings "after the setup"
  activation "after the setup"
  echo "  after the setup: owned by QDFTOWN [$(db2 "select 'own=' concat coalesce(listagg(trim(objname) concat ' ' concat trim(objtype), ', ') within group (order by objname, objtype), 'nothing') from table(qsys2.object_statistics('$T', '*ALL')) x where objowner = 'QDFTOWN'" 2>&1 | grep 'own=' | sed 's/.*own=//;s/ *$//')]" >> $L
  ;;
QDFTOWN)
  n=0
  for o in XDB:SRVPGM LIBPQ:SRVPGM XDBARD:PGM XDBCMT:PGM XDBPRFM:PGM XDBPRFT:PGM WRKXDBPRF:CMD WRKXDBPRF:PNLGRP XDBPRFD:FILE XDBKWD:FILE XDBPRFMDL:FILE XDBPROMDL:FILE XDBLICENSE:FILE XDBVER:DTAARA; do
    if system "CHGOBJOWN OBJ($T/${o%%:*}) OBJTYPE(*${o##*:}) NEWOWN(QDFTOWN)" > $X 2>&1; then n=$((n+1))
    else echo "${o%%:*} *${o##*:} to QDFTOWN -> FAILED: $(grep -E 'CPF[0-9A-F]{4}' $X | head -1 | cut -c1-90)" >> $L; fi
  done
  echo "objects given to QDFTOWN: $n of 14" >> $L
  ;;
CLEAN)
  for o in XDB LIBPQ; do t1 "delete $o" "DLTSRVPGM SRVPGM($T/$o)"; done
  for o in XDBARD XDBCMT XDBPRFM XDBPRFT; do t1 "delete $o" "DLTPGM PGM($T/$o)"; done
  t1 "delete command" "DLTCMD CMD($T/WRKXDBPRF)"
  t1 "delete help" "DLTPNLGRP PNLGRP($T/WRKXDBPRF)"
  for o in XDBPRFD XDBKWD XDBPRFMDL XDBPROMDL XDBPRF XDBPRO XDBLICENSE XDBSAVF; do t1 "delete file $o" "DLTF FILE($T/$o)"; done
  t1 "delete version" "DLTDTAARA DTAARA($T/XDBVER)"
  t1 "delete password list" "DLTVLDL VLDL($T/XDBVLDL)"
  ;;
*) echo "mode not known: $MODE" >> $L ;;
esac
rm -f $X
echo "end" >> $L
