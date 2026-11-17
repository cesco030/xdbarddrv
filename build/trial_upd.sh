# QSH script: trial of an UPDATE - an older package installed and set up in the build library,
# then the objects save file of a newer package restored over it, as the README tells it.
#   usage: sh trial_upd.sh <kitdir> <buildlib> <oldversion> <newversion> <pgmlib> [<owner>]
#   Both packages are taken from <kitdir>/../dist and must have been made in <buildlib>.
#   <pgmlib> holds XDBT19, the batch driver of the setup module (see trial.sh).
#   <owner>: an existing user profile that gets the driver and the password list of the old
#   installation, as the setup does it.  Left out, the owner lines are not run, and the trial
#   does not cover a driver that has changed its owner (one of the two reasons for ALWOBJDIF).
#   Run it in a job WITHOUT <buildlib> in its library list.
# 1. empties <buildlib> (only when it carries the marker of dist.sh), restores <oldversion>,
#    runs its setup and writes one profile into XDBPRF
# 2. restores <newversion> over it:  RSTOBJ ... ALWOBJDIF(*OWNER *FILELVL)
# 3. shows the checks (what F9 shows), runs the setup again (what F10 does), then looks at the
#    checks, at the driver programs, at the profile and at the version
# 4. leaves <buildlib> as trial_all.sh does: <newversion> restored, not set up
#   log: <kitdir>/out/trial_upd.log.  Good: last line "RESULT: update trial OK"; every step
#   that is judged has a line ending in "-> OK".
K=$1; B=$2; VO=$3; VN=$4; P=$5; OWN=$6
test -d $K/out || mkdir $K/out
L=$K/out/trial_upd.log
X=$K/out/trial_upd.tmp
FO=$K/../dist/xdb-$VO-objects.savf
FN=$K/../dist/xdb-$VN-objects.savf
echo "trial of an update $VO -> $VN in $B, owner [$OWN], $(date)" > $L
if test -z "$P"; then echo "usage: sh trial_upd.sh <kitdir> <buildlib> <oldversion> <newversion> <pgmlib> [<owner>]" >> $L; exit 1; fi
for f in $FO $FN; do
  if test ! -f $f; then echo "$f not found. Nothing was done." >> $L; exit 1; fi
done
if system "CHKOBJ OBJ($B/XDBBUILD) OBJTYPE(*DTAARA)" > /dev/null 2>&1; then
  system "CLRLIB LIB($B)" > /dev/null 2>&1
  system "CRTDTAARA DTAARA($B/XDBBUILD) TYPE(*CHAR) LEN(50) VALUE('XDB build library') TEXT('Marks a library that dist.sh may clear')" > /dev/null 2>&1
else
  echo "$B is not a build library of dist.sh (no data area XDBBUILD). Nothing was done." >> $L; exit 1
fi
OWNP=$OWN; if test -z "$OWNP"; then OWNP=" "; fi
activation() { # label; returns 1 when a driver program does not start
  r=0
  for o in XDBARD XDBCMT; do
    if system "CALL PGM($B/$o)" > $X 2>&1; then echo "  $1: CALL $B/$o -> starts" >> $L
    else r=1; echo "  $1: CALL $B/$o -> does not start: $(grep -E 'MCH[0-9]{4}|CPF[0-9A-F]{4}|CPD[0-9A-F]{4}|CEE[0-9]{4}' $X | head -1 | cut -c1-100)" >> $L; fi
  done
  return $r
}
notok() { # lines of the last checks in /tmp/xdbt19.out that are not OK; without an owner the two owner lines do not count
  if test -n "$OWN"; then sed -n '/--- after/,$p' /tmp/xdbt19.out | grep -E "^[TWF] " | wc -l | tr -d ' '
  else sed -n '/--- after/,$p' /tmp/xdbt19.out | grep -E "^[TWF] " | grep -v -E "^. (OWNPRF|OWNER) " | wc -l | tr -d ' '; fi
}

echo "--- 1. $VO installed and set up" >> $L
sh $K/build/trial.sh $K $FO $B $B $P RESTORE
grep -E "CPC37|CPF37|FAILED" $K/out/trial.log >> $L
if test -n "$OWN"; then sh $K/build/trial.sh $K $FO $B $B $P OWNER $OWN; else sh $K/build/trial.sh $K $FO $B $B $P SETUP; fi
grep -E "rc=|FAILED" $K/out/trial.log >> $L
n=$(notok)
if test "$n" = "0"; then echo "setup of $VO -> OK" >> $L; else echo "setup of $VO -> $n lines NOT OK" >> $L; sed -n '/--- after/,$p' /tmp/xdbt19.out | grep -E "^[TWF] " >> $L; fi
db2 "insert into $B.XDBPRF (PRRDB, PRTYPE, PRSTS, PRTEXT, PRHOST, PRPORT, PRDBNM, PRUSER, PRPWDS, PRTLS, PRTMO, PRCCSID, PRTXTL, PRNSCL, PRTRC, PRCRTU, PRCRTT, PRCHGU, PRCHGT) values('TRIALUPD', 'PG', 'A', 'trial of an update', 'localhost', 0, 'db', 'user', 'N', '*PREFER', 10, 0, 0, 0, 'N', user, current timestamp, user, current timestamp)" > $X 2>&1
if grep "DB20000I" $X > /dev/null; then echo "one profile written into XDBPRF -> OK" >> $L; else echo "one profile written into XDBPRF -> FAILED" >> $L; head -4 $X >> $L; fi

echo "--- 2. $VN restored over it" >> $L
system "CRTSAVF FILE($B/XDBSAVN) TEXT('XDB package (trial of an update)')" > /dev/null 2>&1
if system "CPYFRMSTMF FROMSTMF('$FN') TOMBR('/QSYS.LIB/$B.LIB/XDBSAVN.FILE') MBROPT(*REPLACE) CVTDTA(*NONE)" > $X 2>&1; then :; else echo "copy of $FN into $B/XDBSAVN -> FAILED" >> $L; fi
echo "RSTOBJ OBJ(*ALL) SAVLIB($B) DEV(*SAVF) SAVF($B/XDBSAVN) RSTLIB($B) ALWOBJDIF(*OWNER *FILELVL)" >> $L
system "RSTOBJ OBJ(*ALL) SAVLIB($B) DEV(*SAVF) SAVF($B/XDBSAVN) RSTLIB($B) ALWOBJDIF(*OWNER *FILELVL)" > $X 2>&1
grep -E "CPF37[0-9A-F]{2}|CPF38[0-9A-F]{2}|CPC37[0-9A-F]{2}|CPF32[0-9A-F]{2}" $X | sed 's/^/  /' >> $L
if grep -E " 0 not restored|CPC3703" $X > /dev/null; then echo "restore of $VN over $VO -> OK" >> $L; else echo "restore of $VN over $VO -> INCOMPLETE" >> $L; fi

echo "--- 3. checks after the restore (what F9 shows)" >> $L
rm -f /tmp/xdbt19.out
system "CALL PGM($P/XDBT19) PARM('$B' '$OWNP' 'C' '/tmp/xdbt19.out')" > /dev/null 2>&1
grep -E "^[OTWF] |cmd:" /tmp/xdbt19.out >> $L
activation "before the setup"
M=A; if test -n "$OWN"; then M=O; fi
rm -f /tmp/xdbt19.out
system "CALL PGM($P/XDBT19) PARM('$B' '$OWNP' '$M' '/tmp/xdbt19.out')" > $X 2>&1
echo "--- setup run (what F10 does)" >> $L
grep -E "^  run:|^   ->" /tmp/xdbt19.out >> $L
echo "--- checks after the setup" >> $L
sed -n '/--- after/,$p' /tmp/xdbt19.out | grep -E "^[OTWF] " >> $L
n=$(notok)
if test "$n" = "0"; then echo "setup after the update -> OK" >> $L; else echo "setup after the update -> $n lines NOT OK" >> $L; fi
if activation "after the setup"; then echo "driver programs after the update -> OK" >> $L; else echo "driver programs after the update -> FAILED" >> $L; fi
db2 "select 'profiles kept: ' concat trim(char(count(*))) from $B.XDBPRF where PRRDB = 'TRIALUPD'" > $X 2>&1
if grep "profiles kept: 1" $X > /dev/null; then echo "profile of the old installation still there -> OK" >> $L; else echo "profile of the old installation still there -> FAILED" >> $L; head -4 $X >> $L; fi
db2 "select 'version: ' concat trim(data_area_value) from qsys2.data_area_info where data_area_library = '$B' and data_area_name = 'XDBVER'" > $X 2>&1
if grep "version: $VN " $X > /dev/null; then echo "version reads $VN -> OK" >> $L; else echo "version reads $VN -> FAILED" >> $L; grep "version" $X >> $L; fi
db2 "select 'XDB: owner ' concat trim(o.objowner) concat ', user profile ' concat trim(coalesce(p.user_profile, '?')) from table(qsys2.object_statistics('$B', '*SRVPGM', 'XDB')) o, qsys2.program_info p where p.program_library = '$B' and p.program_name = 'XDB' and p.object_type = '*SRVPGM'" 2>&1 | grep "XDB: owner" | sed 's/^ *//' >> $L

echo "--- 4. $B left with $VN restored and not set up" >> $L
sh $K/build/trial_all.sh $K $B $VN $P fresh
grep -E "CPC37|CPF37|FAILED" $K/out/trial3.log >> $L
rm -f $X
bad=$(grep -E "FAILED|INCOMPLETE|NOT OK" $L | wc -l | tr -d ' ')
if test "$bad" = "0"; then echo "RESULT: update trial OK" >> $L; else echo "RESULT: update trial FAILED ($bad lines)" >> $L; fi
echo "end $(date)" >> $L
