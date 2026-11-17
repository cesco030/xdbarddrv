#!/bin/sh
# Linux test of the allowed users of a connection profile (option allowed_users), through the
# emulation of the driver: the profile comes from a text file, the user of the "job" and its
# groups from XDB_EMU_USER and XDB_EMU_GROUPS (test/xdbprf_emu.c).
#   usage: sh alwtest.sh <emu dir with ardsh>      needs the PostgreSQL of the other tests
#   (127.0.0.1:5433, user postgres, password xdbpw).  Compare the output with alwtest.expected.
E=$1
P=/tmp/xdb_alw.emu
export XDB_EMU_JOBCCSID=280 XDB_PRF_EMU=$P XDB_TEST_RDB=ALWPG XDB_CONF=/dev/null
try() { # list, user, groups, arguments of the connect (user password)
  {
    echo "P|ALWPG|PG|A|127.0.0.1|5433|postgres|postgres|*OFF|10|0|1000|6|N"
    echo "W|ALWPG|xdbpw"
    if test -n "$1"; then echo "O|ALWPG|allowed_users|D|$1"; fi
  } > $P
  r=$(printf 'C %s\n' "$4" | XDB_EMU_USER="$2" XDB_EMU_GROUPS="$3" $E/ardsh 2>&1 | grep ARCN0100 | sed 's/ outlen 20//')
  printf '%-22s user %-6s groups %-10s %-15s %s\n' "[$1]" "$2" "[$3]" "[$4]" "$r"
}
try ''                MARIO ''
try '*ALL'            MARIO ''
try '*all'            MARIO ''
try 'MARIO'           MARIO ''
try 'mario'           MARIO ''
try 'LUIGI'           MARIO ''
try 'LUIGI  MARIO'    MARIO ''
try 'LUIGI GRPIT'     MARIO 'GRPIT'
try 'LUIGI GRPIT'     MARIO 'GRPX GRPY'
try 'A B GRPY'        MARIO 'GRPX GRPY'
try 'MARIOX'          MARIO ''
try 'MARI'            MARIO ''
try 'VERYLONGNAMEXX'  MARIO ''
try 'LUIGI *ALL'      MARIO ''
try 'LUIGI'           MARIO '' 'postgres xdbpw'
try 'MARIO'           MARIO '' 'postgres xdbpw'
rm -f $P
