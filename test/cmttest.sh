#!/bin/sh
# Linux test of COMMIT when something went wrong on the server: the connection was lost inside
# the unit of work, the server refuses the commit, or the server rolled the transaction back
# before.  The harness (ardsh) plays the system's part in a commit, over one connection and
# over two: it asks the connections that are asked, lets the last agent commit, and tells the
# others the outcome.  After each case the rows are counted from outside the program.
#   usage: sh cmttest.sh <emu dir with ardsh>     needs the servers of the other tests:
#   PostgreSQL 127.0.0.1:5433 (postgres / xdbpw), MySQL 127.0.0.1:3306 (database xdbtest, user
#   xdbn / xdbpw, and root through the socket to end sessions and to count), and for the
#   SQL Server backend Babelfish (test/babelfish_build.sh): TDS on 127.0.0.1:14333, login
#   babelfish_user / xdbpw, its PostgreSQL side on port 5544 to end sessions and to count.
#   Compare with cmttest.expected.
E=$1
D=/tmp/xdb_cmt
P=$D/profile.emu
mkdir -p $D
export XDB_EMU_JOBCCSID=280 XDB_PRF_EMU=$P XDB_CONF=/dev/null XDB_SH_MAXROWS=200 XDB_EMU_JOBLOG=1
{
  echo "P|CMTPG|PG|A|127.0.0.1|5433|postgres|postgres|*OFF|10|0|1000|6|N"
  echo "W|CMTPG|xdbpw"
  echo "O|CMTPG|application_name|C|xdbcmt"
  echo "P|CMTPN|PG|A|127.0.0.1|5433|postgres|postgres|*OFF|10|0|1000|6|N"
  echo "W|CMTPN|xdbpw"
  echo "O|CMTPN|application_name|C|xdbcmt"
  echo "O|CMTPN|savepoints|D|off"
  echo "P|CMTPO|PG|A|127.0.0.1|5433|postgres|postgres|*OFF|10|0|1000|6|N"
  echo "W|CMTPO|xdbpw"
  echo "O|CMTPO|application_name|C|xdbcmt"
  echo "O|CMTPO|commit_vote|D|off"
  echo "P|CMTMY|MYSQL|A|127.0.0.1|3306|xdbtest|xdbn|*OFF|10|0|1000|6|N"
  echo "W|CMTMY|xdbpw"
  echo "P|CMTPZ|PG|A|127.0.0.1|5433|postgres|postgres|*OFF|10|0|1000|6|N"
  echo "W|CMTPZ|xdbpw"
  echo "O|CMTPZ|application_name|C|xdbcmt"
  echo "O|CMTPZ|idle_check|D|0"
  echo "P|CMTMZ|MYSQL|A|127.0.0.1|3306|xdbtest|xdbn|*OFF|10|0|1000|6|N"
  echo "W|CMTMZ|xdbpw"
  echo "O|CMTMZ|idle_check|D|0"
  echo "P|CMTMS|MSSQL|A|127.0.0.1|14333|master|babelfish_user|*OFF|10|0|1000|6|N"
  echo "W|CMTMS|xdbpw"
} > $P

PSQL="psql -h 127.0.0.1 -p 5433 -U postgres -At"
export PGPASSWORD=xdbpw
# the PostgreSQL side of Babelfish, where its TDS sessions and tables can be seen
BBF=${XDB_BBF:-${XDB_BBF_DIR:-/opt/bbf}/inst}
BSQL="env LD_LIBRARY_PATH=$BBF/lib:/usr/local/lib $BBF/bin/psql -h /tmp -p 5544 -U postgres -d babelfish_db -At"
# what happens outside the program: sessions are ended, rows are counted
cat > $D/endpg <<EOF
$PSQL -c "select count(pg_terminate_backend(pid)) from pg_stat_activity where application_name = 'xdbcmt'" > /dev/null 2>&1
echo "outside: the PostgreSQL session is ended"
EOF
cat > $D/endmy <<EOF
mysql -uroot -N -e "select concat('kill ', id, ';') from information_schema.processlist where user = 'xdbn'" 2>/dev/null | mysql -uroot 2>/dev/null
echo "outside: the MySQL session is ended"
EOF
cat > $D/endms <<EOF
$BSQL -c "select count(pg_terminate_backend(pid)) from pg_stat_activity where usename = 'babelfish_user' and pid <> pg_backend_pid()" > /dev/null 2>&1
echo "outside: the SQL Server session is ended"
EOF
cat > $D/cnt <<EOF
echo "outside: rows committed: PostgreSQL \$($PSQL -c "select count(*) from xdb_cmt_t"), MySQL \$(mysql -uroot -N -e "select count(*) from xdbtest.xdb_cmt_t"), SQL Server \$($BSQL -c "select count(*) from master_dbo.xdb_cmt_t")"
EOF
cat > $D/clean <<EOF
$PSQL -c "truncate xdb_cmt_t" > /dev/null 2>&1
mysql -uroot -e "truncate xdbtest.xdb_cmt_t" 2>/dev/null
$BSQL -c "truncate master_dbo.xdb_cmt_t" > /dev/null 2>&1
EOF
$PSQL -c "drop table if exists xdb_cmt_t, xdb_cmt_d; create table xdb_cmt_t (n int);
          create table xdb_cmt_d (n int, constraint xdb_cmt_d_u unique (n) deferrable initially deferred)" > /dev/null 2>&1
mysql -uroot -e "drop table if exists xdbtest.xdb_cmt_t; create table xdbtest.xdb_cmt_t (n int) engine = innodb" 2>/dev/null
printf 'RDB CMTMS\nC\nX drop table xdb_cmt_t\nX create table xdb_cmt_t (n int)\nD\n' | $E/ardsh > /dev/null 2>&1

# the lines that tell the story; what varies is replaced by *
show() {
  grep -E "^(>|AR|  row|  COMMIT|  [A-Z]+ |  resource|outside|JOBLOG)" |
  grep -v -E "^AR(PD|OC|CC|FC|CN|DI|XI|XP|PS)0100 -> sqlcode (0|100) " |
  grep -v -E "^> (!|SLEEP|ISO|RDB) " |
  grep -E "^(>|AR|  COMMIT|  [A-Z]+ |  resource|outside|JOBLOG|  row: [-0-9]|  row: \[($INFO)\])" |
  sed -E -e 's/(native|error) [0-9]+/\1 */' | cut -c1-330
}
INFO="session\] \[(unit_of_work|commit_resource)|counters\] \[(commits|commits_failed|rollbacks|reconnects|units_of_work_lost)"
I="Q select * from xdb_connection_info"

# one server alone: its connection is the last agent
alone() { # $1 profile, $2 name of the end script
  sh $D/clean
  echo "== $1 alone: nothing is lost"
  $E/ardsh 2>&1 <<EOF | show
RDB $1
ISO *CHG
C
X insert into xdb_cmt_t values (1)
$I
COMMIT
! sh $D/cnt
D
EOF
  echo "== $1 alone: the session ends while the program does something else, then COMMIT"
  $E/ardsh 2>&1 <<EOF | show
RDB $1
ISO *CHG
C
X insert into xdb_cmt_t values (2)
! sh $D/$2
SLEEP 3
COMMIT
! sh $D/cnt
Q select * from xdb_last_error
Q select count(*) from xdb_cmt_t
$I
COMMIT
D
EOF
  echo "== $1 alone: only read, the session ends, COMMIT"
  $E/ardsh 2>&1 <<EOF | show
RDB $1
ISO *CHG
C
Q select count(*) from xdb_cmt_t
X set $3
! sh $D/$2
SLEEP 3
COMMIT
! sh $D/cnt
Q select count(*) from xdb_cmt_t
COMMIT
D
EOF
  echo "== $1 alone: the session ends, ROLLBACK"
  $E/ardsh 2>&1 <<EOF | show
RDB $1
ISO *CHG
C
X insert into xdb_cmt_t values (3)
! sh $D/$2
SLEEP 3
ROLLBACK
! sh $D/cnt
X insert into xdb_cmt_t values (4)
COMMIT
! sh $D/cnt
D
EOF
  echo "== $1 alone: the session ends, the next statement is told (-911), the program goes on and commits"
  $E/ardsh 2>&1 <<EOF | show
RDB $1
ISO *CHG
C
X insert into xdb_cmt_t values (5)
! sh $D/$2
SLEEP 3
X insert into xdb_cmt_t values (6)
X insert into xdb_cmt_t values (7)
$I
COMMIT
! sh $D/cnt
Q select * from xdb_last_error
X insert into xdb_cmt_t values (8)
COMMIT
! sh $D/cnt
D
EOF
  echo "== $1 alone: the session ends and COMMIT comes at once, before the driver would test the connection"
  $E/ardsh 2>&1 <<EOF | show
RDB $1
ISO *CHG
C
X insert into xdb_cmt_t values (9)
! sh $D/$2
COMMIT
! sh $D/cnt
Q select * from xdb_last_error
D
EOF
}
alone CMTPG endpg "application_name = 'xdbcmt'"
alone CMTMY endmy "@xdb_cmt = 1"
alone CMTMS endms "nocount off"

# MySQL 8 says a last word before it closes a session for inactivity; that message waits on the
# line and is read as the answer to the COMMIT.  With idle_check = 0 the driver never tests the
# connection first, so the COMMIT is what finds it.
sh $D/clean
echo "== MySQL closes the session for inactivity (wait_timeout), it was only read, COMMIT"
$E/ardsh 2>&1 <<EOF | show
RDB CMTMZ
ISO *CHG
C
Q select count(*) from xdb_cmt_t
X set session wait_timeout = 1
SLEEP 3
COMMIT
Q select * from xdb_last_error
D
EOF
echo "== MySQL closes the session for inactivity (wait_timeout), a row was written, COMMIT"
$E/ardsh 2>&1 <<EOF | show
RDB CMTMZ
ISO *CHG
C
X insert into xdb_cmt_t values (1)
X set session wait_timeout = 1
SLEEP 3
COMMIT
! sh $D/cnt
Q select * from xdb_last_error
D
EOF

# The session ends between two statements of a unit of work and nothing tests the connection
# before the second: the statement is what finds it.  It must fail, and the COMMIT after it too.
sh $D/clean
for R in CMTPZ CMTMZ; do
  if test $R = CMTPZ; then END=endpg; else END=endmy; fi
  echo "== $R, never tested (idle_check = 0): the session ends between two statements of a unit of work"
  $E/ardsh 2>&1 <<EOF | show
RDB $R
ISO *CHG
C
X insert into xdb_cmt_t values (1)
! sh $D/$END
X insert into xdb_cmt_t values (2)
X insert into xdb_cmt_t values (3)
COMMIT
! sh $D/cnt
X insert into xdb_cmt_t values (4)
COMMIT
! sh $D/cnt
D
EOF
done

echo "== PostgreSQL: the server refuses the COMMIT (a constraint that is checked at the end)"
$PSQL -c "truncate xdb_cmt_d" > /dev/null 2>&1
$E/ardsh 2>&1 <<EOF | show
RDB CMTPG
ISO *CHG
C
X insert into xdb_cmt_d values (1)
X insert into xdb_cmt_d values (1)
COMMIT
Q select * from xdb_last_error
Q select count(*) from xdb_cmt_d
COMMIT
D
EOF
echo "== PostgreSQL without a savepoint for each statement: a statement fails, the program commits"
sh $D/clean
$E/ardsh 2>&1 <<EOF | show
RDB CMTPN
ISO *CHG
C
X insert into xdb_cmt_t values (1)
X insert into no_such_table values (1)
COMMIT
! sh $D/cnt
Q select * from xdb_last_error
D
EOF
echo "== option commit_vote = off: the connection is only told, a lost session cannot stop the commit"
sh $D/clean
$E/ardsh 2>&1 <<EOF | show
RDB CMTPO
ISO *CHG
C
X insert into xdb_cmt_t values (1)
$I
! sh $D/endpg
SLEEP 3
COMMIT
! sh $D/cnt
Q select * from xdb_last_error
D
EOF
echo "== a system that takes no last agent: the connection is asked before the commit"
sh $D/clean
XDB_EMU_CMT=asked $E/ardsh 2>&1 <<EOF | show
RDB CMTPG
ISO *CHG
C
X insert into xdb_cmt_t values (1)
$I
COMMIT
X insert into xdb_cmt_t values (2)
! sh $D/endpg
SLEEP 3
COMMIT
! sh $D/cnt
D
EOF
echo "== a system that takes no resource at all: every statement is committed at once"
sh $D/clean
XDB_EMU_CMT=none $E/ardsh 2>&1 <<EOF | show
RDB CMTPG
ISO *CHG
C
X insert into xdb_cmt_t values (1)
X insert into xdb_cmt_t values (2)
$I
! sh $D/cnt
D
! sh $D/cnt
EOF

# two servers in one unit of work: the first is the last agent, the second is asked
both() { # $1 first profile, $2 second, $3 end script of the first, $4 of the second
  sh $D/clean
  echo "== $1 then $2: nothing is lost"
  $E/ardsh 2>&1 <<EOF | show
ISO *CHG
RDB $1
C
X insert into xdb_cmt_t values (1)
$I
RDB $2
C
X insert into xdb_cmt_t values (1)
$I
COMMIT
! sh $D/cnt
EOF
  echo "== $1 then $2: the session on $2 ends, COMMIT"
  $E/ardsh 2>&1 <<EOF | show
ISO *CHG
RDB $1
C
X insert into xdb_cmt_t values (2)
RDB $2
C
X insert into xdb_cmt_t values (2)
! sh $D/$4
SLEEP 3
COMMIT
! sh $D/cnt
Q select * from xdb_last_error
EOF
  echo "== $1 then $2: the session on $1 ends, COMMIT"
  $E/ardsh 2>&1 <<EOF | show
ISO *CHG
RDB $1
C
X insert into xdb_cmt_t values (3)
RDB $2
C
X insert into xdb_cmt_t values (3)
! sh $D/$3
SLEEP 3
COMMIT
! sh $D/cnt
Q select * from xdb_last_error
EOF
  echo "== $1 then $2: $2 is only read and its session ends, COMMIT"
  $E/ardsh 2>&1 <<EOF | show
ISO *CHG
RDB $1
C
X insert into xdb_cmt_t values (4)
RDB $2
C
Q select count(*) from xdb_cmt_t
! sh $D/$4
SLEEP 3
COMMIT
! sh $D/cnt
EOF
  echo "== $1 then $2: $1 is disconnected and connected again"
  $E/ardsh 2>&1 <<EOF | show
ISO *CHG
RDB $1
C
X insert into xdb_cmt_t values (5)
RDB $2
C
X insert into xdb_cmt_t values (5)
COMMIT
RDB $1
D
C
X insert into xdb_cmt_t values (6)
$I
COMMIT
! sh $D/cnt
EOF
}
both CMTPG CMTMY endpg endmy
both CMTMY CMTPG endmy endpg
both CMTPG CMTMS endpg endms

$PSQL -c "drop table if exists xdb_cmt_t, xdb_cmt_d" > /dev/null 2>&1
mysql -uroot -e "drop table if exists xdbtest.xdb_cmt_t" 2>/dev/null
printf 'RDB CMTMS\nC\nX drop table xdb_cmt_t\nD\n' | $E/ardsh > /dev/null 2>&1
