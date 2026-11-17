**free
// XDBETL - one program, several databases at the same time: a sample and a test.
//
// Part 1, for each server: rows made on the server are read through a cursor and
//   - only fetched, 500 rows with each FETCH, then one row with each FETCH,
//   - written to a local table with native WRITE (no SQL on the local side, so the
//     connection is never changed),
//   - written to a local table with a blocked INSERT (SET CONNECTION to the local
//     database and back, once for each block of rows),
//   and for the first server also row by row with a change of connection for every
//   row, which is the way not to do it.  Every way is timed and the local rows are
//   counted and summed.
// Part 2: all the connections stay open.  Rows go into a temporary table on each
//   server and into a journaled local table; one COMMIT, then more rows and one
//   ROLLBACK, and the rows are counted everywhere after each.
//
// On the servers only temporary tables are made; they end with the connection.
//
//   CALL XDBETL PARM('<PostgreSQL>' '<MySQL>' '<SQL Server>' '<rows>' '<library>'
//                    '<output file>' '<database>')
//   The first three are names of connection profiles (command WRKXDBPRF) with their
//   RDB directory entries, or *NONE.  rows: 1 to 100000.  library: where the local
//   tables XDBETLT and XDBETLC are (xetl.sh makes them).  database: only for MySQL
//   when its connection has no database, the place of the temporary table; blank =
//   the first database of the server that lets this user make one.
//
// compile: CRTSQLRPGI ... COMMIT(*CHG) OPTION(*SQL) RDB(*LOCAL) CVTCCSID(*JOB)
//   RDB(*LOCAL): the statements for the servers are dynamic (PREPARE with ? for the
//   values), the static ones in this source are for the local database.
ctl-opt main(pMain) dftactgrp(*no) actgrp(*new);

// the local target of the native WRITE; not under commitment control
dcl-f xdbetlt usage(*output) block(*yes) prefix(o_) usropn;

// one block of rows as it comes from a server, and its null indicators
dcl-ds r qualified dim(500);
  id int(10);
  d date;
  amt packed(11 : 2);
  code char(10);
  txt varchar(40);
end-ds;
dcl-ds ni qualified dim(500);
  col int(5) dim(5);
end-ds;

dcl-s lines varchar(300) dim(200);
dcl-s nl int(10) inz(0);
dcl-s localRdb char(18);
dcl-s curRdb char(18);
dcl-s lib varchar(10);
dcl-s q varchar(1) inz('''');

// one line of the result with the SQLCODE of the statement before it; gives the SQLCODE back
dcl-proc note;
  dcl-pi *n int(10);
    what varchar(200) const;
  end-pi;
  dcl-s code int(10);
  dcl-s state char(5);
  dcl-s text varchar(200) inz('');

  code = sqlcod;
  state = sqlstt;
  exec sql set option datfmt = *iso, closqlcsr = *endmod, alwblk = *allread;
  if code < 0;
    exec sql get diagnostics condition 1 :text = message_text;
  endif;
  if nl < %elem(lines);
    nl += 1;
    lines(nl) = what + ' -> ' + %char(code) + ' ' + state;
    if text <> '';
      lines(nl) += '  [' + text + ']';
    endif;
  endif;
  return code;
end-proc;

dcl-proc say;
  dcl-pi *n;
    what varchar(300) const;
  end-pi;
  if nl < %elem(lines);
    nl += 1;
    lines(nl) = what;
  endif;
end-proc;

// milliseconds since t0
dcl-proc ms;
  dcl-pi *n int(10);
    t0 timestamp const;
  end-pi;
  return %div(%diff(%timestamp() : t0 : *mseconds) : 1000);
end-proc;

dcl-proc speed;
  dcl-pi *n varchar(80);
    rows int(10) const;
    msec int(10) const;
  end-pi;
  dcl-s m int(20);
  dcl-s n int(20);

  m = msec;
  if m < 1;
    m = 1;
  endif;
  n = rows;
  return %char(rows) + ' rows in ' + %char(msec) + ' ms = ' + %char(%div(n * 1000 : m))
       + ' rows a second';
end-proc;

dcl-proc nameOf;
  dcl-pi *n varchar(12);
    kind char(2) const;
  end-pi;
  if kind = 'PG';
    return 'PostgreSQL';
  elseif kind = 'MY';
    return 'MySQL';
  endif;
  return 'SQL Server';
end-proc;

// a query with one row and one number, on the current connection; -1 when it fails
dcl-proc one;
  dcl-pi *n int(20);
    label varchar(100) const;
    stm varchar(400) const;
  end-pi;
  dcl-s s varchar(400);
  dcl-s n int(20) inz(-1);
  dcl-s nn int(5) inz(0);

  exec sql declare c1 cursor for q1;
  s = stm;
  exec sql prepare q1 from :s;
  if sqlcod < 0;
    note(label + ' (prepare)');
    return -1;
  endif;
  exec sql open c1;
  if sqlcod < 0;
    note(label + ' (open)');
    return -1;
  endif;
  exec sql fetch c1 into :n :nn;
  if sqlcod < 0;
    note(label + ' (fetch)');
    exec sql close c1;
    return -1;
  endif;
  exec sql close c1;
  return n;
end-proc;

// a query with one row and one text, on the current connection
dcl-proc show;
  dcl-pi *n;
    label varchar(100) const;
    stm varchar(400) const;
  end-pi;
  dcl-s s varchar(400);
  dcl-s v varchar(200) inz('');
  dcl-s vn int(5) inz(0);

  exec sql declare c4 cursor for q4;
  s = stm;
  exec sql prepare q4 from :s;
  if sqlcod < 0;
    note(label + ' (prepare)');
    return;
  endif;
  exec sql open c4;
  if sqlcod < 0;
    note(label + ' (open)');
    return;
  endif;
  exec sql fetch c4 into :v :vn;
  if sqlcod < 0;
    note(label + ' (fetch)');
    exec sql close c4;
    return;
  endif;
  exec sql close c4;
  say(label + ': ' + v);
end-proc;

// The rows to copy, made by the server itself: a number, a date, an amount, a code of
// fixed length and a text.  Each server has its own way to write it; the types are
// cast to what the local table has.  ? is the number of rows.
dcl-proc srcStmt;
  dcl-pi *n varchar(2000);
    kind char(2) const;
  end-pi;
  dcl-s dg varchar(200);
  dcl-s gen varchar(1400);
  dcl-s i int(10);

  if kind = 'PG';
    return 'select g as id, date ' + q + '2024-01-01' + q + ' + (g % 365) as d, '
         + 'cast(g * 1.25 as numeric(11,2)) as amt, '
         + 'cast(lpad(cast(g % 100000 as text), 10, ' + q + '0' + q + ') as char(10)) as code, '
         + 'cast(' + q + 'row ' + q + ' || g || ' + q + ' from PostgreSQL' + q
         + ' as varchar(40)) as txt '
         + 'from generate_series(1, cast(? as integer)) g';
  endif;
  // no row generator on the other two: five times the ten digits make 100000 numbers
  dg = '(select 0 as n';
  for i = 1 to 9;
    dg += ' union all select ' + %char(i);
  endfor;
  dg += ')';
  gen = ' from (select d1.n + 10 * d2.n + 100 * d3.n + 1000 * d4.n + 10000 * d5.n + 1 as id'
      + ' from ' + dg + ' d1 cross join ' + dg + ' d2 cross join ' + dg + ' d3 cross join '
      + dg + ' d4 cross join ' + dg + ' d5) x where x.id <= ?';
  if kind = 'MY';
    return 'select x.id as id, '
         + 'date_add(date ' + q + '2024-01-01' + q + ', interval mod(x.id, 365) day) as d, '
         + 'cast(x.id * 1.25 as decimal(11,2)) as amt, '
         + 'cast(lpad(mod(x.id, 100000), 10, ' + q + '0' + q + ') as char(10)) as code, '
         + 'cast(concat(' + q + 'row ' + q + ', x.id, ' + q + ' from MySQL' + q
         + ') as char(40)) as txt' + gen;
  endif;
  return 'select cast(x.id as int) as id, '
       + 'dateadd(day, x.id % 365, cast(' + q + '2024-01-01' + q + ' as date)) as d, '
       + 'cast(x.id * 1.25 as decimal(11,2)) as amt, '
       + 'cast(right(' + q + '0000000000' + q
       + ' + cast(x.id % 100000 as varchar(10)), 10) as char(10)) as code, '
       + 'cast(' + q + 'row ' + q + ' + cast(x.id as varchar(10)) + ' + q + ' from SQL Server'
       + q + ' as varchar(40)) as txt' + gen;
end-proc;

// Reads the rows of stm from the current connection (a server) and
//   how = 'F'  only fetches them, 500 with each FETCH
//         '1'  only fetches them, one with each FETCH
//         'N'  writes them to the local table with native WRITE
//         'S'  writes them to the local table with one INSERT for each block
dcl-proc copyRows;
  dcl-pi *n;
    label varchar(80) const;
    stm varchar(2000) const;
    rows int(10) const;
    how char(1) const;
  end-pi;
  dcl-s s varchar(2000);
  dcl-s n int(10);
  dcl-s want int(10) inz(500);
  dcl-s got int(10);
  dcl-s total int(10) inz(0);
  dcl-s fetches int(10) inz(0);
  dcl-s full int(10) inz(0);
  dcl-s least int(10) inz(0);
  dcl-s i int(10);
  dcl-s atEnd ind inz(*off);
  dcl-s failed ind inz(*off);
  dcl-s t0 timestamp;
  dcl-s took int(10);
  dcl-s fId int(10);
  dcl-s fD date;
  dcl-s fAmt packed(11 : 2);
  dcl-s fCode char(10);
  dcl-s fTxt varchar(40);
  dcl-s tail varchar(120) inz('');

  exec sql declare rd cursor for rdStmt;
  s = stm;
  n = rows;
  t0 = %timestamp();
  exec sql prepare rdStmt from :s;
  if sqlcod < 0;
    note(label + ' (prepare)');
    return;
  endif;
  exec sql open rd using :n;
  if sqlcod < 0;
    note(label + ' (open)');
    return;
  endif;
  if how = 'N';
    open xdbetlt;
  endif;

  dow not atEnd;
    if how = '1';
      exec sql fetch rd into :fId, :fD, :fAmt, :fCode, :fTxt;
      if sqlcod < 0;
        note(label + ' (fetch)');
        leave;
      endif;
      if sqlcod = 100;
        leave;
      endif;
      total += 1;
      iter;
    endif;

    // up to 500 rows; SQLERRD(3) tells how many came, and the last ones can come
    // together with SQLCODE 100
    exec sql fetch rd for :want rows into :r :ni;
    if sqlcod < 0;
      note(label + ' (fetch)');
      leave;
    endif;
    got = sqlerrd(3);
    atEnd = (sqlcod = 100);
    if got > 0;
      fetches += 1;
      if got = want;
        full += 1;
      elseif not atEnd and (least = 0 or got < least);
        least = got;
      endif;
    endif;

    if how = 'N';
      for i = 1 to got;
        o_id = r(i).id;
        o_d = r(i).d;
        o_amt = r(i).amt;
        o_code = r(i).code;
        o_txt = r(i).txt;
        write xdbetlr;
      endfor;
    elseif how = 'S' and got > 0;
      // the cursor on the server stays open while the local connection is the current one
      exec sql set connection :localRdb;
      exec sql insert into qtemp.xdbetls :got rows values (:r :ni) with nc;
      if sqlcod < 0;
        note(label + ' (INSERT of a block)');
        failed = *on;
      endif;
      exec sql set connection :curRdb;
      if failed;
        leave;
      endif;
    endif;
    total += got;
  enddo;

  if how = 'N';
    close xdbetlt;
  endif;
  exec sql close rd;
  exec sql commit;
  took = ms(t0);
  if how <> '1';
    tail = '; ' + %char(fetches) + ' FETCH, ' + %char(full) + ' of them with 500 rows';
    if least > 0;
      tail += ', the smallest before the end ' + %char(least);
    endif;
  endif;
  say(label + ': ' + speed(total : took) + tail);
end-proc;

// The way not to do it: one row with each FETCH and a change of connection for each row
dcl-proc rowByRow;
  dcl-pi *n;
    label varchar(80) const;
    stm varchar(2000) const;
    rows int(10) const;
  end-pi;
  dcl-s s varchar(2000);
  dcl-s n int(10);
  dcl-s total int(10) inz(0);
  dcl-s t0 timestamp;
  dcl-s fId int(10);
  dcl-s fD date;
  dcl-s fAmt packed(11 : 2);
  dcl-s fCode char(10);
  dcl-s fTxt varchar(40);

  exec sql declare rr cursor for rrStmt;
  s = stm;
  n = rows;
  t0 = %timestamp();
  exec sql prepare rrStmt from :s;
  if sqlcod < 0;
    note(label + ' (prepare)');
    return;
  endif;
  exec sql open rr using :n;
  if sqlcod < 0;
    note(label + ' (open)');
    return;
  endif;
  exec sql set connection :localRdb;
  s = 'insert into qtemp.xdbetls values (?, ?, ?, ?, ?) with nc';
  exec sql prepare rrIns from :s;
  if sqlcod < 0;
    note(label + ' (prepare of the local INSERT)');
  endif;
  exec sql set connection :curRdb;
  dow 1 = 1;
    exec sql fetch rr into :fId, :fD, :fAmt, :fCode, :fTxt;
    if sqlcod < 0;
      note(label + ' (fetch)');
      leave;
    endif;
    if sqlcod = 100;
      leave;
    endif;
    exec sql set connection :localRdb;
    exec sql execute rrIns using :fId, :fD, :fAmt, :fCode, :fTxt;
    if sqlcod < 0;
      note(label + ' (local INSERT)');
      exec sql set connection :curRdb;
      leave;
    endif;
    exec sql set connection :curRdb;
    total += 1;
  enddo;
  exec sql close rr;
  exec sql commit;
  say(label + ': ' + speed(total : ms(t0)));
end-proc;

// counts and sums the rows of a local table, says whether they are the ones sent, and
// empties the table for the next run
dcl-proc localCheck;
  dcl-pi *n;
    table varchar(40) const;
    rows int(10) const;
  end-pi;
  dcl-s cnt int(20);
  dcl-s sm int(20);
  dcl-s expected int(20);
  dcl-s s varchar(200);
  dcl-s verdict varchar(60);

  exec sql set connection :localRdb;
  cnt = one('count of ' + table : 'select count(*) from ' + table);
  sm = one('sum of ' + table : 'select coalesce(sum(cast(id as bigint)), 0) from ' + table);
  expected = rows;
  expected = %div(expected * (expected + 1) : 2);
  if cnt = rows and sm = expected;
    verdict = 'as sent';
  else;
    verdict = 'NOT AS SENT (sum expected ' + %char(expected) + ')';
  endif;
  say('      ' + table + ' now has ' + %char(cnt) + ' rows, sum of the numbers ' + %char(sm)
      + ': ' + verdict);
  s = 'delete from ' + table + ' with nc';
  exec sql execute immediate :s;
  if sqlcod < 0;
    note('      emptying ' + table);
  endif;
  exec sql commit;
  exec sql set connection :curRdb;
end-proc;

// part 1 for one server; *on when the connection is there
dcl-proc etlFrom;
  dcl-pi *n ind;
    kind char(2) const;
    rdb char(18) const;
    rows int(10) const;
    first ind const;
  end-pi;
  dcl-s stm varchar(2000);
  dcl-s few int(10);

  say('--- ' + nameOf(kind));
  curRdb = rdb;
  exec sql connect to :curRdb;
  if note('CONNECT TO the profile of type ' + nameOf(kind)) < 0;
    return *off;
  endif;
  if kind = 'PG';
    show('   server' : 'select ' + q + 'PostgreSQL ' + q + ' || current_setting(' + q
         + 'server_version' + q + ')');
  elseif kind = 'MY';
    show('   server' : 'select concat(' + q + 'MySQL ' + q + ', version())');
  else;
    show('   server' : 'select ' + q + 'SQL Server ' + q + ' + cast(serverproperty(' + q
         + 'ProductVersion' + q + ') as varchar(30))');
  endif;
  exec sql commit;

  stm = srcStmt(kind);
  copyRows('   fetched only, 500 rows with each FETCH' : stm : rows : 'F');
  copyRows('   fetched only, one row with each FETCH' : stm : rows : '1');
  copyRows('   to a local table with native WRITE' : stm : rows : 'N');
  localCheck(lib + '.xdbetlt' : rows);
  copyRows('   to a local table with a blocked INSERT' : stm : rows : 'S');
  localCheck('qtemp.xdbetls' : rows);
  if first;
    few = rows;
    if few > 2000;
      few = 2000;
    endif;
    rowByRow('   row by row, connection changed for every row' : stm : few);
    localCheck('qtemp.xdbetls' : few);
  endif;
  return *on;
end-proc;

// part 2: a temporary table on the current connection; its name, or '' when it fails
dcl-proc tempTable;
  dcl-pi *n varchar(90);
    kind char(2) const;
    wantDb varchar(64) const;
  end-pi;
  dcl-s cols varchar(40) inz(' (id int, txt varchar(40))');
  dcl-s s varchar(400);
  dcl-s db varchar(64);
  dcl-s dbs varchar(64) dim(40);
  dcl-s ndb int(10) inz(0);
  dcl-s i int(10);
  dcl-s tab varchar(90);
  dcl-s made ind inz(*off);

  if kind = 'PG';
    tab = 'xdbetl_c';
    s = 'create temp table ' + tab + cols;
    exec sql execute immediate :s;
    made = (sqlcod = 0);
  elseif kind = 'MS';
    tab = '#xdbetl_c';
    s = 'create table ' + tab + cols;
    exec sql execute immediate :s;
    made = (sqlcod = 0);
  elseif wantDb <> '';
    tab = '`' + wantDb + '`.xdbetl_c';
    s = 'create temporary table ' + tab + cols;
    exec sql execute immediate :s;
    made = (sqlcod = 0);
  else;
    // MySQL keeps a temporary table in a database: the one of the connection, or the
    // first of the server that lets this user make one
    s = 'select schema_name from information_schema.schemata where schema_name not in ('
      + q + 'information_schema' + q + ', ' + q + 'mysql' + q + ', ' + q
      + 'performance_schema' + q + ', ' + q + 'sys' + q + ') order by schema_name';
    exec sql declare c6 cursor for q6;
    exec sql prepare q6 from :s;
    exec sql open c6;
    dow sqlcod = 0 and ndb < %elem(dbs);
      exec sql fetch c6 into :db;
      if sqlcod = 0;
        ndb += 1;
        dbs(ndb) = db;
      endif;
    enddo;
    exec sql close c6;
    tab = 'xdbetl_c';
    s = 'create temporary table ' + tab + cols;
    exec sql execute immediate :s;
    made = (sqlcod = 0);
    if not made;
      for i = 1 to ndb;
        tab = '`' + dbs(i) + '`.xdbetl_c';
        s = 'create temporary table ' + tab + cols;
        exec sql execute immediate :s;
        if sqlcod = 0;
          made = *on;
          say('   the MySQL connection has no database: temporary table in database number '
              + %char(i) + ' of the ' + %char(ndb) + ' the server lists');
          leave;
        endif;
      endfor;
    endif;
  endif;
  if not made;
    note('   temporary table on ' + nameOf(kind));
    return '';
  endif;
  say('   temporary table on ' + nameOf(kind) + ' made');
  return tab;
end-proc;

// part 2: rows first .. first + rows - 1 through a prepared INSERT on the current connection
dcl-proc fill;
  dcl-pi *n;
    label varchar(80) const;
    table varchar(90) const;
    first int(10) const;
    rows int(10) const;
  end-pi;
  dcl-s s varchar(200);
  dcl-s kId int(10);
  dcl-s kTx varchar(40);
  dcl-s done int(10) inz(0);
  dcl-s t0 timestamp;

  s = 'insert into ' + table + ' (id, txt) values (?, ?)';
  exec sql prepare ins from :s;
  if sqlcod < 0;
    note(label + ' (prepare)');
    return;
  endif;
  t0 = %timestamp();
  for kId = first to first + rows - 1;
    kTx = 'row ' + %char(kId);
    exec sql execute ins using :kId, :kTx;
    if sqlcod < 0;
      note(label + ' (row ' + %char(kId) + ')');
      leave;
    endif;
    done += 1;
  endfor;
  say(label + ': ' + speed(done : ms(t0)));
end-proc;

dcl-proc pMain;
  dcl-pi *n extpgm('XDBETL');
    pPg char(32) const;
    pMy char(32) const;
    pMs char(32) const;
    pRows char(32) const;
    pLib char(32) const;
    pFile char(32) const;
    pDb char(32) const options(*nopass);
  end-pi;
  dcl-s rows int(10);
  dcl-s fname varchar(60);
  dcl-s wline varchar(300);
  dcl-s wantDb varchar(64) inz('');
  dcl-s s varchar(400);
  dcl-s cRdb char(18) dim(3);
  dcl-s cKind char(2) dim(3);
  dcl-s cTab varchar(90) dim(3);
  dcl-s nc int(10) inz(0);
  dcl-s k int(10);
  dcl-s i int(10);
  dcl-s n int(20);
  dcl-s any ind inz(*off);
  dcl-s tmp char(18);
  dcl-s locTab varchar(40);

  monitor;
    rows = %int(%trim(pRows));
  on-error;
    rows = 10000;
  endmon;
  if rows < 1;
    rows = 1;
  elseif rows > 100000;
    rows = 100000;
  endif;
  lib = %trim(pLib);
  fname = %trim(pFile);
  if %parms() >= 7;
    wantDb = %trim(pDb);
  endif;
  locTab = lib + '.xdbetlc';

  // the local database first: its name is needed for SET CONNECTION later
  exec sql connect reset;
  note('CONNECT RESET (the local database)');
  exec sql values current server into :localRdb;
  exec sql create table qtemp.xdbetls (id integer not null, d date not null,
             amt decimal(11, 2) not null, code char(10) not null, txt varchar(40) not null);
  note('local table QTEMP.XDBETLS for the blocked INSERT');
  exec sql commit;
  say('rows asked from each server: ' + %char(rows) + ', asked for 500 at a time');

  // ---- part 1: server after server; every connection stays open
  if %trim(pPg) <> '*NONE' and etlFrom('PG' : pPg : rows : not any);
    nc += 1;
    cRdb(nc) = pPg;
    cKind(nc) = 'PG';
    any = *on;
  endif;
  if %trim(pMy) <> '*NONE' and etlFrom('MY' : pMy : rows : not any);
    nc += 1;
    cRdb(nc) = pMy;
    cKind(nc) = 'MY';
    any = *on;
  endif;
  if %trim(pMs) <> '*NONE' and etlFrom('MS' : pMs : rows : not any);
    nc += 1;
    cRdb(nc) = pMs;
    cKind(nc) = 'MS';
    any = *on;
  endif;

  // ---- part 2: all of them and the local database in one unit of work
  if nc > 0;
    say('--- ' + %char(nc) + ' server connections and the local one, open at the same time');
    for k = 1 to nc;
      curRdb = cRdb(k);
      exec sql set connection :curRdb;
      if sqlcod < 0;
        note('   SET CONNECTION to ' + nameOf(cKind(k)));
      endif;
      cTab(k) = tempTable(cKind(k) : wantDb);
    endfor;
    exec sql set connection :localRdb;
    s = 'delete from ' + locTab;
    exec sql execute immediate :s;
    exec sql commit;
    note('COMMIT (the temporary tables stay)');

    say('unit of work 1: 200 rows into each table, then one COMMIT');
    for k = 1 to nc;
      curRdb = cRdb(k);
      exec sql set connection :curRdb;
      if cTab(k) <> '';
        fill('   ' + nameOf(cKind(k)) + ', one INSERT for each row' : cTab(k) : 1 : 200);
      endif;
    endfor;
    exec sql set connection :localRdb;
    fill('   local Db2, one INSERT for each row' : locTab : 1 : 200);
    curRdb = cRdb(1);
    exec sql set connection :curRdb;
    exec sql commit;
    note('one COMMIT, given while ' + nameOf(cKind(1)) + ' is the current connection');
    exsr counts;

    say('unit of work 2: 200 more rows into each table, then one ROLLBACK');
    for k = 1 to nc;
      curRdb = cRdb(k);
      exec sql set connection :curRdb;
      if cTab(k) <> '';
        fill('   ' + nameOf(cKind(k)) + ', one INSERT for each row' : cTab(k) : 201 : 200);
      endif;
    endfor;
    exec sql set connection :localRdb;
    fill('   local Db2, one INSERT for each row' : locTab : 201 : 200);
    exec sql rollback;
    note('one ROLLBACK, given while the local database is the current connection');
    exsr counts;

    exec sql set connection :localRdb;
    s = 'delete from ' + locTab;
    exec sql execute immediate :s;
    exec sql commit;
    note('COMMIT before the connections are ended');
  endif;

  // ---- the end: back to the local database, then every server connection is ended
  exec sql connect reset;
  for k = 1 to nc;
    tmp = cRdb(k);
    exec sql disconnect :tmp;
    note('DISCONNECT from ' + nameOf(cKind(k)));
  endfor;
  s = 'call qsys2.ifs_write(path_name => ?, line => ?, overwrite => ' + q + 'APPEND' + q
    + ', end_of_line => ' + q + 'LF' + q + ')';
  exec sql prepare w1 from :s;
  for i = 1 to nl;
    wline = lines(i);
    exec sql execute w1 using :fname, :wline;
  endfor;
  exec sql commit;
  return;

  // the rows of every table, each counted on its own connection
  begsr counts;
    for k = 1 to nc;
      curRdb = cRdb(k);
      exec sql set connection :curRdb;
      if cTab(k) <> '';
        n = one('   count on ' + nameOf(cKind(k)) : 'select count(*) from ' + cTab(k));
        say('   ' + nameOf(cKind(k)) + ': ' + %char(n) + ' rows');
      endif;
    endfor;
    exec sql set connection :localRdb;
    n = one('   count on local Db2' : 'select count(*) from ' + locTab);
    say('   local Db2: ' + %char(n) + ' rows');
    exec sql commit;
  endsr;
end-proc;
