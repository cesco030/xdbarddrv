**free
// XDBALW - allowed users of an XDB connection profile (module, no main).
//
// A profile can name the user profiles and group profiles that may connect
// with it. The names are option allowed_users of the profile (file XDBPRO),
// separated by blanks; no option, or *ALL, lets everybody connect. The
// driver does the check at CONNECT (xdbprf_user_in in xdbprfc.c). This
// module holds the rules for typing the names: on the profile display,
// which has room for three, and as the value of the option.
//
// Bound into XDBPRFM (display) and XDBT25 (batch test). No file is used
// here; the caller reads and writes the option.

ctl-opt nomain option(*srcstmt : *nodebugio);

/copy QXDBSRC,XDBALWH

dcl-ds pgmSts psds qualified;
  msgId char(7) pos(40);
end-ds;

dcl-pr runCommand extpgm('QCMDEXC');
  cmd char(300) const;
  len packed(15 : 5) const;
end-pr;

dcl-c UPPER 'ABCDEFGHIJKLMNOPQRSTUVWXYZ';
dcl-c LOWER 'abcdefghijklmnopqrstuvwxyz';
// characters that cannot be in a profile name and mean something to a command
dcl-c NOTINNAME ' ()''/*"?&=,:;<>+-%';

dcl-proc alw_nameError export;
  dcl-pi *n varchar(78);
    name char(10) const;
  end-pi;
  dcl-s cmd varchar(80);
  dcl-s i int(10);

  if name = '*ALL';
    return '';
  endif;
  if name = *blanks;
    return 'Type a user or group profile, or *ALL.';
  endif;
  for i = 1 to %len(%trimr(name));
    if %scan(%subst(name : i : 1) : NOTINNAME) > 0;
      return %trimr(name) + ' is not a profile name: type a user, a group, or *ALL.';
    endif;
  endfor;
  cmd = 'CHKOBJ OBJ(QSYS/' + %trimr(name) + ') OBJTYPE(*USRPRF)';
  monitor;
    runCommand(cmd : %len(cmd));
  on-error;
    select;
      when pgmSts.msgId = 'CPF9801';
        return 'User or group profile ' + %trimr(name) + ' does not exist on this system.';
      when pgmSts.msgId = 'CPF0001' or pgmSts.msgId = 'CPF0006';
        return %trimr(name) + ' is not a profile name: type a user, a group, or *ALL.';
      other;
        // it is there: the answer was only that it may not be looked at
        return '';
    endsl;
  endmon;
  return '';
end-proc;

dcl-proc alw_listError export;
  dcl-pi *n varchar(78);
    list varchar(200) const;
  end-pi;
  dcl-s words varchar(200) dim(*auto : 60);
  dcl-s i int(10);
  dcl-s j int(10);
  dcl-s name char(10);
  dcl-s msg varchar(78);

  if %trim(list) = '';
    return '';
  endif;
  words = %split(%xlate(LOWER : UPPER : %trim(list)) : ' ');
  for i = 1 to %elem(words);
    if %len(words(i)) > 10;
      return %subst(words(i) : 1 : 10) + '... is longer than a profile name (10).';
    endif;
    name = words(i);
    if name = '*ALL' and %elem(words) > 1;
      return '*ALL allows everybody: it cannot be mixed with names.';
    endif;
    for j = 1 to i - 1;
      if words(j) = words(i);
        return %trimr(name) + ' is in the list twice.';
      endif;
    endfor;
    msg = alw_nameError(name);
    if msg <> '';
      return msg;
    endif;
  endfor;
  return '';
end-proc;

dcl-proc alw_toNames export;
  dcl-pi *n ind;
    value varchar(200) const;
    names char(10) dim(3);
  end-pi;
  dcl-s words varchar(200) dim(*auto : 60);
  dcl-s more ind inz(*off);
  dcl-s i int(10);

  clear names;
  if %trim(value) = '';
    names(1) = '*ALL';
    return *off;
  endif;
  words = %split(%xlate(LOWER : UPPER : %trim(value)) : ' ');
  more = (%elem(words) > 3);
  for i = 1 to %elem(words);
    if %len(words(i)) > 10;
      more = *on;
    endif;
    if i <= 3;
      names(i) = words(i);
    endif;
  endfor;
  return more;
end-proc;

dcl-proc alw_check export;
  dcl-pi *n varchar(78);
    names char(10) dim(3);
    loaded char(10) dim(3) const;
    more ind const;
    bad int(10);
  end-pi;
  dcl-s i int(10);
  dcl-s j int(10);
  dcl-s msg varchar(78);

  bad = 0;
  for i = 1 to 3;
    names(i) = %xlate(LOWER : UPPER : %trim(names(i)));
  endfor;
  if names(1) = *blanks and names(2) = *blanks and names(3) = *blanks;
    // nothing typed: everybody
    names(1) = '*ALL';
  endif;
  if names(1) = loaded(1) and names(2) = loaded(2) and names(3) = loaded(3);
    return '';
  endif;
  if more;
    bad = 1;
    return 'The list has more names than these three: change it with option 7.';
  endif;
  for i = 1 to 3;
    if names(i) = *blanks;
      iter;
    endif;
    for j = 1 to 3;
      if j <> i and names(j) <> *blanks and names(i) = '*ALL';
        bad = j;
        return '*ALL allows everybody: leave the other names blank, or take *ALL away.';
      endif;
      if j < i and names(j) = names(i);
        bad = i;
        return 'The same name is there twice.';
      endif;
    endfor;
    msg = alw_nameError(names(i));
    if msg <> '';
      bad = i;
      return msg;
    endif;
  endfor;
  return '';
end-proc;

dcl-proc alw_value export;
  dcl-pi *n varchar(200);
    names char(10) dim(3) const;
  end-pi;
  dcl-s value varchar(200) inz('');
  dcl-s i int(10);

  for i = 1 to 3;
    if names(i) = '*ALL';
      return '';
    endif;
    if names(i) <> *blanks;
      if value <> '';
        value += ' ';
      endif;
      value += %trim(names(i));
    endif;
  endfor;
  return value;
end-proc;
