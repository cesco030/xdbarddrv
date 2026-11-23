#!/bin/sh
# Linux test of stored procedures of a SQL Server (xdb/xdbms.c), through the emulation of the
# driver: the rows of a procedure through a cursor on its EXEC.
#   usage: sh proctest_ms.sh <emu dir with ardsh>     needs
#     - Babelfish for PostgreSQL (test/babelfish_build.sh): TDS on 127.0.0.1:14333, login
#       babelfish_user / xdbpw (the login creates and drops the procedures xdb_pt_*);
#     - test/tdsmock.py on 127.0.0.1:14330 (login sa / xdbpw), which answers as SQL Server 2000.
#   Compare with proctest_ms.expected.
# The EXEC is described without running it (sp_prepare, SET FMTONLY) and runs when the cursor
# is opened; its rows are kept by the driver.  What is tried:
#   - one result set, the cursor opened again with another value;
#   - several result sets of different shapes: the first is the cursor, the others are dropped
#     (kept in one table they once were, which freed memory that was never allocated);
#   - no result set (SQLSTATE 07005, the procedure has run), an error after the rows;
#   - a procedure with two ways through it, described with the columns of one and run through
#     the other: SQLSTATE 07008 in place of rows that are not what was described;
#   - a value that comes back: in one batch with the call it goes into a temporary table;
#   - with the mock: a statement that is described with no columns and returns rows when it
#     runs gets its columns with the open ("the open tells n columns");
#   - the row status column that a server cursor adds to its rows (mock: as the real server;
#     Babelfish: none) is not a column of the statement, on every server.
# After each case the connection answers the next statement.
# Work files: /tmp/xdb_procs.  XDB_PROCT_TRACE=Y writes the traces to /tmp/xdb_PROCMS.log ...
E=$1
D=/tmp/xdb_procs
P=$D/profile.emu
mkdir -p $D
export XDB_EMU_JOBCCSID=280 XDB_PRF_EMU=$P XDB_CONF=/dev/null XDB_SH_MAXROWS=20
T=${XDB_PROCT_TRACE:-N}
{
  echo "P|PROCMS|MSSQL|A|127.0.0.1|${XDB_MS_PORT:-14333}|${XDB_MS_DB:-master}|${XDB_MS_USER:-babelfish_user}|*OFF|10|0|1000|6|$T"
  echo "W|PROCMS|xdbpw"
  echo "P|PROCM1|MSSQL|A|127.0.0.1|${XDB_MS_PORT:-14333}|${XDB_MS_DB:-master}|${XDB_MS_USER:-babelfish_user}|*OFF|10|0|1000|6|$T"
  echo "W|PROCM1|xdbpw"
  echo "O|PROCM1|tds|C|7.1"
  echo "P|PROCM0|MSSQL|A|127.0.0.1|14330|testdb|sa|*OFF|10|0|1000|6|$T"
  echo "W|PROCM0|xdbpw"
} > $P
run() { # RDB name, title, then the requests on standard input
  echo "== $2"
  XDB_TEST_RDB=$1 $E/ardsh 2>&1 | grep -E "^(>|AR|  row|  col|  rows|  total|  the open)" | cut -c1-170
}

run PROCMS "the procedures" <<'EOF2'
C
X if object_id('xdb_pt_add') is not null drop procedure xdb_pt_add
X if object_id('xdb_pt_rows') is not null drop procedure xdb_pt_rows
X if object_id('xdb_pt_two') is not null drop procedure xdb_pt_two
X if object_id('xdb_pt_three') is not null drop procedure xdb_pt_three
X if object_id('xdb_pt_none') is not null drop procedure xdb_pt_none
X if object_id('xdb_pt_fail') is not null drop procedure xdb_pt_fail
X if object_id('xdb_pt_way') is not null drop procedure xdb_pt_way
X create procedure xdb_pt_add @a int, @b int, @total int output as begin set @total = @a + @b end
X create procedure xdb_pt_rows @n int as begin select @n as n, @n * @n as sq, 'row ' + cast(@n as varchar(10)) as label union all select @n + 1, (@n + 1) * (@n + 1), 'row ' + cast(@n + 1 as varchar(10)) end
X create procedure xdb_pt_two @n int as begin select @n as first_n, 'first' as which; select @n + 1 as second_n, 'second' as which, 'x' as more end
X create procedure xdb_pt_three @n int as begin select @n as a, 'first' as b, 'third column' as c; select @n + 1 as only_one; select 'and' as one, 'more' as two end
X create procedure xdb_pt_none @n int as begin declare @x int; set @x = @n end
X create procedure xdb_pt_fail @n int as begin select @n as n; raiserror('xdb_pt_fail says no', 16, 1) end
X create procedure xdb_pt_way @n int as begin if @n = 1 select @n as a, 'two columns' as b else select @n as x, 'three' as y, 'columns' as z end
D
EOF2

for R in PROCMS PROCM1; do
run $R "$R: a cursor on an EXEC, opened again; several result sets; none; an error after the rows" <<'EOF2'
C
Q exec xdb_pt_rows 7
O c1 exec xdb_pt_rows ? ## i:200
F c1
F c1
K c1
A c1 ## i:300
F c1
K c1
Q exec xdb_pt_two 7
Q select 1 as in_step
Q exec xdb_pt_three 7
Q select 2 as in_step
O c2 exec xdb_pt_none ? ## i:9
Q XDB LAST ERROR
Q select 3 as in_step
O c3 exec xdb_pt_fail ? ## i:4
Q select 4 as in_step
P s1 exec xdb_pt_three ?
E s1 i:5
Q select 5 as in_step
D
EOF2
run $R "$R: described with the columns of one way through the procedure, run through the other" <<'EOF2'
C
O c1 exec xdb_pt_way ? ## i:2
F c1
K c1
A c1 ## i:1
Q XDB LAST ERROR
Q select 6 as in_step
D
EOF2
run $R "$R: a value that comes back, through a temporary table" <<'EOF2'
C
X create table #xdb_pt_out (total int)
P s1 declare @t int; exec xdb_pt_add ?, ?, @t output; insert into #xdb_pt_out values (@t)
E s1 i:40|i:2
E s1 i:1000|i:234
Q select total from #xdb_pt_out order by total
D
EOF2
done

run PROCM0 "the mock: columns that are known only when the statement has run" <<'EOF2'
C
Q values (1, 'a'), (2, 'b') /*nocursor*/
Q select 7 as in_step
D
EOF2

# The same in one statement slot, as interactive SQL does it: the statement that follows in
# the slot is described at its PREPARE like any other (once the columns of the open stayed
# switched on for the slot, and the next statement was described a second time at its open).
run PROCM0 "the mock: the same in one slot, and the statement that follows in it" <<'EOF2'
C
R values (1, 'a'), (2, 'b') /*nocursor*/
R select 10 as in_slot
D
EOF2

# A server cursor of SQL Server gives every row one column more than the statement has: the
# status of the row, a last column named ROWSTAT.  The mock sends it as the real server does,
# Babelfish does not.  It is no column of the statement; a last column of that name that the
# statement itself has stays.
for R in PROCM0 PROCMS PROCM1; do
run $R "$R: the row status column of a server cursor, and a column of the statement with that name" <<'EOF2'
C
Q select 8 as in_step
Q select 1 as a, 2 as rowstat
O c1 select 3 as a, 4 as rowstat
F c1
K c1
A c1
F c1
K c1
Q select 9 as in_step
D
EOF2
done

run PROCMS "tidy" <<'EOF2'
C
X drop procedure xdb_pt_add
X drop procedure xdb_pt_rows
X drop procedure xdb_pt_two
X drop procedure xdb_pt_three
X drop procedure xdb_pt_none
X drop procedure xdb_pt_fail
X drop procedure xdb_pt_way
D
EOF2
