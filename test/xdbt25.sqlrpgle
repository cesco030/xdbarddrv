**free
// XDBT25 - batch test of module XDBALW: the rules of the allowed users of
// a connection profile, as the profile display and option allowed_users
// apply them. Uses the user of the job and QSECOFR as profiles that exist
// and XDBNOSUCH1 as one that does not. Nothing is changed.
//   CALL XDBT25 PARM('<output file>')
// Every line ends with ok or WRONG.
// compile: CRTSQLRPGI OBJTYPE(*MODULE) COMMIT(*NONE), bound with XDBALW
ctl-opt main(pMain);

/copy QXDBSRC,XDBALWH

dcl-s lines varchar(300) dim(120);
dcl-s nl int(10) inz(0);
dcl-s wrong int(10) inz(0);

// want = '' : got must be empty; otherwise got must contain want
dcl-proc expect;
  dcl-pi *n;
    label varchar(70) const;
    got varchar(200) const;
    want varchar(100) const;
  end-pi;
  dcl-s ok ind;

  if want = '';
    ok = (got = '');
  else;
    ok = (%scan(want : got) > 0);
  endif;
  if not ok;
    wrong += 1;
  endif;
  if nl < %elem(lines);
    nl += 1;
    lines(nl) = label + ' -> [' + got + ']';
    if ok;
      lines(nl) += '  ok';
    else;
      lines(nl) += '  WRONG, wanted [' + want + ']';
    endif;
  endif;
end-proc;

dcl-proc show;
  dcl-pi *n varchar(40);
    names char(10) dim(3) const;
  end-pi;
  return %trimr(names(1)) + '|' + %trimr(names(2)) + '|' + %trimr(names(3));
end-proc;

// alw_check on three names typed; returns message, names and field as text
dcl-proc check;
  dcl-pi *n varchar(200);
    n1 char(10) const;
    n2 char(10) const;
    n3 char(10) const;
    l1 char(10) const;
    l2 char(10) const;
    l3 char(10) const;
    more ind const;
  end-pi;
  dcl-s names char(10) dim(3);
  dcl-s loaded char(10) dim(3);
  dcl-s bad int(10);
  dcl-s msg varchar(78);

  names(1) = n1;
  names(2) = n2;
  names(3) = n3;
  loaded(1) = l1;
  loaded(2) = l2;
  loaded(3) = l3;
  msg = alw_check(names : loaded : more : bad);
  return 'field ' + %char(bad) + ' ' + show(names) + ' ' + msg;
end-proc;

dcl-proc toNames;
  dcl-pi *n varchar(200);
    value varchar(200) const;
  end-pi;
  dcl-s names char(10) dim(3);
  dcl-s more ind;

  more = alw_toNames(value : names);
  if more;
    return 'more ' + show(names);
  endif;
  return 'all shown ' + show(names);
end-proc;

dcl-proc value3;
  dcl-pi *n varchar(200);
    n1 char(10) const;
    n2 char(10) const;
    n3 char(10) const;
  end-pi;
  dcl-s names char(10) dim(3);

  names(1) = n1;
  names(2) = n2;
  names(3) = n3;
  return '<' + alw_value(names) + '>';
end-proc;

dcl-proc pMain;
  dcl-pi *n extpgm('XDBT25');
    pFile char(32) const;
  end-pi;
  dcl-s me char(10);
  dcl-s meLow char(10);
  dcl-s fname varchar(60);
  dcl-s wline varchar(300);
  dcl-s wmode varchar(10) inz('REPLACE');
  dcl-s i int(10);
  dcl-c UP 'ABCDEFGHIJKLMNOPQRSTUVWXYZ';
  dcl-c LO 'abcdefghijklmnopqrstuvwxyz';

  exec sql set option closqlcsr = *endmod;
  exec sql values user into :me;
  meLow = %xlate(UP : LO : me);
  fname = %trim(pFile);

  // one name
  expect('name *ALL' : alw_nameError('*ALL') : '');
  expect('name of the job user' : alw_nameError(me) : '');
  expect('name QSECOFR' : alw_nameError('QSECOFR') : '');
  expect('name that does not exist' : alw_nameError('XDBNOSUCH1') : 'does not exist');
  expect('name with a blank inside' : alw_nameError('AB CD') : 'not a profile name');
  expect('name with a parenthesis' : alw_nameError('AB)CD') : 'not a profile name');
  expect('name with a quote' : alw_nameError('AB''CD') : 'not a profile name');
  expect('name with a slash' : alw_nameError('QSYS/AB') : 'not a profile name');
  expect('name *NONE' : alw_nameError('*NONE') : 'not a profile name');
  expect('name starting with a digit' : alw_nameError('1ABC') : 'not a profile name');
  expect('blank name' : alw_nameError(' ') : 'Type a user');

  // the value of option allowed_users
  expect('list empty' : alw_listError('') : '');
  expect('list *ALL' : alw_listError('*ALL') : '');
  expect('list *all' : alw_listError('*all') : '');
  expect('list user and QSECOFR' : alw_listError(%trim(me) + ' QSECOFR') : '');
  expect('list in lower case, two blanks' : alw_listError(%trim(meLow) + '  qsecofr') : '');
  expect('list *ALL with a name' : alw_listError('*ALL QSECOFR') : 'cannot be mixed');
  expect('list name with *ALL' : alw_listError('QSECOFR *ALL') : 'cannot be mixed');
  expect('list with a missing profile' : alw_listError('QSECOFR XDBNOSUCH1') : 'does not exist');
  expect('list with a long word' : alw_listError('QSECOFR XDBNOSUCHNAME1') : 'longer than');
  expect('list with a name twice' : alw_listError('QSECOFR ' + %trim(me) + ' QSECOFR')
         : 'twice');

  // the option as the three fields of the display
  expect('fields of no option' : toNames('') : 'all shown *ALL||');
  expect('fields of *all' : toNames('*all') : 'all shown *ALL||');
  expect('fields of two names' : toNames('qsecofr  QPGMR') : 'all shown QSECOFR|QPGMR|');
  expect('fields of three names' : toNames('A B C') : 'all shown A|B|C');
  expect('fields of four names' : toNames('A B C D') : 'more A|B|C');
  expect('fields of a long word' : toNames('A XDBNOSUCHNAME1') : 'more A|XDBNOSUCHN|');

  // the three fields as they are typed
  expect('typed nothing' : check(' ' : ' ' : ' ' : '*ALL' : ' ' : ' ' : *off)
         : 'field 0 *ALL|| ');
  expect('typed nothing over a list' : check(' ' : ' ' : ' ' : 'QSECOFR' : ' ' : ' ' : *off)
         : 'field 0 *ALL|| ');
  expect('typed user and QSECOFR' : check(me : 'QSECOFR' : ' ' : '*ALL' : ' ' : ' ' : *off)
         : 'field 0 ' + %trimr(me) + '|QSECOFR| ');
  expect('typed lower case, leading blank'
         : check(' qsecofr' : ' ' : ' ' : '*ALL' : ' ' : ' ' : *off)
         : 'field 0 QSECOFR|| ');
  expect('typed in the second field only' : check(' ' : 'QSECOFR' : ' ' : '*ALL' : ' ' : ' ' : *off)
         : 'field 0 |QSECOFR| ');
  expect('typed a missing profile'
         : check('QSECOFR' : 'XDBNOSUCH1' : ' ' : '*ALL' : ' ' : ' ' : *off)
         : 'field 2 QSECOFR|XDBNOSUCH1| User or group profile XDBNOSUCH1 does not exist');
  expect('typed *ALL and a name' : check('*ALL' : 'QSECOFR' : ' ' : '*ALL' : ' ' : ' ' : *off)
         : 'field 2 *ALL|QSECOFR| *ALL allows everybody');
  expect('typed a name and *ALL' : check('QSECOFR' : ' ' : '*ALL' : '*ALL' : ' ' : ' ' : *off)
         : 'field 1 QSECOFR||*ALL *ALL allows everybody');
  expect('typed a name twice' : check('QSECOFR' : me : 'qsecofr' : '*ALL' : ' ' : ' ' : *off)
         : 'field 3 QSECOFR|' + %trimr(me) + '|QSECOFR The same name');
  expect('not touched, name gone since'
         : check('XDBNOSUCH1' : ' ' : ' ' : 'XDBNOSUCH1' : ' ' : ' ' : *off)
         : 'field 0 XDBNOSUCH1|| ');
  expect('longer list, not touched' : check('A' : 'B' : 'C' : 'A' : 'B' : 'C' : *on)
         : 'field 0 A|B|C ');
  expect('longer list, changed' : check('A' : 'B' : 'QSECOFR' : 'A' : 'B' : 'C' : *on)
         : 'field 1 A|B|QSECOFR The list has more names');

  // the value stored for the three fields
  expect('value of *ALL' : value3('*ALL' : ' ' : ' ') : '<>');
  expect('value of one name' : value3('QSECOFR' : ' ' : ' ') : '<QSECOFR>');
  expect('value of first and third' : value3('QSECOFR' : ' ' : 'QPGMR') : '<QSECOFR QPGMR>');
  expect('value of second only' : value3(' ' : 'QPGMR' : ' ') : '<QPGMR>');
  expect('value of three' : value3('A' : 'B' : 'C') : '<A B C>');

  if nl < %elem(lines);
    nl += 1;
    lines(nl) = %char(nl - 1) + ' checks, ' + %char(wrong) + ' wrong';
  endif;
  for i = 1 to nl;
    wline = lines(i);
    exec sql call qsys2.ifs_write(path_name => :fname, line => :wline,
                                  overwrite => :wmode, end_of_line => 'LF');
    wmode = 'APPEND';
  endfor;
  return;
end-proc;
