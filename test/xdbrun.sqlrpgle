**free
// XDBRUN - tool: runs the statements of a stream file on a database of XDBARDDRV and writes
//          down what came back - a way to try a statement of the server through Db2, as a
//          program would send it, before it goes into a program.
//   CALL XDBRUN PARM('<rdb>' '<script file>' '<output file>')   (literals of 32 at most;
//                                                                 build/xrun.sh does it)
//   Lines of the script, one statement a line, in the language of the server:
//     X <statement>            EXECUTE IMMEDIATE
//     X <statement> ## a|b     PREPARE, EXECUTE with the values as character parameters
//     Q <query> [## a|b]       PREPARE, DESCRIBE, OPEN, FETCH (60 rows at most).  "Query" is
//                              any statement that returns rows: the call of a procedure,
//                              INSERT ... RETURNING and the like too
//     D                        DESCRIBE of the statement of the last Q once more
//     C                        COMMIT
//     R                        ROLLBACK
//     # ...                    a comment
//     = text                   a line for the output
//   The output has, for every step, SQLCODE and SQLSTATE and the text of an error; for a
//   query the columns as Db2 describes them (name, SQL type, length) and the rows.  Every
//   column is fetched as text (a descriptor that asks for VARCHAR(1000)), so the values
//   are shown as Db2 converts them for the job: decimal point, date format.
//   The program runs under commitment control and ends with a ROLLBACK: a script that is
//   to keep what it did says C.
//   A statement whose columns the server tells only when it has run (the call of a MySQL
//   procedure) has none at the PREPARE; the program describes it again after the OPEN.
//   compile: CRTSQLRPGI ... COMMIT(*CHG) OPTION(*SQL) RDB(*LOCAL) CVTCCSID(*JOB)

ctl-opt main(pMain) dftactgrp(*no) actgrp(*new) option(*srcstmt : *nodebugio);

dcl-c MAXC 40;
dcl-c MAXP 10;
dcl-c MAXROWS 60;

dcl-ds var_t qualified template;
  sqltype int(5);
  sqllen int(5);
  sqlres char(12);
  sqldata pointer;
  sqlind pointer;
  sqlnamelen int(5);
  sqlname char(30);
end-ds;

// the columns of a query
dcl-ds da qualified;
  sqldaid char(8);
  sqldabc int(10);
  sqln int(5);
  sqld int(5);
  v likeds(var_t) dim(MAXC);
end-ds;
// the parameters of a statement
dcl-ds pa qualified;
  sqldaid char(8);
  sqldabc int(10);
  sqln int(5);
  sqld int(5);
  v likeds(var_t) dim(MAXP);
end-ds;

dcl-s val varchar(1000) dim(MAXC);
dcl-s ind int(5) dim(MAXC);
dcl-s pval varchar(200) dim(MAXP);

dcl-s script varchar(4000) dim(300);
dcl-s ns int(10) inz(0);
dcl-s lines varchar(2000) dim(3000);
dcl-s nl int(10) inz(0);

dcl-proc say;
  dcl-pi *n;
    text varchar(2000) const;
  end-pi;
  if nl < %elem(lines);
    nl += 1;
    lines(nl) = text;
  endif;
end-proc;

// what was done, SQLCODE, SQLSTATE and the text of an error or warning
dcl-proc note;
  dcl-pi *n;
    what varchar(300) const;
  end-pi;
  dcl-s code int(10);
  dcl-s state char(5);
  dcl-s text varchar(600) inz('');

  code = sqlcod;
  state = sqlstt;
  exec sql set option closqlcsr = *endmod;
  if code <> 0 and code <> 100;
    exec sql get diagnostics condition 1 :text = message_text;
  endif;
  if text <> '';
    say('   ' + what + ' -> ' + %char(code) + ' ' + state + '  [' + text + ']');
  else;
    say('   ' + what + ' -> ' + %char(code) + ' ' + state);
  endif;
end-proc;

// the values after " ## " go into the parameter descriptor; gives their number
dcl-proc params;
  dcl-pi *n int(10);
    stm varchar(4000);
  end-pi;
  dcl-s p int(10);
  dcl-s q int(10);
  dcl-s rest varchar(4000);
  dcl-s n int(10) inz(0);

  p = %scan(' ## ' : stm);
  if p = 0;
    return 0;
  endif;
  rest = %subst(stm : p + 4);
  stm = %subst(stm : 1 : p - 1);
  dow n < MAXP;
    q = %scan('|' : rest);
    n += 1;
    if q = 0;
      pval(n) = rest;
    else;
      pval(n) = %subst(rest : 1 : q - 1);
    endif;
    pa.v(n).sqltype = 448;
    pa.v(n).sqllen = 200;
    pa.v(n).sqldata = %addr(pval(n));
    pa.v(n).sqlind = *null;
    if q = 0;
      leave;
    endif;
    rest = %subst(rest : q + 1);
  enddo;
  pa.sqldaid = 'SQLDA';
  pa.sqldabc = 16 + 80 * MAXP;
  pa.sqln = MAXP;
  pa.sqld = n;
  return n;
end-proc;

// the description Db2 has of the prepared statement; gives the number of columns
dcl-proc showCols;
  dcl-pi *n int(10);
    moment varchar(40) const;
  end-pi;
  dcl-s i int(10);
  dcl-s k int(10);
  dcl-s l varchar(2000);

  da.sqldaid = 'SQLDA';
  da.sqldabc = 16 + 80 * MAXC;
  da.sqln = MAXC;
  da.sqld = 0;
  exec sql describe s1 into :da;
  if sqlcod <> 0;
    note('describe ' + moment);
    if da.sqld < 0 or da.sqld > MAXC;
      da.sqld = 0;
    endif;
  endif;
  if da.sqld > MAXC;
    say('   ' + %char(da.sqld) + ' columns, more than this tool takes');
    return 0;
  endif;
  l = '   columns ' + moment + ': ' + %char(da.sqld);
  for i = 1 to da.sqld;
    k = da.v(i).sqlnamelen;
    if k < 0 or k > 30;
      k = 30;
    endif;
    l += '  ' + %subst(da.v(i).sqlname : 1 : k) + ' ('
       + %char(da.v(i).sqltype) + ',' + %char(da.v(i).sqllen) + ')';
    da.v(i).sqltype = 449;
    da.v(i).sqllen = 1000;
    da.v(i).sqldata = %addr(val(i));
    da.v(i).sqlind = %addr(ind(i));
  endfor;
  say(l);
  return da.sqld;
end-proc;

dcl-proc runQuery;
  dcl-pi *n;
    text varchar(4000) const;
  end-pi;
  dcl-s stm varchar(4000);
  dcl-s np int(10);
  dcl-s nc int(10);
  dcl-s i int(10);
  dcl-s rows int(10) inz(0);
  dcl-s rc int(10);
  dcl-s l varchar(2000);

  stm = text;
  np = params(stm);
  exec sql declare c1 cursor for s1;
  exec sql prepare s1 from :stm;
  rc = sqlcod;
  if rc <> 0;
    note('prepare');
    if rc < 0;
      return;
    endif;
  endif;
  nc = showCols('at the prepare');
  if np > 0;
    exec sql open c1 using descriptor :pa;
  else;
    exec sql open c1;
  endif;
  rc = sqlcod;
  note('open');
  if rc < 0;
    return;
  endif;
  if nc = 0;
    // a statement whose columns the server tells only when it has run
    nc = showCols('after the open');
  endif;
  if nc > 0;
    dow rows < MAXROWS;
      exec sql fetch c1 using descriptor :da;
      rc = sqlcod;
      if rc <> 0;
        if rc <> 100;
          note('fetch');
        endif;
        if rc < 0 or rc = 100;
          leave;
        endif;
      endif;
      rows += 1;
      l = '   row:';
      for i = 1 to nc;
        if ind(i) < 0;
          l += ' <null>';
        else;
          l += ' [' + %trimr(val(i)) + ']';
        endif;
      endfor;
      say(l);
    enddo;
    say('   rows: ' + %char(rows));
  endif;
  exec sql close c1;
  if sqlcod <> 0;
    note('close');
  endif;
end-proc;

dcl-proc runStmt;
  dcl-pi *n;
    text varchar(4000) const;
  end-pi;
  dcl-s stm varchar(4000);
  dcl-s np int(10);
  dcl-s n int(10);

  stm = text;
  np = params(stm);
  if np = 0;
    exec sql execute immediate :stm;
    n = sqlerrd(3);
    note('execute immediate');
  else;
    exec sql prepare s2 from :stm;
    n = sqlcod;
    if n <> 0;
      note('prepare');
      if n < 0;
        return;
      endif;
    endif;
    exec sql execute s2 using descriptor :pa;
    n = sqlerrd(3);
    note('execute');
  endif;
  if n <> 0;
    say('   rows changed: ' + %char(n));
  endif;
end-proc;

dcl-proc pMain;
  dcl-pi *n extpgm('XDBRUN');
    pRdb char(32) const;
    pScript char(32) const;
    pFile char(32) const;
  end-pi;
  dcl-s rdb char(18);
  dcl-s here char(18);
  dcl-s sname varchar(60);
  dcl-s fname varchar(60);
  dcl-s wline varchar(2000);
  dcl-s one varchar(4000);
  dcl-s i int(10);
  dcl-s i2 int(10);
  dcl-s verb char(1);

  rdb = pRdb;
  sname = %trim(pScript);
  fname = %trim(pFile);

  exec sql values current server into :here;
  exec sql declare cs cursor for
    select cast(line as varchar(4000) ccsid 280)
      from table(qsys2.ifs_read(path_name => :sname, end_of_line => 'ANY')) x;
  exec sql open cs;
  if sqlcod < 0;
    note('the script ' + sname);
    exsr report;
    return;
  endif;
  dow ns < %elem(script);
    exec sql fetch cs into :one;
    if sqlcod <> 0;
      leave;
    endif;
    ns += 1;
    script(ns) = %trimr(one);
  enddo;
  exec sql close cs;
  exec sql commit;

  exec sql connect to :rdb;
  note('connect to ' + %trim(rdb));
  if sqlcod < 0;
    exsr report;
    return;
  endif;

  for i = 1 to ns;
    one = script(i);
    if %len(one) = 0;
      iter;
    endif;
    verb = %subst(one : 1 : 1);
    if verb = '#';
      iter;
    endif;
    say(one);
    select;
      when verb = '=';
      when verb = 'C';
        exec sql commit;
        note('commit');
      when verb = 'R';
        exec sql rollback;
        note('rollback');
      when verb = 'X' and %len(one) > 2;
        runStmt(%subst(one : 3));
      when verb = 'Q' and %len(one) > 2;
        runQuery(%subst(one : 3));
      when verb = 'D';
        // DESCRIBE of the statement of the last Q, whatever became of its PREPARE
        i2 = showCols('now');
        say('   describe now: sqlcode as noted above, columns ' + %char(i2));
      other;
        say('   ?');
    endsl;
  endfor;

  exec sql rollback;
  exec sql disconnect :rdb;
  note('disconnect');
  exsr report;
  return;

  begsr report;
    exec sql set connection :here;
    for i = 1 to nl;
      wline = lines(i);
      exec sql call qsys2.ifs_write(path_name => :fname, line => :wline,
                                    overwrite => 'APPEND', end_of_line => 'LF');
    endfor;
    exec sql commit;
  endsr;
end-proc;
