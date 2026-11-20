#!/bin/sh
# Linux test of decimal columns that have more digits than Db2 takes from a driver (31),
# through the emulation of the driver, on all three kinds of server.
#   usage: sh dectest.sh <emu dir with ardsh>     needs the servers of the other tests:
#   PostgreSQL 127.0.0.1:5433 (postgres / xdbpw), MySQL 127.0.0.1:3306 (database xdbtest, user
#   xdbn / xdbpw), Babelfish 127.0.0.1:14333 (master, babelfish_user / xdbpw).  Nothing is
#   made on the servers.  Compare with dectest.expected.
# On IBM i a column described with more than 31 digits makes the DESCRIBE and the OPEN of a
# program fail with SQL0804, whatever its values are.  Such columns are common: the SUM of a
# decimal column is numeric(38,s) on SQL Server and decimal(p+22,s) on MySQL.  The driver
# describes them with 31 digits and the scale they have (type 485, length 31 * 256 + scale):
#   - a sum, and a value of 29 digits before the point and 2 after it, arrive as they are;
#   - a value with more digits before the point than 31 less the scale is shown by the
#     harness as <conv-error>: the program gets the indicator of a value that could not be
#     converted, for that value only, and the other columns and rows as they are;
#   - a scale above 31 is cut to 31 (digits at the end are left off).
E=$1
D=/tmp/xdb_dec
P=$D/profile.emu
mkdir -p $D
export XDB_EMU_JOBCCSID=280 XDB_PRF_EMU=$P XDB_CONF=/dev/null XDB_SH_MAXROWS=20
prof() { # name, type, port, database, user
  echo "P|$1|$2|A|127.0.0.1|$3|$4|$5|*OFF|10|0|1000|6|N"
  echo "W|$1|xdbpw"
}
{
  prof DCPG PG 5433 postgres postgres
  prof DCMY MYSQL 3306 xdbtest xdbn
  prof DCMS MSSQL 14333 master babelfish_user
} > $P
run() { # RDB name, title, then the requests on standard input
  echo "== $2"
  XDB_TEST_RDB=$1 $E/ardsh 2>&1 | grep -E "^(>|AR|  row|  col|  total)" | cut -c1-230
}
run DCPG "PostgreSQL" <<'EOF2'
C
Q select cast(1.5 as numeric(31,10)) as n31, cast(1.5 as numeric(32,10)) as n32, cast(1.5 as numeric(38,10)) as n38, cast(1.5 as numeric(63,10)) as n63
Q select cast(12345678901234567890123456789.5 as numeric(38,2)) as fits, cast(1234567890123456789012345678901234.5 as numeric(38,2)) as too_long, cast(-7.25 as numeric(38,2)) as after_it
Q select cast(0.12345678901234567890123456789012345 as numeric(38,35)) as small
Q select 1 as still_here
D
EOF2
run DCMY "MySQL: the sum of a decimal(20,2) column is decimal(42,2)" <<'EOF2'
C
Q select sum(d) as s, max(d) as m from (select cast(1.5 as decimal(20,2)) as d union all select cast(2.25 as decimal(20,2))) t
Q select cast(12345678901234567890123456789.5 as decimal(38,2)) as fits, cast(1234567890123456789012345678901234.5 as decimal(38,2)) as too_long, cast(-7.25 as decimal(38,2)) as after_it
Q select cast(1.5 as decimal(65,30)) as d65
Q select 1 as still_here
D
EOF2
run DCMS "SQL Server (Babelfish): the sum of a numeric(15,2) column is numeric(38,2)" <<'EOF2'
C
Q select sum(d) as s, max(d) as m from (select cast(1.5 as numeric(15,2)) as d union all select cast(2.25 as numeric(15,2))) t
Q select cast(12345678901234567890123456789.5 as numeric(38,2)) as fits, cast(1234567890123456789012345678901234.5 as numeric(38,2)) as too_long, cast(-7.25 as numeric(38,2)) as after_it
Q select cast(0.12345678901234567890123456789012345 as numeric(38,35)) as small
Q select 1 as still_here
D
EOF2
