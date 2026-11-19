**free
// XDBPROC - sample: stored procedures of a MySQL (or MariaDB) server from an RPG program.
//   CALL XDBPROC PARM('<rdb>' '<database>' '<steps>' '<output file>')   (literals of 32 at most)
//     database: the database (schema) to work in. A connection whose profile names none goes
//               into it (USE); a connection that is in another one creates, runs and drops
//               nothing. *TEST = whatever database the connection is in, when its name has
//               "test" in it.
//     steps:    letters, done in this order: I look, C create the sample procedures, R run
//               them, D drop them again
//   The procedures, all named xdb_demo_...:
//     xdb_demo_add(in a, in b, out total)   an output parameter
//     xdb_demo_rows(in p_n)                 returns a result set
//     xdb_demo_fill(in p_n)                 fills the temporary table xdb_demo_out
//   What the program shows:
//     1. values go in as parameter markers; a value comes out through a variable of the
//        session, read with a query (an EXECUTE hands nothing back);
//     2. the same without markers (EXECUTE IMMEDIATE);
//     3. the rows of a procedure: PREPARE the CALL, DECLARE a cursor for it, OPEN, FETCH.
//        The procedure runs at the OPEN and its first result set is the cursor;
//     4. the other way to rows: the procedure fills a temporary table and a query reads it
//        (for many rows, and for looking at them in STRSQL, which shows no rows of a CALL).
//   compile: CRTSQLRPGI ... COMMIT(*CHG) OPTION(*SQL) RDB(*LOCAL) CVTCCSID(*JOB)   (build/xproc.sh)
ctl-opt main(pMain) dftactgrp(*no) actgrp(*new);

dcl-c Q '''';
dcl-s lines varchar(600) dim(150);
dcl-s nl int(10) inz(0);

// one line of the report: what was done, SQLCODE, SQLSTATE, and the text of an error
dcl-proc note;
  dcl-pi *n;
    what varchar(300) const;
  end-pi;
  dcl-s code int(10);
  dcl-s state char(5);
  dcl-s text varchar(300) inz('');

  code = sqlcod;
  state = sqlstt;
  exec sql set option closqlcsr = *endmod;
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
end-proc;

dcl-proc say;
  dcl-pi *n;
    text varchar(500) const;
  end-pi;
  if nl < %elem(lines);
    nl += 1;
    lines(nl) = text;
  endif;
end-proc;

// a statement without a result
dcl-proc run;
  dcl-pi *n;
    stm varchar(1000) const;
  end-pi;
  dcl-s s varchar(1000);

  s = stm;
  exec sql execute immediate :s;
  note(s);
end-proc;

// a query of one text column: every row is a line of the report; gives the first value
dcl-proc show;
  dcl-pi *n varchar(200);
    label varchar(80) const;
    stm varchar(1000) const;
  end-pi;
  dcl-s s varchar(1000);
  dcl-s v varchar(200);
  dcl-s vi int(5);
  dcl-s first varchar(200) inz('');
  dcl-s rows int(10) inz(0);

  exec sql declare c1 cursor for q1;
  s = stm;
  exec sql prepare q1 from :s;
  if sqlcod < 0;
    note(label + ': ' + s + ' (prepare)');
    return '';
  endif;
  exec sql open c1;
  if sqlcod < 0;
    note(label + ': ' + s + ' (open)');
    return '';
  endif;
  dow 1 = 1;
    exec sql fetch c1 into :v :vi;
    if sqlcod <> 0;
      leave;
    endif;
    rows += 1;
    if vi < 0;
      v = '(null)';
    endif;
    if rows = 1;
      first = v;
    endif;
    say('   ' + label + ': ' + v);
  enddo;
  if sqlcod < 0;
    note(label + ' (fetch)');
  endif;
  exec sql close c1;
  if rows = 0;
    say('   ' + label + ': no rows');
  endif;
  return first;
end-proc;

// the error the driver kept last, in full: Db2 itself shows 70 characters of a message
dcl-proc lastError;
  dcl-s s varchar(60) inz('XDB LAST ERROR');
  dcl-s code int(10);
  dcl-s state char(5);
  dcl-s native int(10);
  dcl-s dbms varchar(16);
  dcl-s rdb varchar(18);
  dcl-s msg varchar(300);
  dcl-s i1 int(5);
  dcl-s i2 int(5);
  dcl-s i3 int(5);
  dcl-s i4 int(5);
  dcl-s i5 int(5);
  dcl-s i6 int(5);

  exec sql declare c9 cursor for q9;
  exec sql prepare q9 from :s;
  exec sql open c9;
  if sqlcod = 0;
    exec sql fetch c9 into :code :i1, :state :i2, :native :i3, :dbms :i4, :rdb :i5, :msg :i6;
    if sqlcod = 0;
      say('      the driver about it (XDB LAST ERROR): ' + %char(code) + ' ' + state
        + ', error ' + %char(native) + ' of ' + %trim(dbms) + ': ' + msg);
    endif;
  endif;
  exec sql close c9;
end-proc;

dcl-proc pMain;
  dcl-pi *n extpgm('XDBPROC');
    pRdb char(32) const;
    pDb char(32) const;
    pSteps char(32) const;
    pFile char(32) const;
  end-pi;
  dcl-s rdb char(18);
  dcl-s want varchar(32);
  dcl-s steps varchar(32);
  dcl-s fname varchar(60);
  dcl-s wline varchar(600);
  dcl-s db varchar(200);
  dcl-s s varchar(1000);
  dcl-s i int(10);
  dcl-s a int(10);
  dcl-s b int(10);
  dcl-s n int(10);
  dcl-s rn int(10);
  dcl-s rsq int(10);
  dcl-s rlb varchar(40);
  dcl-s rni int(5);
  dcl-s rsqi int(5);
  dcl-s rlbi int(5);
  dcl-s ok ind inz(*off);
  dcl-s opened ind;

  rdb = pRdb;
  want = %trim(pDb);
  steps = %trim(pSteps);
  fname = %trim(pFile);

  exec sql connect to :rdb;
  note('connect to ' + %trim(rdb));
  if sqlcod < 0;
    exsr report;
    return;
  endif;
  db = show('database of the connection' : 'select database()');
  show('server' : 'select version()');
  show('user' : 'select current_user()');
  if db = '(null)' and want <> '*TEST';
    // the profile names no database: go into the one asked for
    run('use ' + want);
    db = show('database of the connection now' : 'select database()');
  endif;
  if want = '*TEST';
    ok = (%scan('test' : db) > 0);
  else;
    ok = (db = want);
  endif;

  if %scan('I' : steps) > 0;
    show('a database with test in its name' : 'select schema_name from information_schema.schemata '
       + 'where schema_name like ' + Q + '%test%' + Q + ' order by schema_name');
    say('== demo procedures there now');
    show('procedure' : 'select concat(routine_name, ' + Q + ' (' + Q + ', lower(routine_type), '
       + Q + ', created ' + Q + ', created, ' + Q + ')' + Q + ') from information_schema.routines '
       + 'where routine_schema = database() and left(routine_name, 9) = ' + Q + 'xdb_demo_' + Q
       + ' order by routine_name');
  endif;

  if not ok and (%scan('C' : steps) > 0 or %scan('R' : steps) > 0 or %scan('D' : steps) > 0);
    say('The connection is in database "' + db + '", asked for was "' + want
      + '": nothing created, run or dropped.');
    steps = '';
  endif;

  if %scan('C' : steps) > 0;
    say('== create the demo procedures (EXECUTE IMMEDIATE, the text goes to the server as it is)');
    run('drop procedure if exists xdb_demo_add');
    run('create procedure xdb_demo_add(in a int, in b int, out total int) '
      + 'begin set total = a + b; end');
    run('drop procedure if exists xdb_demo_fill');
    run('create procedure xdb_demo_fill(in p_n int) begin '
      + 'create temporary table if not exists xdb_demo_out '
      + '(n int, square int, label varchar(20)); '
      + 'delete from xdb_demo_out; '
      + 'insert into xdb_demo_out values (p_n, p_n * p_n, concat(' + Q + 'row ' + Q + ', p_n)), '
      + '(p_n + 1, (p_n + 1) * (p_n + 1), concat(' + Q + 'row ' + Q + ', p_n + 1)); end');
    run('drop procedure if exists xdb_demo_rows');
    run('create procedure xdb_demo_rows(in p_n int) begin '
      + 'select p_n as n, p_n * p_n as square, concat(' + Q + 'row ' + Q + ', p_n) as label '
      + 'union all select p_n + 1, (p_n + 1) * (p_n + 1), concat(' + Q + 'row ' + Q
      + ', p_n + 1); end');
    exec sql commit;
    note('commit');
  endif;

  if %scan('R' : steps) > 0;
    say('== 1. input parameters as markers, the output parameter in a variable of the session');
    s = 'call xdb_demo_add(?, ?, @xdb_demo_total)';
    exec sql prepare p1 from :s;
    note('prepare: ' + s);
    a = 40;
    b = 2;
    exec sql execute p1 using :a, :b;
    note('execute with 40 and 2');
    show('@xdb_demo_total' : 'select cast(@xdb_demo_total as char)');
    a = 1000;
    b = 234;
    exec sql execute p1 using :a, :b;
    note('execute again with 1000 and 234');
    show('@xdb_demo_total' : 'select cast(@xdb_demo_total as char)');

    say('== 2. the same without markers (EXECUTE IMMEDIATE)');
    run('call xdb_demo_add(5, 6, @xdb_demo_total)');
    show('@xdb_demo_total' : 'select cast(@xdb_demo_total as char)');

    say('== 3. the rows of a procedure: a cursor on its CALL');
    exec sql declare c3 cursor for p3;
    s = 'call xdb_demo_rows(?)';
    exec sql prepare p3 from :s;
    note('prepare: ' + s);
    n = 7;
    exec sql open c3 using :n;
    opened = (sqlcod = 0);
    note('open a cursor on it with 7 (the procedure runs now)');
    if not opened;
      lastError();
    else;
      dow 1 = 1;
        exec sql fetch c3 into :rn :rni, :rsq :rsqi, :rlb :rlbi;
        if sqlcod <> 0;
          leave;
        endif;
        say('   row: ' + %char(rn) + ' | ' + %char(rsq) + ' | ' + rlb);
      enddo;
      note('fetch to the end');
      exec sql close c3;
      // the same cursor again, with another value
      n = 20;
      exec sql open c3 using :n;
      note('open the cursor again with 20');
      if sqlcod = 0;
        dow 1 = 1;
          exec sql fetch c3 into :rn :rni, :rsq :rsqi, :rlb :rlbi;
          if sqlcod <> 0;
            leave;
          endif;
          say('   row: ' + %char(rn) + ' | ' + %char(rsq) + ' | ' + rlb);
        enddo;
        note('fetch to the end');
        exec sql close c3;
      endif;
    endif;
    // executed and not opened, the procedure runs too: its rows are read and dropped
    exec sql execute p3 using :n;
    note('execute it with 20 (the rows have nowhere to go)');

    say('== 4. the other way: the procedure fills a temporary table, a query reads it');
    s = 'call xdb_demo_fill(?)';
    exec sql prepare p2 from :s;
    note('prepare: ' + s);
    n = 7;
    exec sql execute p2 using :n;
    note('execute with 7');
    show('row' : 'select concat_ws(' + Q + ' | ' + Q + ', n, square, label) '
       + 'from xdb_demo_out order by n');

    exec sql commit;
    note('commit');
  endif;

  if %scan('D' : steps) > 0;
    say('== drop the demo procedures');
    run('drop procedure if exists xdb_demo_add');
    run('drop procedure if exists xdb_demo_fill');
    run('drop procedure if exists xdb_demo_rows');
    run('drop temporary table if exists xdb_demo_out');
    exec sql commit;
    note('commit');
  endif;

  exec sql commit;
  exec sql disconnect :rdb;
  note('disconnect');
  exsr report;
  return;

  begsr report;
    exec sql connect reset;
    for i = 1 to nl;
      wline = lines(i);
      exec sql call qsys2.ifs_write(path_name => :fname, line => :wline,
                                    overwrite => 'APPEND', end_of_line => 'LF');
    endfor;
    exec sql commit;
  endsr;
end-proc;
