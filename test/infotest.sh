#!/bin/sh
# Linux test of the statement XDB CONNECTION INFO (SELECT * FROM XDB_CONNECTION_INFO), which the
# driver answers itself: rows of AREA, NAME, VALUE about the connection, the server, the
# session and the driver.  All three backends; on PostgreSQL and MySQL the session is also ended
# from outside, once between two units of work and once inside one, and the counters must say so.
#   usage: sh infotest.sh <emu dir with ardsh>     needs the servers of the other tests:
#   PostgreSQL 127.0.0.1:5433 (postgres / xdbpw), MySQL 127.0.0.1:3306 (database xdbtest, user
#   xdbn / xdbpw, and root through the socket to end sessions), the TDS mock on 14330.
#   Compare with infotest.expected.
# What changes from run to run or from one installation to the next is shown as *: times,
# session numbers, versions, the cipher.
E=$1
D=/tmp/xdb_info
P=$D/profile.emu
mkdir -p $D
export XDB_EMU_JOBCCSID=280 XDB_PRF_EMU=$P XDB_CONF=/dev/null XDB_SH_MAXROWS=200
{
  echo "P|INFPG|PG|A|127.0.0.1|5433|postgres|postgres|*OFF|10|0|1000|6|N"
  echo "W|INFPG|xdbpw"
  echo "O|INFPG|application_name|C|xdbinfo"
  echo "P|INFMY|MYSQL|A|127.0.0.1|3306|xdbtest|xdbn|*OFF|10|0|1000|6|N"
  echo "W|INFMY|xdbpw"
  echo "P|INFMT|MYSQL|A|127.0.0.1|3306|xdbtest|xdbn|*REQUIRE|10|0|1000|6|N"
  echo "W|INFMT|xdbpw"
  echo "P|INFMS|MSSQL|A|127.0.0.1|14330|testdb|sa|*OFF|10|0|1000|6|N"
  echo "W|INFMS|xdbpw"
  echo "P|INFMZ|MYSQL|A|127.0.0.1|3306|xdbtest|xdbn|*OFF|10|0|1000|6|N"
  echo "W|INFMZ|xdbpw"
  echo "O|INFMZ|socket_buffer|C|0"
} > $P

# the rows of the areas named in $2 (all when empty), what varies replaced by *
show() {
  grep -E "^(>|AR|  row|  total)" | grep -v -E "^AR(PD|OC|CC|FC)0100 -> sqlcode (0|100) " |
  if test -n "$1"; then grep -E "^(>|AR|  total|  row: \[($1)\])"; else cat; fi |
  sed -E \
    -e 's/(\[(connected|session_id|version|version_number|server_version_num|built|client_library|time_zone|date_style|default_collation_id|seconds_since_last_statement|socket_buffer)\]) \[.*\]$/\1 [*]/' \
    -e 's/(\[last_reconnect\]) \[[0-9].*\]$/\1 [*]/' \
    -e 's/(\[encryption\]) \[TLS .*\]$/\1 [TLS *]/' | cut -c1-150
}
INFO="Q select * from xdb_connection_info"

echo "== PostgreSQL: a new connection, everything"
printf 'ISO *CHG\nC\n%s\nROLLBACK\nD\n' "$INFO" | XDB_TEST_RDB=INFPG $E/ardsh 2>&1 | show ""

echo "== PostgreSQL: after some work"
XDB_TEST_RDB=INFPG $E/ardsh 2>&1 <<EOF | show "session|counters|link"
ISO *CHG
C
X create temp table xdb_info_t (n int)
X insert into xdb_info_t values (1), (2), (3)
Q select count(*) from xdb_info_t
Q select no_such_column from xdb_info_t
Q xdb connection info
COMMIT
$INFO
D
EOF

endpg() { PGPASSWORD=xdbpw psql -h 127.0.0.1 -p 5433 -U postgres -Atc \
  "select count(pg_terminate_backend(pid)) from pg_stat_activity where application_name = 'xdbinfo'" > /dev/null 2>&1; }
endmy() { mysql -uroot -N -e "select concat('kill ', id, ';') from information_schema.processlist where user = 'xdbn'" 2>/dev/null |
  mysql -uroot 2>/dev/null; }

# session ended from outside: first with nothing open, then inside a unit of work
lost() { # $1 profile, $2 function that ends its sessions
  ( printf 'ISO *CHG\nC\nQ select 1\nCOMMIT\n'
    sleep 1; $2; sleep 3
    printf 'Q select 2\n%s\n' "$INFO"
    sleep 1; $2; sleep 3
    printf 'Q select 3\n%s\nROLLBACK\nQ select 4\n%s\nCOMMIT\nD\n' "$INFO" "$INFO"
  ) | XDB_TEST_RDB=$1 $E/ardsh 2>&1 | show "counters|session\] \[unit_of_work|link\] \[state"
}
echo "== PostgreSQL: the session is ended by the server, between two units of work and inside one"
lost INFPG endpg

echo "== MySQL: a new connection, everything"
printf 'ISO *CHG\nC\n%s\nROLLBACK\nD\n' "$INFO" | XDB_TEST_RDB=INFMY $E/ardsh 2>&1 | show ""
echo "== MySQL with TLS: the connection to the server"
printf 'ISO *CHG\nC\n%s\nROLLBACK\nD\n' "$INFO" | XDB_TEST_RDB=INFMT $E/ardsh 2>&1 | show "link"
echo "== MySQL: the session is ended by the server, between two units of work and inside one"
lost INFMY endmy

echo "== SQL Server (the mock of the tests): a new connection, everything"
printf 'ISO *CHG\nC\n%s\nROLLBACK\nD\n' "$INFO" | XDB_TEST_RDB=INFMS $E/ardsh 2>&1 | show ""

echo "== without commitment control, and what is not this statement"
XDB_TEST_RDB=INFPG $E/ardsh 2>&1 <<EOF | show "session"
ISO *NONE
C
$INFO
Q select * from xdb_connection_info where area = 'server'
Q select name from xdb_connection_info
D
EOF

# option socket_buffer: without it the driver asks for a buffer of 1 MB for its socket, with 0
# the socket keeps what the system gives it.  How much a system grants is its own matter, so
# only the comparison is shown.
sockbuf() { printf 'ISO *CHG\nC\n%s\nROLLBACK\nD\n' "$INFO" | XDB_TEST_RDB=$1 $E/ardsh 2>&1 |
  sed -n -E 's/^  row: \[link\] \[socket_buffer\] \[([0-9]+) bytes\]$/\1/p'; }
echo "== MySQL: the buffer of the socket, without option socket_buffer and with 0"
A=$(sockbuf INFMY); B=$(sockbuf INFMZ)
if test -n "$A" && test -n "$B" && test "$A" -gt "$B"; then echo "  larger without the option: yes"
else echo "  larger without the option: NO ($A, $B)"; fi
