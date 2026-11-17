**free
// XDBPRFM - Work with XDB connection profiles (command WRKXDBPRF).
//
// A profile tells the XDB driver how to reach the external database behind
// a relational database (RDB) name: database type, host, port, database,
// user, encryption and the driver options.  Profiles are records of file
// XDBPRF, their options records of XDBPRO; the password is an entry of
// validation list XDBVLDL and is never shown.  All objects are taken from
// the library this program is in.
//
// The program is also the installer.  F9 shows the Setup display: the
// checks of module XDBSETUP for this library and, for what is to do, the
// CL commands, which run only after the user has seen and confirmed them.
// When the profile files are not there yet (objects just restored from a
// package) the Setup display comes first.
//
// Help: F1 on every display.  The display file names the help modules of
// panel group WRKXDBPRF, which the system looks for through the library
// list.  So that the help is found whatever the library list of the job
// is, the program puts its own library at the end of the list while it
// runs and takes it out again when it ends, however it ends.
//
// Built as module XDBPRFM + module XDBSETUP (checks) + module XDBPRFC
// (password store, ILE C), CRTPGM ... ACTGRP(XDBPRFM).  It binds no service
// program of XDB, so it starts from any library before anything is set up.

ctl-opt option(*srcstmt : *nodebugio)
        text('XDB - work with connection profiles');

dcl-f XDBPRFD workstn indds(ind) extfile(dspName) usropn
              sfile(SFL1 : rrn1) sfile(SFL2 : rrn2) sfile(PMTSFL : rrnP)
              sfile(SFL3 : rrn3);
dcl-f XDBPRF usage(*update : *delete : *output) keyed extfile(prfName) usropn;
dcl-f XDBPRO usage(*update : *delete : *output) keyed extfile(proName) usropn;
dcl-f XDBKWD usage(*input) keyed extfile(kwdName) usropn;

dcl-ds pgmSts psds qualified;
  msgId char(7) pos(40);
  pgmLib char(10) pos(81);
  msgData char(80) pos(91);
  user char(10) pos(358);
end-ds;

dcl-ds ind len(99);
  exitKey ind pos(3);
  promptKey ind pos(4);
  refreshKey ind pos(5);
  addKey ind pos(6);
  setupKey ind pos(9);
  f10Key ind pos(10);
  cancelKey ind pos(12);
  sfl1Dsp ind pos(31);
  sfl1DspCtl ind pos(32);
  sfl1Clr ind pos(33);
  sfl1End ind pos(34);
  pmtDsp ind pos(35);
  pmtDspCtl ind pos(36);
  pmtClr ind pos(37);
  pmtEnd ind pos(38);
  keyProtect ind pos(40);
  allProtect ind pos(41);
  sflNextChange ind pos(44);
  s1OptErr ind pos(45);
  s2OptErr ind pos(46);
  o1KeyProtect ind pos(47);
  s3OptErr ind pos(48);
  errFields char(18) pos(51);
  errRdb ind pos(51);
  errType ind pos(52);
  errSts ind pos(53);
  errHost ind pos(54);
  errPort ind pos(55);
  errDbnm ind pos(56);
  errUser ind pos(57);
  errTls ind pos(58);
  errTmo ind pos(59);
  errCcsid ind pos(60);
  errTxtl ind pos(61);
  errNscl ind pos(62);
  errTrc ind pos(63);
  errOKey ind pos(64);
  errOVal ind pos(65);
  errAlw ind dim(3) pos(66);
  sfl2Dsp ind pos(71);
  sfl2DspCtl ind pos(72);
  sfl2Clr ind pos(73);
  sfl2End ind pos(74);
  sfl3Dsp ind pos(75);
  sfl3DspCtl ind pos(76);
  sfl3Clr ind pos(77);
  sfl3End ind pos(78);
  stInOrder ind pos(81);
  stToDo ind pos(82);
  stFailing ind pos(83);
  stRemark ind pos(84);
  msgIsError ind pos(90);
end-ds;

dcl-s dspName char(21);
dcl-s prfName char(21);
dcl-s proName char(21);
dcl-s kwdName char(21);
dcl-s myLib char(10);
dcl-s rrn1 int(10);
dcl-s rrn2 int(10);
dcl-s rrnP int(10);
dcl-s rrn3 int(10);
dcl-s setupOwner char(10);
dcl-s listMsg varchar(78) inz('');
dcl-s listMsgErr ind inz(*off);
dcl-s libAdded ind inz(*off);   // this program put myLib into the library list

// Long entries are two or more plain fields on the screen, one below the
// other (not every 5250 emulator shows continued-entry fields); the program
// works with the whole value.
dcl-s D1HOST char(120);
dcl-s W1PWD1 char(120);
dcl-s W1PWD2 char(120);
dcl-s O1VAL char(200);

// Allowed users: the three names of the screen as they were read, and
// whether the option holds more than the screen can show.
dcl-s alwLoaded char(10) dim(3);
dcl-s alwMore ind inz(*off);
dcl-c ALWKEY 'allowed_users';

// values offered by F4, filled before promptList() is called
dcl-s pmtVal char(20) dim(60);
dcl-s pmtTxt char(34) dim(60);
dcl-s pmtCount int(10);

dcl-c UPPER 'ABCDEFGHIJKLMNOPQRSTUVWXYZ';
dcl-c LOWER 'abcdefghijklmnopqrstuvwxyz';
dcl-c NAMECHARS 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_';

// password store in module XDBPRFC: 0 = done, otherwise an error number
dcl-pr setPassword int(10) extproc('xdbprf_setpwd');
  lib char(10) const;
  rdb char(18) const;
  pwd pointer value;
  len int(10) value;
end-pr;
dcl-pr removePassword int(10) extproc('xdbprf_delpwd');
  lib char(10) const;
  rdb char(18) const;
end-pr;

dcl-pr runCommand extpgm('QCMDEXC');
  cmd char(300) const;
  len packed(15 : 5) const;
end-pr;

// checks and repairs of the installation: module XDBSETUP
/copy QXDBSRC,XDBSETUPH
// rules of the allowed users of a profile: module XDBALW
/copy QXDBSRC,XDBALWH
dcl-c WINLINES 12;
dcl-c WINWIDTH 68;

// ------------------------------------------------------------------ main

myLib = pgmSts.pgmLib;
mainLine();
*inlr = *on;
return;

dcl-proc mainLine;
  dcl-s cmd varchar(60);

dspName = %trim(myLib) + '/XDBPRFD';
prfName = %trim(myLib) + '/XDBPRF';
proName = %trim(myLib) + '/XDBPRO';
kwdName = %trim(myLib) + '/XDBKWD';
helpLibrary();
open XDBPRFD;
setupOwner = xs_defaultOwner(myLib);
if xs_missing(myLib);
  // objects just restored: the profile files are made by the setup
  setupScreen('The profile files are not there yet. Use F10, or option 1 line by line.');
  if xs_missing(myLib);
    close XDBPRFD;
    return;
  endif;
endif;
open XDBPRF;
open XDBPRO;
open XDBKWD;

C1USER = pgmSts.user;
C1LIB = myLib;
C1POS = *blanks;
loadList();
if xs_open(myLib) > 0;
  listMsg = 'The setup of XDB in this library is not complete. F9 shows what is to do.';
endif;

dow *on;
  F1MSG = listMsg;
  msgIsError = listMsgErr;
  listMsg = '';
  listMsgErr = *off;
  write FOOT1;
  exfmt CTL1;
  select;
    when exitKey or cancelKey;
      leave;
    when refreshKey;
      C1POS = *blanks;
      loadList();
    when addKey;
      editProfile('A' : *blanks);
      loadList();
    when setupKey;
      setupScreen('');
      loadList();
    other;
      listOptions();
  endsl;
enddo;

close XDBPRFD;
close XDBPRF;
close XDBPRO;
close XDBKWD;
return;

on-exit;
  // also when the program is cancelled or ends with an error
  if libAdded;
    libAdded = *off;
    cmd = 'RMVLIBLE LIB(' + %trim(myLib) + ')';
    callp(e) runCommand(cmd : %len(cmd));
  endif;
end-proc;

// The help panel group is looked for through the library list (the display
// file cannot name a library that is known only at run time).  A library
// that is in the list already, in whatever part of it, is left alone: then
// nothing is changed and nothing is taken out at the end.
dcl-proc helpLibrary;
  dcl-s cmd varchar(60);

  if inLibraryList(myLib);
    return;
  endif;
  cmd = 'ADDLIBLE LIB(' + %trim(myLib) + ') POSITION(*LAST)';
  callp(e) runCommand(cmd : %len(cmd));
  libAdded = not %error();
end-proc;

// Is the library in the library list of the job?  QUSRJOBI format JOBI0700:
// the numbers of system, product, current and user libraries at 64, 68, 72
// and 76, then the names in that order, eleven bytes each.
dcl-proc inLibraryList;
  dcl-pi *n ind;
    lib char(10) const;
  end-pi;
  dcl-pr retrieveJobInfo extpgm('QUSRJOBI');
    receiver char(3100) options(*varsize);
    length int(10) const;
    format char(8) const;
    job char(26) const;
    internalId char(16) const;
  end-pr;
  dcl-ds info len(3100) qualified;
    systemLibs int(10) pos(65);
    productLibs int(10) pos(69);
    currentLibs int(10) pos(73);
    userLibs int(10) pos(77);
    names char(3020) pos(81);
  end-ds;
  dcl-s i int(10);
  dcl-s n int(10);

  info = *allx'00';
  callp(e) retrieveJobInfo(info : %size(info) : 'JOBI0700' : '*' : *blanks);
  if %error();
    return *off;
  endif;
  n = info.systemLibs + info.productLibs + info.currentLibs + info.userLibs;
  for i = 1 to n;
    if i * 11 > %size(info.names);
      leave;
    endif;
    if %subst(info.names : (i - 1) * 11 + 1 : 10) = lib;
      return *on;
    endif;
  endfor;
  return *off;
end-proc;

// ------------------------------------------------------- list of profiles

dcl-proc loadList;
  sfl1Clr = *on;
  sfl1Dsp = *off;
  sfl1DspCtl = *off;
  write CTL1;
  sfl1Clr = *off;
  rrn1 = 0;
  s1OptErr = *off;
  C1POS = %xlate(LOWER : UPPER : C1POS);
  setll C1POS XDBPRFR;
  read(n) XDBPRFR;
  dow not %eof(XDBPRF) and rrn1 < 9999;
    S1OPT = ' ';
    S1RDB = PRRDB;
    S1TYPE = PRTYPE;
    S1HOST = PRHOST;
    S1DBNM = PRDBNM;
    S1STS = PRSTS;
    rrn1 += 1;
    write SFL1;
    read(n) XDBPRFR;
  enddo;
  sfl1Dsp = (rrn1 > 0);
  sfl1DspCtl = *on;
  sfl1End = *on;
  S1RRN = 1;
  if rrn1 = 0 and listMsg = '';
    if C1POS = *blanks;
      listMsg = 'There are no connection profiles. Press F6 to add one.';
    else;
      listMsg = 'No profile from this name on. Press F5 to see all.';
    endif;
  endif;
end-proc;

// Options typed in the list: collect them first, then run them one by one.
dcl-proc listOptions;
  dcl-s selRdb char(18) dim(50);
  dcl-s selOpt char(1) dim(50);
  dcl-s n int(10) inz(0);
  dcl-s i int(10);
  dcl-s bad ind inz(*off);

  if rrn1 > 0;
    readc SFL1;
    dow not %eof(XDBPRFD);
      if S1OPT <> ' ';
        if %scan(S1OPT : '2345789') = 0;
          bad = *on;
          s1OptErr = *on;
          S1RRN = rrn1;
        elseif n < %elem(selRdb);
          n += 1;
          selRdb(n) = S1RDB;
          selOpt(n) = S1OPT;
        endif;
        // keep what was typed: it is read again after a correction
        sflNextChange = *on;
        update SFL1;
        sflNextChange = *off;
        s1OptErr = *off;
      endif;
      readc SFL1;
    enddo;
  endif;
  if bad;
    listMsg = 'Option not valid. Use one of the options shown above the list.';
    listMsgErr = *on;
    return;
  endif;

  for i = 1 to n;
    select;
      when selOpt(i) = '2';
        editProfile('C' : selRdb(i));
      when selOpt(i) = '3';
        editProfile('K' : selRdb(i));
      when selOpt(i) = '4';
        deleteProfile(selRdb(i));
      when selOpt(i) = '5';
        editProfile('D' : selRdb(i));
      when selOpt(i) = '7';
        workWithOptions(selRdb(i));
      when selOpt(i) = '8';
        addRdbEntry(selRdb(i));
      when selOpt(i) = '9';
        testConnection(selRdb(i));
    endsl;
    if exitKey;
      leave;
    endif;
  endfor;
  exitKey = *off;
  loadList();
end-proc;

// ------------------------------------------------------------ one profile

// mode: A add, C change, K copy of an existing profile, D display
dcl-proc editProfile;
  dcl-pi *n;
    startMode char(1) const;
    rdb char(18) const;
  end-pi;
  dcl-s mode char(1);
  dcl-s copyFrom char(18);

  mode = startMode;
  copyFrom = rdb;
  D1MSG = *blanks;
  msgIsError = *off;
  errFields = *all'0';

  if mode = 'A';
    clear XDBPRFR;
    PRTYPE = 'PG';
    PRSTS = 'A';
    PRPWDS = 'N';
    PRTLS = '*PREFER';
    PRTMO = 10;
    PRTXTL = 1000;
    PRNSCL = 6;
    PRTRC = 'N';
  else;
    chain(n) rdb XDBPRFR;
    if not %found(XDBPRF);
      listMsg = 'Profile ' + %trim(rdb) + ' is no longer there.';
      listMsgErr = *on;
      return;
    endif;
    if mode = 'K';
      PRRDB = *blanks;
      PRPWDS = 'N';
    endif;
  endif;
  recordToScreen();
  if mode = 'A';
    loadAllowed(*blanks);
  else;
    loadAllowed(rdb);
  endif;
  if alwMore;
    D1MSG = 'More than three allowed users: option 7 shows all (allowed_users).';
  endif;

  dow *on;
    keyProtect = (mode = 'C' or mode = 'D');
    allProtect = (mode = 'D');
    select;
      when mode = 'A';
        D1TITLE = '        Add Connection Profile';
        D1KEYS = 'F1=Help   F3=Exit   F4=Prompt   F12=Cancel';
      when mode = 'K';
        D1TITLE = '       Copy Connection Profile';
        D1KEYS = 'F1=Help   F3=Exit   F4=Prompt   F12=Cancel';
      when mode = 'C';
        D1TITLE = '      Change Connection Profile';
        D1KEYS = 'F1=Help   F3=Exit   F4=Prompt   F10=Password   F12=Cancel';
      other;
        D1TITLE = '      Display Connection Profile';
        D1KEYS = 'F1=Help   F3=Exit   F12=Cancel';
    endsl;
    D1HOST1 = %subst(D1HOST : 1 : 60);
    D1HOST2 = %subst(D1HOST : 61 : 60);
    exfmt PRFDTL;
    D1HOST = D1HOST1 + D1HOST2;
    D1MSG = *blanks;
    msgIsError = *off;
    errFields = *all'0';

    if exitKey or cancelKey;
      return;
    endif;
    if mode = 'D';
      return;
    endif;
    if promptKey;
      promptProfileField();
      iter;
    endif;
    if f10Key;
      if mode = 'C';
        passwordWindow(D1RDB);
      else;
        D1MSG = 'Press Enter to save the profile first, then F10 for the password.';
      endif;
      iter;
    endif;

    if not profileIsValid(mode);
      msgIsError = *on;
      iter;
    endif;

    if mode = 'C';
      chain D1RDB XDBPRFR;
      if not %found(XDBPRF);
        D1MSG = 'The profile was deleted by someone else.';
        msgIsError = *on;
        iter;
      endif;
      screenToRecord();
      PRCHGU = pgmSts.user;
      PRCHGT = %timestamp();
      update XDBPRFR;
      saveAllowed(D1RDB);
      listMsg = 'Profile ' + %trim(D1RDB) + ' changed.';
      return;
    endif;

    // add or copy
    clear XDBPRFR;
    PRRDB = D1RDB;
    PRPWDS = 'N';
    screenToRecord();
    PRCRTU = pgmSts.user;
    PRCRTT = %timestamp();
    PRCHGU = PRCRTU;
    PRCHGT = PRCRTT;
    write(e) XDBPRFR;
    if %error();
      D1MSG = 'A profile with this RDB name exists already.';
      errRdb = *on;
      msgIsError = *on;
      iter;
    endif;
    if mode = 'K';
      copyOptions(copyFrom : D1RDB);
    endif;
    saveAllowed(D1RDB);
    // stay on the profile so that the password can be set right away
    mode = 'C';
    D1PWDT = 'Not stored';
    D1CHG = %trim(PRCHGU) + '  ' + %char(PRCHGT);
    D1MSG = 'Profile added. F10 sets the password; Enter or F12 goes back.';
    listMsg = 'Profile ' + %trim(D1RDB) + ' added.';
  enddo;
end-proc;

dcl-proc recordToScreen;
  D1RDB = PRRDB;
  D1TEXT = PRTEXT;
  D1TYPE = PRTYPE;
  D1STS = PRSTS;
  D1HOST = PRHOST;
  D1PORT = PRPORT;
  D1DBNM = PRDBNM;
  D1USER = PRUSER;
  D1TLS = PRTLS;
  D1TMO = PRTMO;
  D1CCSID = PRCCSID;
  D1TXTL = PRTXTL;
  D1NSCL = PRNSCL;
  D1TRC = PRTRC;
  if PRPWDS = 'Y';
    D1PWDT = 'Stored';
  else;
    D1PWDT = 'Not stored';
  endif;
  if PRCHGU = *blanks;
    D1CHG = *blanks;
  else;
    D1CHG = %trim(PRCHGU) + '  ' + %char(PRCHGT);
  endif;
end-proc;

// everything but the key, the password flag and the audit fields
dcl-proc screenToRecord;
  PRTEXT = D1TEXT;
  PRTYPE = D1TYPE;
  PRSTS = D1STS;
  PRHOST = D1HOST;
  PRPORT = D1PORT;
  PRDBNM = D1DBNM;
  PRUSER = D1USER;
  PRTLS = D1TLS;
  PRTMO = D1TMO;
  PRCCSID = D1CCSID;
  PRTXTL = D1TXTL;
  PRNSCL = D1NSCL;
  PRTRC = D1TRC;
end-proc;

// Checks the screen fields; the first error sets the message and the cursor.
dcl-proc profileIsValid;
  dcl-pi *n ind;
    mode char(1) const;
  end-pi;

  D1RDB = %xlate(LOWER : UPPER : D1RDB);
  D1TYPE = %xlate(LOWER : UPPER : D1TYPE);
  D1STS = %xlate(LOWER : UPPER : D1STS);
  D1TLS = %xlate(LOWER : UPPER : D1TLS);
  D1TRC = %xlate(LOWER : UPPER : D1TRC);
  if D1TMO = 0;
    D1TMO = 10;
  endif;
  if D1TXTL = 0;
    D1TXTL = 1000;
  endif;

  select;
    when mode <> 'C' and D1RDB = *blanks;
      errRdb = *on;
      D1MSG = 'Type the RDB name: the name used on CONNECT TO.';
    when mode <> 'C' and (%check(NAMECHARS : %trimr(D1RDB)) > 0
                          or %check(UPPER : %subst(D1RDB : 1 : 1)) > 0);
      errRdb = *on;
      D1MSG = 'RDB name: letters, digits and _ only, starting with a letter.';
    when D1TYPE <> 'PG' and D1TYPE <> 'MYSQL' and D1TYPE <> 'MSSQL';
      errType = *on;
      D1MSG = 'Database type must be PG, MYSQL or MSSQL. F4 shows the list.';
    when D1STS <> 'A' and D1STS <> 'D';
      errSts = *on;
      D1MSG = 'Status must be A (active) or D (disabled).';
    when D1HOST = *blanks;
      errHost = *on;
      D1MSG = 'Type the host name or the IP address of the database server.';
    when %scan(' ' : %trim(D1HOST)) > 0;
      errHost = *on;
      D1MSG = 'The host name cannot contain blanks. Fill the first line before the second.';
    when D1PORT > 65535;
      errPort = *on;
      D1MSG = 'The port can be 65535 at most. 0 means the default port of the type.';
    when D1TLS <> '*OFF' and D1TLS <> '*PREFER' and D1TLS <> '*REQUIRE'
         and D1TLS <> '*VERIFY';
      errTls = *on;
      D1MSG = 'Encryption must be *OFF, *PREFER, *REQUIRE or *VERIFY. F4 shows the list.';
    when D1CCSID = 65535;
      errCcsid = *on;
      D1MSG = 'CCSID 65535 cannot be used. 0 means the CCSID of the job.';
    when D1TXTL > 32000;
      errTxtl = *on;
      D1MSG = 'The text column length can be 32000 at most.';
    when D1NSCL > 20;
      errNscl = *on;
      D1MSG = 'The numeric scale can be 20 at most.';
    when D1TRC <> 'Y' and D1TRC <> 'N';
      errTrc = *on;
      D1MSG = 'Trace must be Y or N.';
    when not allowedIsValid();
      // message and cursor are set there
    other;
      return *on;
  endsl;
  return *off;
end-proc;

// ---------------------------------------------------------- allowed users

// Who may connect with a profile is its option allowed_users: user or group
// profiles separated by blanks. No option, or *ALL, allows everybody. The
// profile screen shows three names; a longer list is kept with option 7.
// The rules are in module XDBALW.
dcl-proc loadAllowed;
  dcl-pi *n;
    rdb char(18) const;
  end-pi;
  dcl-s value varchar(200) inz('');

  if rdb <> *blanks;
    chain(n) (rdb : ALWKEY) XDBPROR;
    if %found(XDBPRO);
      value = %trim(POVAL);
    endif;
  endif;
  alwMore = alw_toNames(value : alwLoaded);
  D1ALW1 = alwLoaded(1);
  D1ALW2 = alwLoaded(2);
  D1ALW3 = alwLoaded(3);
end-proc;

// Checks the three names of the screen; sets the message and the cursor.
dcl-proc allowedIsValid;
  dcl-pi *n ind end-pi;
  dcl-s names char(10) dim(3);
  dcl-s bad int(10);
  dcl-s msg varchar(78);

  names(1) = D1ALW1;
  names(2) = D1ALW2;
  names(3) = D1ALW3;
  msg = alw_check(names : alwLoaded : alwMore : bad);
  D1ALW1 = names(1);
  D1ALW2 = names(2);
  D1ALW3 = names(3);
  if msg = '';
    return *on;
  endif;
  if bad >= 1 and bad <= 3;
    errAlw(bad) = *on;
  endif;
  D1MSG = msg;
  return *off;
end-proc;

// Writes the three names as option allowed_users; *ALL removes the option.
dcl-proc saveAllowed;
  dcl-pi *n;
    rdb char(18) const;
  end-pi;
  dcl-s names char(10) dim(3);
  dcl-s value varchar(200);

  names(1) = D1ALW1;
  names(2) = D1ALW2;
  names(3) = D1ALW3;
  if names(1) = alwLoaded(1) and names(2) = alwLoaded(2) and names(3) = alwLoaded(3);
    // as it was read: also a list of more than three names stays whole
    return;
  endif;
  value = alw_value(names);
  chain (rdb : ALWKEY) XDBPROR;
  select;
    when value = '' and %found(XDBPRO);
      delete XDBPROR;
    when value = '';
    when %found(XDBPRO);
      POKIND = 'D';
      POVAL = value;
      POCHGU = pgmSts.user;
      POCHGT = %timestamp();
      update XDBPROR;
    other;
      PORDB = rdb;
      POKEY = ALWKEY;
      POKIND = 'D';
      POVAL = value;
      POCHGU = pgmSts.user;
      POCHGT = %timestamp();
      write(e) XDBPROR;
  endsl;
  alwLoaded(1) = names(1);
  alwLoaded(2) = names(2);
  alwLoaded(3) = names(3);
  alwMore = *off;
end-proc;

// F4 on the profile screen: the field the cursor is in decides the list.
dcl-proc promptProfileField;
  dcl-s chosen char(20);

  select;
    when D1CFLD = 'D1TYPE';
      pmtCount = 3;
      pmtVal(1) = 'PG';
      pmtTxt(1) = 'PostgreSQL';
      pmtVal(2) = 'MYSQL';
      pmtTxt(2) = 'MySQL, MariaDB';
      pmtVal(3) = 'MSSQL';
      pmtTxt(3) = 'Microsoft SQL Server 2000 and later';
      chosen = promptList('Database type');
      if chosen <> *blanks;
        D1TYPE = chosen;
      endif;
    when D1CFLD = 'D1STS';
      pmtCount = 2;
      pmtVal(1) = 'A';
      pmtTxt(1) = 'Active';
      pmtVal(2) = 'D';
      pmtTxt(2) = 'Disabled: CONNECT is refused';
      chosen = promptList('Status');
      if chosen <> *blanks;
        D1STS = chosen;
      endif;
    when D1CFLD = 'D1TLS';
      pmtCount = 4;
      pmtVal(1) = '*OFF';
      pmtTxt(1) = 'No encryption';
      pmtVal(2) = '*PREFER';
      pmtTxt(2) = 'Encrypt when the server offers it';
      pmtVal(3) = '*REQUIRE';
      pmtTxt(3) = 'Encrypt or fail';
      pmtVal(4) = '*VERIFY';
      pmtTxt(4) = 'Encrypt and check the certificate';
      chosen = promptList('Encryption (TLS)');
      if chosen <> *blanks;
        D1TLS = chosen;
      endif;
    when D1CFLD = 'D1TRC';
      pmtCount = 2;
      pmtVal(1) = 'N';
      pmtTxt(1) = 'No trace';
      pmtVal(2) = 'Y';
      pmtTxt(2) = 'Trace file in /tmp';
      chosen = promptList('Trace');
      if chosen <> *blanks;
        D1TRC = chosen;
      endif;
    when %subst(D1CFLD : 1 : 5) = 'D1ALW';
      D1MSG = 'Type up to three user or group profiles, or *ALL for everybody.';
    other;
      D1MSG = 'F4 works on Database type, Status, Encryption and Trace.';
  endsl;
end-proc;

// Shows pmtVal/pmtTxt in a window; returns the value chosen or blanks.
dcl-proc promptList;
  dcl-pi *n char(20);
    title varchar(40) const;
  end-pi;
  dcl-s i int(10);
  dcl-s chosen char(20) inz(*blanks);

  write PMTWIN;
  pmtClr = *on;
  pmtDsp = *off;
  pmtDspCtl = *off;
  write PMTCTL;
  pmtClr = *off;
  rrnP = 0;
  for i = 1 to pmtCount;
    PMOPT = ' ';
    PMVAL = pmtVal(i);
    PMTXT = pmtTxt(i);
    rrnP += 1;
    write PMTSFL;
  endfor;
  pmtDsp = (rrnP > 0);
  pmtDspCtl = *on;
  pmtEnd = *on;
  PMTITLE = title;

  exfmt PMTCTL;
  if not exitKey and not cancelKey and rrnP > 0;
    readc PMTSFL;
    dow not %eof(XDBPRFD);
      if PMOPT <> ' ' and chosen = *blanks;
        chosen = PMVAL;
      endif;
      readc PMTSFL;
    enddo;
  endif;
  exitKey = *off;
  cancelKey = *off;
  return chosen;
end-proc;

// --------------------------------------------------------------- password

dcl-proc passwordWindow;
  dcl-pi *n;
    rdb char(18) const;
  end-pi;
  dcl-s rc int(10);

  W1RDB = rdb;
  W1MSG = *blanks;
  msgIsError = *off;
  dow *on;
    W1P1A = *blanks;
    W1P1B = *blanks;
    W1P2A = *blanks;
    W1P2B = *blanks;
    exfmt PWDWIN;
    W1PWD1 = W1P1A + W1P1B;
    W1PWD2 = W1P2A + W1P2B;
    W1P1A = *blanks;
    W1P1B = *blanks;
    W1P2A = *blanks;
    W1P2B = *blanks;
    W1MSG = *blanks;
    msgIsError = *off;
    if exitKey or cancelKey;
      leave;
    endif;

    if f10Key;
      rc = removePassword(myLib : rdb);
      if rc <> 0;
        W1MSG = 'The password was not removed: error ' + %char(rc) + ' on list XDBVLDL.';
        msgIsError = *on;
        iter;
      endif;
      setPasswordFlag(rdb : 'N');
      D1PWDT = 'Not stored';
      D1MSG = 'The stored password was removed.';
      leave;
    endif;

    if W1PWD1 = *blanks;
      W1MSG = 'Type the password, twice.';
      msgIsError = *on;
      iter;
    endif;
    if W1PWD1 <> W1PWD2;
      W1MSG = 'The two entries are not the same. Type them again.';
      msgIsError = *on;
      iter;
    endif;
    rc = setPassword(myLib : rdb : %addr(W1PWD1) : %len(%trimr(W1PWD1)));
    if rc <> 0;
      W1MSG = 'The password was not stored: error ' + %char(rc) + ' on list XDBVLDL.';
      msgIsError = *on;
      iter;
    endif;
    setPasswordFlag(rdb : 'Y');
    D1PWDT = 'Stored';
    D1MSG = 'The password was stored.';
    leave;
  enddo;
  W1PWD1 = *blanks;
  W1PWD2 = *blanks;
  exitKey = *off;
  cancelKey = *off;
  f10Key = *off;
end-proc;

dcl-proc setPasswordFlag;
  dcl-pi *n;
    rdb char(18) const;
    flag char(1) const;
  end-pi;

  chain rdb XDBPRFR;
  if %found(XDBPRF);
    PRPWDS = flag;
    PRCHGU = pgmSts.user;
    PRCHGT = %timestamp();
    update XDBPRFR;
    D1CHG = %trim(PRCHGU) + '  ' + %char(PRCHGT);
  endif;
end-proc;

// ----------------------------------------------------------------- delete

dcl-proc deleteProfile;
  dcl-pi *n;
    rdb char(18) const;
  end-pi;

  if not confirmed('Delete connection profile ' + %trim(rdb)
                   : 'Options and stored password are deleted too.');
    return;
  endif;
  setll rdb XDBPROR;
  reade rdb XDBPROR;
  dow not %eof(XDBPRO);
    delete XDBPROR;
    reade rdb XDBPROR;
  enddo;
  removePassword(myLib : rdb);
  chain rdb XDBPRFR;
  if %found(XDBPRF);
    delete XDBPRFR;
  endif;
  listMsg = 'Profile ' + %trim(rdb) + ' deleted. Its RDB directory entry is not touched.';
end-proc;

dcl-proc confirmed;
  dcl-pi *n ind;
    title varchar(50) const;
    text varchar(50) const;
  end-pi;
  dcl-s yes ind;

  X1TITLE = title;
  X1TEXT = text;
  exfmt CNFWIN;
  yes = not (exitKey or cancelKey);
  exitKey = *off;
  cancelKey = *off;
  return yes;
end-proc;

// ---------------------------------------------------------------- options

dcl-proc copyOptions;
  dcl-pi *n;
    fromRdb char(18) const;
    toRdb char(18) const;
  end-pi;
  dcl-ds opt likerec(XDBPROR : *all) dim(60);
  dcl-s n int(10) inz(0);
  dcl-s i int(10);

  setll fromRdb XDBPROR;
  reade(n) fromRdb XDBPROR;
  dow not %eof(XDBPRO) and n < %elem(opt);
    n += 1;
    opt(n).POKEY = POKEY;
    opt(n).POKIND = POKIND;
    opt(n).POVAL = POVAL;
    reade(n) fromRdb XDBPROR;
  enddo;
  for i = 1 to n;
    PORDB = toRdb;
    POKEY = opt(i).POKEY;
    POKIND = opt(i).POKIND;
    POVAL = opt(i).POVAL;
    POCHGU = pgmSts.user;
    POCHGT = %timestamp();
    write(e) XDBPROR;
  endfor;
end-proc;

dcl-proc workWithOptions;
  dcl-pi *n;
    rdb char(18) const;
  end-pi;
  dcl-s selKey char(20) dim(40);
  dcl-s selOpt char(1) dim(40);
  dcl-s n int(10);
  dcl-s i int(10);
  dcl-s bad ind;
  dcl-s msg varchar(78) inz('');
  dcl-s msgErr ind inz(*off);

  chain(n) rdb XDBPRFR;
  if not %found(XDBPRF);
    listMsg = 'Profile ' + %trim(rdb) + ' is no longer there.';
    listMsgErr = *on;
    return;
  endif;
  C2RDB = PRRDB;
  C2TYPE = PRTYPE;

  dow *on;
    // load
    sfl2Clr = *on;
    sfl2Dsp = *off;
    sfl2DspCtl = *off;
    write CTL2;
    sfl2Clr = *off;
    rrn2 = 0;
    setll rdb XDBPROR;
    reade(n) rdb XDBPROR;
    dow not %eof(XDBPRO) and rrn2 < 9999;
      S2OPT = ' ';
      S2KEY = POKEY;
      S2KIND = POKIND;
      S2VAL = POVAL;
      rrn2 += 1;
      write SFL2;
      reade(n) rdb XDBPROR;
    enddo;
    sfl2Dsp = (rrn2 > 0);
    sfl2DspCtl = *on;
    sfl2End = *on;
    S2RRN = 1;
    if rrn2 = 0 and msg = '';
      msg = 'This profile has no options. Press F6 to add one.';
    endif;

    F2MSG = msg;
    msgIsError = msgErr;
    msg = '';
    msgErr = *off;
    write FOOT2;
    exfmt CTL2;
    if exitKey or cancelKey;
      cancelKey = *off;
      return;
    endif;
    if addKey;
      editOption('A' : rdb : C2TYPE : *blanks);
      iter;
    endif;

    n = 0;
    bad = *off;
    if rrn2 > 0;
      readc SFL2;
      dow not %eof(XDBPRFD);
        if S2OPT <> ' ';
          if S2OPT <> '2' and S2OPT <> '4';
            bad = *on;
          elseif n < %elem(selKey);
            n += 1;
            selKey(n) = S2KEY;
            selOpt(n) = S2OPT;
          endif;
        endif;
        readc SFL2;
      enddo;
    endif;
    if bad;
      // the list is loaded again: the options typed are dropped with it
      msg = 'Option not valid: use 2 to change or 4 to delete. Nothing was done.';
      msgErr = *on;
      iter;
    endif;
    for i = 1 to n;
      if selOpt(i) = '2';
        editOption('C' : rdb : C2TYPE : selKey(i));
      elseif confirmed('Delete option ' + %trim(selKey(i)) : 'of profile ' + %trim(rdb));
        chain (rdb : selKey(i)) XDBPROR;
        if %found(XDBPRO);
          delete XDBPROR;
          msg = 'Option ' + %trim(selKey(i)) + ' deleted.';
        endif;
      endif;
    endfor;
  enddo;
end-proc;

// The keyword as the dictionary has it for this type or for all types.
dcl-proc keywordFound;
  dcl-pi *n ind;
    type char(10) const;
    key char(20) const;
  end-pi;

  chain (type : key) XDBKWDR;
  if not %found(XDBKWD);
    chain ('*ALL' : key) XDBKWDR;
  endif;
  return %found(XDBKWD);
end-proc;

dcl-proc showKeyword;
  O1TEXT = KWTEXT;
  O1VALS = KWVALS;
  if KWVALS = *blanks;
    O1VALS = '(any value)';
  endif;
  if KWKIND = 'C';
    O1KINDT = 'C  Keyword for the database client';
  else;
    O1KINDT = 'D  Setting of the XDB driver';
  endif;
end-proc;

// True when value is one of the blank separated words of allowed.
dcl-proc isAllowed;
  dcl-pi *n ind;
    value varchar(200) const;
    allowed char(50) const;
  end-pi;
  dcl-s words varchar(50) dim(*auto : 20);
  dcl-s i int(10);

  if allowed = *blanks;
    return *on;
  endif;
  words = %split(%trim(allowed) : ' ');
  for i = 1 to %elem(words);
    if words(i) = value;
      return *on;
    endif;
  endfor;
  return *off;
end-proc;

// mode: A add, C change
dcl-proc editOption;
  dcl-pi *n;
    mode char(1) const;
    rdb char(18) const;
    type char(10) const;
    key char(20) const;
  end-pi;
  dcl-s chosen char(20);
  dcl-s words varchar(50) dim(*auto : 20);
  dcl-s i int(10);

  O1MSG = *blanks;
  O1KEY = key;
  O1VAL = *blanks;
  O1TEXT = *blanks;
  O1VALS = *blanks;
  O1KINDT = *blanks;
  errOKey = *off;
  errOVal = *off;
  msgIsError = *off;
  o1KeyProtect = (mode = 'C');
  if mode = 'C';
    O1TITLE = 'Change Option';
    chain(n) (rdb : key) XDBPROR;
    if %found(XDBPRO);
      O1VAL = POVAL;
    endif;
    if keywordFound(type : key);
      showKeyword();
    endif;
  else;
    O1TITLE = 'Add Option';
  endif;

  dow *on;
    O1VAL1 = %subst(O1VAL : 1 : 50);
    O1VAL2 = %subst(O1VAL : 51 : 50);
    O1VAL3 = %subst(O1VAL : 101 : 50);
    O1VAL4 = %subst(O1VAL : 151 : 50);
    exfmt OPTWIN;
    O1VAL = O1VAL1 + O1VAL2 + O1VAL3 + O1VAL4;
    O1MSG = *blanks;
    msgIsError = *off;
    errOKey = *off;
    errOVal = *off;
    if exitKey or cancelKey;
      leave;
    endif;

    if promptKey and O1CFLD = 'O1KEY' and mode = 'A';
      pmtCount = 0;
      setll *loval XDBKWDR;
      read XDBKWDR;
      dow not %eof(XDBKWD) and pmtCount < %elem(pmtVal);
        if KWTYPE = type or KWTYPE = '*ALL';
          pmtCount += 1;
          pmtVal(pmtCount) = KWKEY;
          pmtTxt(pmtCount) = KWTEXT;
        endif;
        read XDBKWDR;
      enddo;
      chosen = promptList('Options for ' + %trim(type));
      if chosen <> *blanks;
        O1KEY = chosen;
        if keywordFound(type : O1KEY);
          showKeyword();
        endif;
      endif;
      iter;
    endif;

    if not keywordFound(type : O1KEY);
      errOKey = *on;
      msgIsError = *on;
      O1MSG = 'Keyword not known for this database type. F4 shows the list.';
      O1TEXT = *blanks;
      O1VALS = *blanks;
      O1KINDT = *blanks;
      iter;
    endif;
    showKeyword();

    if promptKey;
      if KWVALS = *blanks;
        O1MSG = 'This keyword takes any value: there is no list.';
        iter;
      endif;
      words = %split(%trim(KWVALS) : ' ');
      pmtCount = %elem(words);
      for i = 1 to pmtCount;
        pmtVal(i) = words(i);
        pmtTxt(i) = *blanks;
      endfor;
      chosen = promptList('Values of ' + %trim(O1KEY));
      if chosen <> *blanks;
        O1VAL = chosen;
      endif;
      iter;
    endif;

    if O1VAL = *blanks;
      errOVal = *on;
      O1MSG = 'Type the value of the option.';
      iter;
    endif;
    if not isAllowed(%trim(O1VAL) : KWVALS);
      errOVal = *on;
      msgIsError = *on;
      O1MSG = 'Value not allowed. F4 on the value shows the list.';
      iter;
    endif;
    if O1KEY = ALWKEY;
      // names of user or group profiles: checked as on the profile screen
      O1VAL = %xlate(LOWER : UPPER : O1VAL);
      O1MSG = alw_listError(%trim(O1VAL));
      if O1MSG <> *blanks;
        errOVal = *on;
        msgIsError = *on;
        iter;
      endif;
    endif;

    chain (rdb : O1KEY) XDBPROR;
    if %found(XDBPRO);
      if mode = 'A';
        unlock XDBPRO;
        errOKey = *on;
        msgIsError = *on;
        O1MSG = 'The profile has this option already. Use 2=Change on the list.';
        iter;
      endif;
      POKIND = KWKIND;
      POVAL = %trim(O1VAL);
      POCHGU = pgmSts.user;
      POCHGT = %timestamp();
      update XDBPROR;
    else;
      PORDB = rdb;
      POKEY = O1KEY;
      POKIND = KWKIND;
      POVAL = %trim(O1VAL);
      POCHGU = pgmSts.user;
      POCHGT = %timestamp();
      write XDBPROR;
    endif;
    leave;
  enddo;
  exitKey = *off;
  cancelKey = *off;
  promptKey = *off;
end-proc;

// ------------------------------------------------- RDB entry and the test

// The RDB directory entry makes CONNECT TO <name> call the XDB driver.
dcl-proc addRdbEntry;
  dcl-pi *n;
    rdb char(18) const;
  end-pi;
  dcl-s cmd varchar(300);

  if not confirmed('Add RDB directory entry ' + %trim(rdb)
                   : 'It will call the XDB driver in ' + %trim(myLib) + '.');
    return;
  endif;
  cmd = 'ADDRDBDIRE RDB(' + %trim(rdb) + ') RMTLOCNAME(*ARDPGM) ARDPGM('
      + %trim(myLib) + '/XDBARD)';
  callp(e) runCommand(cmd : %len(cmd));
  if %error();
    listMsg = 'RDB entry not added (' + pgmSts.msgId + '). It may exist already: WRKRDBDIRE.';
    listMsgErr = *on;
  else;
    listMsg = 'RDB directory entry ' + %trim(rdb) + ' added.';
  endif;
end-proc;

// The test runs in program XDBPRFT (ILE C, SQL call level interface) so that
// its connection ends with it.
dcl-proc testConnection;
  dcl-pi *n;
    rdb char(18) const;
  end-pi;
  dcl-s testPgm char(21);
  dcl-pr runTest extpgm(testPgm);
    rdb char(18) const;
    ok ind;
    line1 char(62);
    line2 char(62);
    line3 char(62);
    line4 char(62);
  end-pr;
  dcl-s ok ind inz(*off);

  testPgm = %trim(myLib) + '/XDBPRFT';
  T1RDB = rdb;
  T1L1 = *blanks;
  T1L2 = *blanks;
  T1L3 = *blanks;
  T1L4 = *blanks;
  callp(e) runTest(rdb : ok : T1L1 : T1L2 : T1L3 : T1L4);
  if %error();
    ok = *off;
    T1L1 = 'The test program XDBPRFT could not be run (' + pgmSts.msgId + ').';
  endif;
  msgIsError = not ok;
  exfmt TSTWIN;
  msgIsError = *off;
  exitKey = *off;
  cancelKey = *off;
end-proc;

// ----------------------------------------------------------------- setup

// The Setup display: one line for every check of the installation in this
// library.  1 = do what the line needs, 5 = only show its commands,
// F10 = all lines that are to do, in their order.
dcl-proc setupScreen;
  dcl-pi *n;
    firstMsg varchar(78) const;
  end-pi;
  dcl-ds items likeds(xsItem_t) dim(XS_MAXITEMS);
  dcl-s count int(10);
  dcl-s selIdx int(10) dim(XS_MAXITEMS);
  dcl-s selOpt char(1) dim(XS_MAXITEMS);
  dcl-s n int(10);
  dcl-s i int(10);
  dcl-s did int(10);
  dcl-s bad ind;
  dcl-s msg varchar(78);
  dcl-s msgErr ind inz(*off);

  msg = firstMsg;
  C3USER = pgmSts.user;
  C3LIB = myLib;
  C3VER = xs_version(myLib);
  C3OWN = setupOwner;

  dow *on;
    xs_check(myLib : setupOwner : items : count);
    sfl3Clr = *on;
    sfl3Dsp = *off;
    sfl3DspCtl = *off;
    write CTL3;
    sfl3Clr = *off;
    rrn3 = 0;
    for i = 1 to count;
      S3OPT = ' ';
      S3IDX = i;
      S3TEXT = items(i).text;
      S3STAT = items(i).status;
      stInOrder = (items(i).state = 'O');
      stToDo = (items(i).state = 'T');
      stFailing = (items(i).state = 'F');
      stRemark = (items(i).state = 'W');
      rrn3 += 1;
      write SFL3;
    endfor;
    sfl3Dsp = (rrn3 > 0);
    sfl3DspCtl = *on;
    sfl3End = *on;
    S3RRN = 1;
    F3MSG = msg;
    msgIsError = msgErr;
    msg = '';
    msgErr = *off;
    write FOOT3;
    exfmt CTL3;
    if exitKey or cancelKey;
      leave;
    endif;

    // the owner profile can be another name than the one proposed
    if C3OWN = *blanks;
      C3OWN = setupOwner;
    elseif %check(NAMECHARS : %trimr(C3OWN)) > 0 or %scan(' ' : %trimr(C3OWN)) > 0
           or %scan(%subst(C3OWN : 1 : 1) : '0123456789_') > 0;
      msg = 'The owner profile must be a name: letters and digits, a letter first.';
      msgErr = *on;
      C3OWN = setupOwner;
      iter;
    elseif C3OWN <> setupOwner;
      setupOwner = C3OWN;
      msg = 'Owner profile ' + %trim(setupOwner) + ': the lines were checked again for it.';
    endif;
    if refreshKey;
      iter;
    endif;

    // a library that cannot be used is not set up
    if f10Key and items(1).state = 'F';
      msg = 'This library cannot be used: see the first line. Nothing was run.';
      msgErr = *on;
      iter;
    endif;

    if f10Key;
      did = 0;
      for i = 1 to count;
        // an earlier line may have changed what this one needs
        xs_check(myLib : setupOwner : items : count);
        if items(i).state = 'T' and not items(i).manual;
          did += 1;
          setupLine(items(i) : *on : msg : msgErr);
          if exitKey or msgErr;
            leave;
          endif;
        endif;
      endfor;
      exitKey = *off;
      cancelKey = *off;
      if did = 0;
        msg = 'There is nothing to do.';
      endif;
      iter;
    endif;

    n = 0;
    bad = *off;
    if rrn3 > 0;
      readc SFL3;
      dow not %eof(XDBPRFD);
        if S3OPT <> ' ';
          if S3OPT <> '1' and S3OPT <> '5';
            bad = *on;
          elseif n < %elem(selIdx);
            n += 1;
            selIdx(n) = S3IDX;
            selOpt(n) = S3OPT;
          endif;
        endif;
        readc SFL3;
      enddo;
    endif;
    if bad;
      msg = 'Option not valid: use 1 to do it or 5 to show the commands. Nothing was done.';
      msgErr = *on;
      iter;
    endif;
    for i = 1 to n;
      xs_check(myLib : setupOwner : items : count);
      if selIdx(i) >= 1 and selIdx(i) <= count;
        if selOpt(i) = '1' and items(1).state = 'F' and selIdx(i) > 1;
          // shown, not run
          setupLine(items(selIdx(i)) : *off : msg : msgErr);
          msg = 'This library cannot be used: see the first line. Nothing was run.';
          msgErr = *on;
        else;
          setupLine(items(selIdx(i)) : (selOpt(i) = '1') : msg : msgErr);
        endif;
      endif;
      if exitKey;
        leave;
      endif;
    endfor;
    exitKey = *off;
    cancelKey = *off;
  enddo;
  exitKey = *off;
  cancelKey = *off;
end-proc;

// One line of the setup: a window with its commands, and when the user
// confirms, the commands are run and a second window tells how it went.
dcl-proc setupLine;
  dcl-pi *n;
    item likeds(xsItem_t) const;
    wantRun ind const;
    msg varchar(78);
    msgErr ind;
  end-pi;
  dcl-s lines char(WINWIDTH) dim(WINLINES);
  dcl-s notes char(WINWIDTH) dim(WINLINES);
  dcl-s n int(10) inz(0);
  dcl-s nn int(10) inz(0);
  dcl-s j int(10);
  dcl-s run ind;
  dcl-s ok ind inz(*on);
  dcl-s msgId char(7);
  dcl-s msgText varchar(132);

  run = wantRun;
  M1TITLE = item.text;
  wrapText(item.note : notes : nn : 0);
  M1N1 = notes(1);
  M1N2 = notes(2);
  if item.ncmd = 0;
    run = *off;
    if item.state = 'O';
      wrapText('This line is in order. There is nothing to do.' : lines : n : 0);
    else;
      wrapText(%trim(item.status) + '. Setup has no command for this.' : lines : n : 0);
    endif;
  else;
    for j = 1 to item.ncmd;
      wrapText(item.cmd(j) : lines : n : 2);
    endfor;
  endif;
  if run and item.manual;
    run = *off;
  endif;
  select;
    when run;
      M1KEYS = 'Enter=Run these commands   F12=Do not run';
    when item.manual;
      M1KEYS = 'Setup does not run this command.   Enter=Continue';
    other;
      M1KEYS = 'Enter=Continue';
  endsl;
  showWindow(lines);
  if exitKey or cancelKey or not run;
    cancelKey = *off;
    return;
  endif;

  clear lines;
  n = 0;
  for j = 1 to item.ncmd;
    ok = xs_run(item.cmd(j) : msgId : msgText);
    if ok;
      wrapText('Done:   ' + item.cmd(j) : lines : n : 8);
    else;
      wrapText('Failed: ' + item.cmd(j) : lines : n : 8);
      wrapText(msgId + ' ' + msgText : lines : n : 8);
      leave;
    endif;
  endfor;
  M1TITLE = 'Result: ' + item.text;
  M1N1 = *blanks;
  M1N2 = *blanks;
  if ok;
    M1N1 = 'All commands of this line ran.';
    msg = 'Done: ' + %trim(item.text) + '.';
  else;
    M1N1 = 'A command failed. The commands after it were not run.';
    M1N2 = 'The job log has the details.';
    msg = 'Not done: ' + %trim(item.text) + '. See the job log.';
    msgErr = *on;
  endif;
  M1KEYS = 'Enter=Continue';
  showWindow(lines);
  cancelKey = *off;
end-proc;

dcl-proc showWindow;
  dcl-pi *n;
    lines char(WINWIDTH) dim(WINLINES) const;
  end-pi;
  M1L01 = lines(1);
  M1L02 = lines(2);
  M1L03 = lines(3);
  M1L04 = lines(4);
  M1L05 = lines(5);
  M1L06 = lines(6);
  M1L07 = lines(7);
  M1L08 = lines(8);
  M1L09 = lines(9);
  M1L10 = lines(10);
  M1L11 = lines(11);
  M1L12 = lines(12);
  exfmt CMDWIN;
end-proc;

// Adds text to the window lines, broken at blanks; the lines after the
// first of a text are moved in by indent positions.
dcl-proc wrapText;
  dcl-pi *n;
    text varchar(300) const;
    lines char(WINWIDTH) dim(WINLINES);
    n int(10);
    indent int(10) const;
  end-pi;
  dcl-s rest varchar(300);
  dcl-s w int(10);
  dcl-s cut int(10);
  dcl-s k int(10);
  dcl-s first ind inz(*on);

  rest = %trim(text);
  dow %len(rest) > 0 and n < WINLINES;
    w = WINWIDTH;
    if not first;
      w = WINWIDTH - indent;
    endif;
    if %len(rest) <= w;
      cut = %len(rest);
    else;
      cut = w;
      for k = w + 1 downto 2;
        if %subst(rest : k : 1) = ' ';
          cut = k - 1;
          leave;
        endif;
      endfor;
    endif;
    n += 1;
    lines(n) = *blanks;
    if first;
      lines(n) = %subst(rest : 1 : cut);
    else;
      %subst(lines(n) : indent + 1) = %subst(rest : 1 : cut);
    endif;
    if cut >= %len(rest);
      rest = '';
    else;
      rest = %trim(%subst(rest : cut + 1));
    endif;
    first = *off;
  enddo;
end-proc;
