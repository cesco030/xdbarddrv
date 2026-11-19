#!/bin/sh
# Linux test of option fetch_rows (rows asked from the server at a time), through the
# emulation of the driver, on all three kinds of server.
#   usage: sh fetchtest.sh <emu dir with ardsh>     needs the servers of the other tests:
#   PostgreSQL 127.0.0.1:5433 (postgres / xdbpw), MySQL 127.0.0.1:3306 (database xdbtest, user
#   xdbn / xdbpw), Babelfish 127.0.0.1:14333 (master, babelfish_user / xdbpw).  Only temporary
#   tables are made.  Compare with fetchtest.expected.
# The same 2500 short rows are read with a profile that has no such option (200 at a time),
# with 1000, with 5000, and with a number above the limit, which counts as 5000.  What is
# looked at is the counter fetches_from_server of XDB CONNECTION INFO: 13, 3, 1 and 1
# exchanges, and 2500 rows each time, the last one always the same.  Rows that can be long
# are asked for in smaller groups whatever the option says (one group stays within about
# 4 MB): 600 rows of ten text columns take 4 exchanges with and without the option.
E=$1
D=/tmp/xdb_fetch
P=$D/profile.emu
mkdir -p $D
export XDB_EMU_JOBCCSID=280 XDB_PRF_EMU=$P XDB_CONF=/dev/null XDB_SH_MAXROWS=3000
prof() { # name, type, port, database, user, value of the option or nothing
  echo "P|$1|$2|A|127.0.0.1|$3|$4|$5|*OFF|10|0|1000|6|N"
  echo "W|$1|xdbpw"
  if test -n "$6"; then echo "O|$1|fetch_rows|D|$6"; fi
}
{
  for v in "" 1000 5000 99999; do
    prof FTPG$v PG 5433 postgres postgres $v
    prof FTMY$v MYSQL 3306 xdbtest xdbn $v
    prof FTMS$v MSSQL 14333 master babelfish_user $v
  done
} > $P
# what is shown: the requests, errors, of a query the number of its rows and the rows where
# one group of 200 or of 1000 ends and the next begins, the first and the last, and the four
# facts
show() {
  grep -E "^(>|AR|  row|  total)" | grep -v -E "^AR(PD|OC|CC|FC|XI)0100 -> sqlcode (0|100) " |
  grep -E "^(>|AR|  total|  row: (1|199|200|201|600|1000|1001|2500) |  row: \[(options\] \[fetch_rows|limits\] \[rows_per_exchange|counters\] \[(rows_fetched|fetches_from_server))\])" |
  sed -E 's/ in [0-9]+ fetch calls//' | cut -c1-150
}
INFO="Q select * from xdb_connection_info"
PGROWS="select g as n, g * 2 as twice from generate_series(1, 2500) g"
MYROWS="with recursive s (n) as (select 1 union all select n + 1 from s where n < 2500) select n, n * 2 as twice from s"
MSROWS="select n, n * 2 as twice from #xdb_ft order by n"

one() { # title, rdb, statement before the query (or nothing), the query
  echo "== $1"
  { printf 'ISO *CHG\nC\n'
    if test -n "$3"; then printf 'X %s\n' "$3"; fi
    printf 'Q %s\n%s\nROLLBACK\nD\n' "$4" "$INFO"
  } | XDB_TEST_RDB=$2 $E/ardsh 2>&1 | show
}
MSFILL="create table #xdb_ft (n int primary key); declare @i int; set @i = 1; while @i <= 2500 begin insert into #xdb_ft values (@i); set @i = @i + 1 end"
for v in "" 1000 5000 99999; do
  w=${v:-none}
  one "PostgreSQL, 2500 short rows, option fetch_rows: $w" FTPG$v "" "$PGROWS"
  one "MySQL, 2500 short rows, option fetch_rows: $w" FTMY$v "set session cte_max_recursion_depth = 5000" "$MYROWS"
  one "SQL Server, 2500 short rows, option fetch_rows: $w" FTMS$v "$MSFILL" "$MSROWS"
done

WIDE="select g as n, repeat('a', 40) as t1, repeat('b', 40) as t2, repeat('c', 40) as t3, repeat('d', 40) as t4, repeat('e', 40) as t5, repeat('f', 40) as t6, repeat('g', 40) as t7, repeat('h', 40) as t8, repeat('i', 40) as t9, repeat('j', 40) as t10 from generate_series(1, 600) g"
one "PostgreSQL, 600 rows of ten text columns, no option" FTPG "" "$WIDE"
one "PostgreSQL, the same rows, option fetch_rows: 5000" FTPG5000 "" "$WIDE"
