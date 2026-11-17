# QSH script: the whole trial of a package made by dist.sh, in the build library itself.
#   usage: sh trial_all.sh <kitdir> <buildlib> <version> <pgmlib> [fresh]
#   <pgmlib> holds XDBT19, the batch driver of the setup module (a development library)
#   Run it in a job WITHOUT <buildlib> in its library list.
# 1. empties <buildlib> (only when it carries the marker of dist.sh), restores the objects save
#    file from the IFS into it and shows the checks                -> out/trial1.log, /tmp/xdbt19_1.out
# 2. runs the setup, all but the owner profile and the ownership     -> out/trial2.log, /tmp/xdbt19_2.out
# 3. empties and restores again, so that <buildlib> is left as an installer finds it: restored,
#    not set up (add it to the library list and run WRKXDBPRF to see the Setup display)
#    -> out/trial3.log
# With "fresh" only step 3 is done.
K=$1; B=$2; V=$3; P=$4; ONLY=$5
F=$K/../dist/xdb-$V-objects.savf
A=$K/out/trial_all.log
echo "trial of $F in $B, $(date)" > $A
empty() {
  if system "CHKOBJ OBJ($B/XDBBUILD) OBJTYPE(*DTAARA)" > /dev/null 2>&1; then
    system "CLRLIB LIB($B)" > /dev/null 2>&1 && echo "$B emptied" >> $A
    system "CRTDTAARA DTAARA($B/XDBBUILD) TYPE(*CHAR) LEN(50) VALUE('XDB build library') TEXT('Marks a library that dist.sh may clear')" > /dev/null 2>&1
    return 0
  fi
  echo "$B is not a build library of dist.sh (no data area XDBBUILD). Nothing was done." >> $A
  return 1
}
if test -z "$ONLY"; then
  empty || exit 1
  sh $K/build/trial.sh $K $F $B $B $P RESTORE
  cp $K/out/trial.log $K/out/trial1.log; cp /tmp/xdbt19.out /tmp/xdbt19_1.out
  sh $K/build/trial.sh $K $F $B $B $P SETUP
  cp $K/out/trial.log $K/out/trial2.log; cp /tmp/xdbt19.out /tmp/xdbt19_2.out
fi
empty || exit 1
sh $K/build/trial.sh $K $F $B $B $P RESTORE
cp $K/out/trial.log $K/out/trial3.log
echo "end $(date)" >> $A
