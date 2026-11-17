**free
// XDBT30 - large values between stream files and an XDB database (PostgreSQL): the functions
//   GET_BLOB_FROM_FILE, GET_CLOB_FROM_FILE, PUT_BLOB_TO_FILE, PUT_CLOB_TO_FILE of the driver
//   (xdb/xdblob.c).  The values are made on the server, written to files, read back into a
//   temporary table and written out again; the server compares what came back with what it
//   made.  Everything on the server is rolled back at the end; the files stay in the
//   directory given, and x30.sh compares them.
//   CALL XDBT30 PARM('<rdb>' '<directory>' '<rows>' '<output file>')   (literals of 32 at most)
//     rows: the large binary value has 16 bytes per row (250000 = 4 MB)
// compile: CRTSQLRPGI ... COMMIT(*CHG) OPTION(*SQL) RDB(*LOCAL) CVTCCSID(*JOB)
ctl-opt main(pMain) dftactgrp(*no) actgrp(*new);

dcl-s lines varchar(600) dim(90);
dcl-s nl int(10) inz(0);

// one line of the report: what was done, SQLCODE, SQLSTATE, and the text of an error
dcl-proc note;
  dcl-pi *n;
    what varchar(200) const;
  end-pi;
  dcl-s code int(10);
  dcl-s state char(5);
  dcl-s text varchar(300) inz('');
  dcl-s reason varchar(20) inz('');

  code = sqlcod;
  state = sqlstt;
  exec sql set option closqlcsr = *endmod;
  if code = -452;
    // a file could not be used: the second token of message SQL0452 is the reason code
    exec sql get diagnostics condition 1 :text = message_text, :reason = db2_ordinal_token_2;
  elseif code < 0;
    exec sql get diagnostics condition 1 :text = message_text;
  endif;
  if nl < %elem(lines);
    nl += 1;
    lines(nl) = what + ' -> ' + %char(code) + ' ' + state;
    if reason <> '';
      lines(nl) += ' reason ' + %trim(reason);
    endif;
    if text <> '';
      lines(nl) += '  [' + text + ']';
    endif;
  endif;
end-proc;

// a statement without a result
dcl-proc run;
  dcl-pi *n;
    label varchar(80) const;
    stm varchar(3000) const;
  end-pi;
  dcl-s s varchar(3000);

  s = stm;
  exec sql execute immediate :s;
  note(label);
end-proc;

// a query whose first column is a number: all its rows are fetched; gives the sum
dcl-proc num;
  dcl-pi *n int(20);
    label varchar(80) const;
    stm varchar(3000) const;
  end-pi;
  dcl-s s varchar(3000);
  dcl-s n int(20);
  dcl-s ni int(5);
  dcl-s total int(20) inz(0);
  dcl-s rows int(10) inz(0);
  dcl-s nulls int(10) inz(0);

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
  dow 1 = 1;
    exec sql fetch c1 into :n :ni;
    if sqlcod <> 0;
      leave;
    endif;
    rows += 1;
    if ni < 0;
      nulls += 1;
    else;
      total += n;
    endif;
  enddo;
  if sqlcod < 0;
    note(label + ' (fetch, after ' + %char(rows) + ' rows)');
    exec sql close c1;
    return -1;
  endif;
  exec sql close c1;
  if nl < %elem(lines);
    nl += 1;
    lines(nl) = label + ': ' + %char(total) + ' (' + %char(rows) + ' rows';
    if nulls > 0;
      lines(nl) += ', ' + %char(nulls) + ' null';
    endif;
    lines(nl) += ')';
  endif;
  return total;
end-proc;

dcl-proc pMain;
  dcl-pi *n extpgm('XDBT30');
    pRdb char(32) const;
    pDir char(32) const;
    pRows char(32) const;
    pFile char(32) const;
  end-pi;
  dcl-s rdb char(18);
  dcl-s dir varchar(40);
  dcl-s big varchar(12);
  dcl-s fname varchar(60);
  dcl-s wline varchar(600);
  dcl-s i int(10);
  dcl-s id int(10);
  dcl-s path varchar(120);
  dcl-s s varchar(3000);
  dcl-s gen varchar(200);
  dcl-s txt varchar(300);
  dcl-s uni varchar(200);
  dcl-s lo0 int(20);
  dcl-s lo9 int(20);
  dcl-s q varchar(1) inz('''');

  rdb = pRdb;
  dir = %trim(pDir);
  big = %trim(pRows);
  fname = %trim(pFile);
  // bytes made on the server: the MD5 values of 1 .. n one after the other
  gen = 'decode(string_agg(md5(i::text), ' + q + q + ' order by i), ' + q + 'hex' + q + ')';
  // text with accented letters (a grave 224, e grave 232, e acute 233 ...: made on the server,
  // so that the CCSID of this source does not matter), and text with characters no EBCDIC
  // CCSID of Europe has
  txt = 'repeat(' + q + 'Citt' + q + ' || chr(224) || ' + q + ' ' + q + ' || chr(232) || ' + q
      + ' perch' + q + ' || chr(233) || chr(236) || chr(242) || chr(249) || ' + q + '. ' + q
      + ', 15000)';
  uni = 'repeat(' + q + 'x' + q + ' || chr(8364) || chr(26085) || chr(26412), 30000)';

  exec sql connect to :rdb;
  note('connect');
  if sqlcod < 0;
    exsr report;
    return;
  endif;
  run('temporary table' : 'create temp table xdbt30 (id int primary key, body bytea, note text)');
  lo0 = num('large objects on the server before' : 'select count(*) from pg_largeobject_metadata');

  // 1. binary values from the server to files
  num('a.bin, bytes written' : 'select put_blob_to_file(' + gen + ', ' + q + dir + '/a.bin' + q
      + ') from generate_series(1, 50) as g(i)');
  num('b.bin, bytes written' : 'select put_blob_to_file(' + gen + ', ' + q + dir + '/b.bin' + q
      + ') from generate_series(1, ' + big + ') as g(i)');

  // 2. from the files into the table: the names in a host variable
  s = 'insert into xdbt30 (id, body) values(?, get_blob_from_file(?))';
  exec sql prepare s1 from :s;
  note('prepare insert with get_blob_from_file(?)');
  id = 1;
  path = dir + '/a.bin';
  exec sql execute s1 using :id, :path;
  note('insert a.bin');
  id = 2;
  path = dir + '/b.bin';
  exec sql execute s1 using :id, :path;
  note('insert b.bin');
  num('a.bin in the table is what the server made (1 = yes)' :
      'select count(*) from xdbt30 where id = 1 and md5(body) = (select md5(' + gen
      + ') from generate_series(1, 50) as g(i))');
  num('b.bin in the table is what the server made (1 = yes)' :
      'select count(*) from xdbt30 where id = 2 and md5(body) = (select md5(' + gen
      + ') from generate_series(1, ' + big + ') as g(i))');
  num('bytes of both in the table' : 'select octet_length(body) from xdbt30 where id in (1, 2)');

  // 3. and from the table to files again, one statement for both rows
  num('c1.bin and c2.bin, bytes written' : 'select put_blob_to_file(body, ' + q + dir + '/c' + q
      + ' || id || ' + q + '.bin' + q + ') from xdbt30 where id in (1, 2) order by id');

  // 4. text: written in three CCSIDs, read back, compared on the server
  num('t1208.txt (UTF-8), bytes written' :
      'select put_clob_to_file(' + txt + ', ' + q + dir + '/t1208.txt' + q + ')');
  num('t280.txt (EBCDIC Italy), bytes written' :
      'select put_clob_to_file(' + txt + ', ' + q + dir + '/t280.txt' + q + ', 16, 280)');
  num('t1252.txt (Windows), bytes written' :
      'select put_clob_to_file(' + txt + ', ' + q + dir + '/t1252.txt' + q + ', 16, 1252)');
  run('insert t1208.txt' : 'insert into xdbt30 (id, note) values(11, get_clob_from_file(' + q
      + dir + '/t1208.txt' + q + '))');
  run('insert t280.txt' : 'insert into xdbt30 (id, note) values(12, get_clob_from_file(' + q
      + dir + '/t280.txt' + q + '))');
  run('insert t1252.txt' : 'insert into xdbt30 (id, note) values(13, get_clob_from_file(' + q
      + dir + '/t1252.txt' + q + '))');
  num('texts in the table equal to the original (3 = all)' :
      'select count(*) from xdbt30 where id between 11 and 13 and note = ' + txt);
  // more text appended to the EBCDIC file: it is converted to the CCSID the file has
  num('e acute and a line feed appended to t280.txt, bytes' : 'select put_clob_to_file(chr(233)'
      + ' || chr(10), ' + q + dir + '/t280.txt' + q + ', 32)');
  run('insert t280.txt again' : 'insert into xdbt30 (id, note) values(15, get_clob_from_file('
      + q + dir + '/t280.txt' + q + '))');
  num('it is the original and what was appended (1 = yes)' :
      'select count(*) from xdbt30 where id = 15 and note = ' + txt + ' || chr(233) || chr(10)');
  // replaced without naming a CCSID: a file that exists keeps the CCSID it has
  num('t280.txt replaced, bytes written (as many as in CCSID 280)' :
      'select put_clob_to_file(' + txt + ', ' + q + dir + '/t280.txt' + q + ', 16)');
  run('insert t280.txt a third time' : 'insert into xdbt30 (id, note) values(16, '
      + 'get_clob_from_file(' + q + dir + '/t280.txt' + q + '))');
  num('it is the original again (1 = yes)' :
      'select count(*) from xdbt30 where id = 16 and note = ' + txt);
  num('u1208.txt (euro sign, Japanese), bytes written' :
      'select put_clob_to_file(' + uni + ', ' + q + dir + '/u1208.txt' + q + ')');
  run('insert u1208.txt' : 'insert into xdbt30 (id, note) values(14, get_clob_from_file(' + q
      + dir + '/u1208.txt' + q + '))');
  num('that text in the table equal to the original (1 = yes)' :
      'select count(*) from xdbt30 where id = 14 and note = ' + uni);

  // 5. file options, a null value
  num('option 8 (new file) on c1.bin, which exists' : 'select put_blob_to_file(body, ' + q + dir
      + '/c1.bin' + q + ', 8) from xdbt30 where id = 1');
  num('option 8 on new.bin' : 'select put_blob_to_file(body, ' + q + dir
      + '/new.bin' + q + ', 8) from xdbt30 where id = 1');
  num('option 32 (append) to app.txt' : 'select put_clob_to_file(' + q + 'riga' + q
      + ' || chr(10), ' + q + dir + '/app.txt' + q + ', 32)');
  num('option 32 (append) to app.txt again' : 'select put_clob_to_file(' + q + 'riga' + q
      + ' || chr(10), ' + q + dir + '/app.txt' + q + ', 32)');
  num('a null value to null.bin' : 'select put_blob_to_file(body, ' + q + dir + '/null.bin' + q
      + ') from xdbt30 where id = 11');

  // 6. what goes wrong
  run('a file that is not there' : 'insert into xdbt30 (id, body) values(21, get_blob_from_file('
      + q + dir + '/nofile.bin' + q + '))');
  id = 22;
  path = dir + '/nofile2.bin';
  exec sql execute s1 using :id, :path;
  note('the same with the name in a host variable');
  run('a directory as file' : 'insert into xdbt30 (id, body) values(23, get_blob_from_file('
      + q + dir + q + '))');
  num('a directory that is not there' : 'select put_blob_to_file(body, ' + q + dir
      + '/nodir/x.bin' + q + ') from xdbt30 where id = 1');
  run('a key that exists, after the file was sent' :
      'insert into xdbt30 (id, body) values(2, get_blob_from_file(' + q + dir + '/b.bin' + q
      + '))');
  run('the function in the wrong place' : 'update xdbt30 set note = put_clob_to_file(note, '
      + q + dir + '/x.txt' + q + ')');
  num('rows in the table after the errors (8)' : 'select count(*) from xdbt30');

  lo9 = num('large objects on the server after' : 'select count(*) from pg_largeobject_metadata');
  if nl < %elem(lines);
    nl += 1;
    lines(nl) = 'large objects left behind by this run: ' + %char(lo9 - lo0);
  endif;
  exec sql rollback;
  note('rollback');
  exec sql disconnect :rdb;
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
