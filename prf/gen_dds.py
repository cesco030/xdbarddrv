#!/usr/bin/env python3
"""Writes the DDS sources of the XDB profile files and of the display file.
DDS is column sensitive; A() puts every piece in its columns and continues
long keyword text on the next line."""
import sys, os

out = []

def A(cond='', typ=' ', name='', ref=' ', ln='', dt=' ', dec='', use=' ', row='', col='', kw=''):
    conds = cond.split() if cond else []
    c = ''.join(('N' + x[1:] if x.startswith('N') else ' ' + x) for x in conds).ljust(9)
    head = ('     A ' + c + typ + ' ' + name.ljust(10) + ref + str(ln).rjust(5) + dt +
            str(dec).rjust(2) + use + str(row).rjust(3) + str(col).rjust(3))
    assert len(head) == 44, (len(head), head)
    first = True
    while True:
        if len(kw) <= 36:
            out.append((head if first else ' ' * 5 + 'A' + ' ' * 38) + kw)
            break
        out.append((head if first else ' ' * 5 + 'A' + ' ' * 38) + kw[:35] + '-')
        kw = kw[35:]
        first = False

def C(text=''):
    out.append('     A*' + (' ' + text if text else ''))

def K(*kws, cond=''):
    for k in kws:
        A(cond=cond, kw=k)

def T(row, col, text, *kws):
    """constant"""
    A(row=row, col=col, kw="'" + text + "'")
    for k in kws:
        A(kw=k)

def F(name, ln, dt, use, row, col, *kws, dec='', conds=()):
    A(name=name, ln=ln, dt=dt, dec=dec, use=use, row=row, col=col, kw=kws[0] if kws else '')
    for k in kws[1:]:
        A(kw=k)
    for c, k in conds:
        A(cond=c, kw=k)

def H(module, area):
    """help specification: UIM help module of panel group WRKXDBPRF for an area of the display.
    area: 'r1 c1 r2 c2', '*RCD' (the whole record), '*FLD name', or '*NONE' (the module is
    part of the help of the whole display only).  Help specifications come after the
    record-level keywords and before the first field of a record."""
    assert len(module) <= 32, module
    A(typ='H', kw="HLPPNLGRP('" + module + "' WRKXDBPRF)")
    A(kw='HLPARA(' + area + ')')

def ROWS(r1, r2=None):
    return '%d 1 %d 80' % (r1, r2 or r1)

def save(path):
    global out
    for l in out:
        assert len(l) <= 80, l
    open(path, 'w').write('\n'.join(out) + '\n')
    out = []

d = sys.argv[1]

# ------------------------------------------------------------ XDBPRF
C('XDBPRF - XDB connection profiles, one record per RDB name.')
C('Kept with WRKXDBPRF. The password is not here: it is an entry of')
C('validation list XDBVLDL. Character and zoned fields only, the')
C('driver reads the record as it is (xdbprf.h).')
A(kw='UNIQUE')
A(typ='R', name='XDBPRFR', kw="TEXT('XDB connection profile')")
def PF(name, ln, dt, text, hdg, dec=''):
    A(name=name, ln=ln, dt=dt, dec=dec, kw="TEXT('" + text + "')")
    A(kw='COLHDG(' + ' '.join("'" + h + "'" for h in hdg) + ')')
PF('PRRDB', 18, 'A', 'RDB directory entry name', ['RDB name'])
PF('PRTYPE', 10, 'A', 'Database type: PG MYSQL MSSQL', ['Database', 'type'])
PF('PRSTS', 1, 'A', 'Status: A=Active D=Disabled', ['Sts'])
PF('PRTEXT', 50, 'A', 'Description', ['Description'])
PF('PRHOST', 120, 'A', 'Host name or IP address', ['Host'])
PF('PRPORT', 5, 'S', 'TCP port, 0 = default of the type', ['Port'], dec=0)
PF('PRDBNM', 60, 'A', 'Database name on the server', ['Database'])
PF('PRUSER', 60, 'A', 'User on the server', ['User'])
PF('PRPWDS', 1, 'A', 'Password stored in XDBVLDL: Y N', ['Pwd', 'stored'])
PF('PRTLS', 10, 'A', 'Encryption: *OFF *PREFER *REQUIRE *VERIFY', ['Encryption'])
PF('PRTMO', 3, 'S', 'Connect timeout in seconds', ['Connect', 'timeout'], dec=0)
PF('PRCCSID', 5, 'S', 'CCSID of character columns, 0 = job', ['Column', 'CCSID'], dec=0)
PF('PRTXTL', 5, 'S', 'Length of text columns without length', ['Text', 'length'], dec=0)
PF('PRNSCL', 2, 'S', 'Scale of numeric columns without scale', ['Numeric', 'scale'], dec=0)
PF('PRTRC', 1, 'A', 'Trace to /tmp/xdb_<rdb>.log: Y N', ['Trace'])
PF('PRCRTU', 10, 'A', 'Created by', ['Created', 'by'])
PF('PRCRTT', '', 'Z', 'Created at', ['Created', 'at'])
PF('PRCHGU', 10, 'A', 'Changed by', ['Changed', 'by'])
PF('PRCHGT', '', 'Z', 'Changed at', ['Changed', 'at'])
A(typ='K', name='PRRDB')
save(os.path.join(d, 'xdbprf.pf'))

# ------------------------------------------------------------ XDBPRO
C('XDBPRO - options of an XDB connection profile.')
C('POKIND C: keyword added to the connection string of the database')
C('client. POKIND D: setting of the XDB driver itself.')
A(kw='UNIQUE')
A(typ='R', name='XDBPROR', kw="TEXT('XDB profile option')")
PF('PORDB', 18, 'A', 'RDB directory entry name', ['RDB name'])
PF('POKEY', 20, 'A', 'Keyword', ['Keyword'])
PF('POKIND', 1, 'A', 'Kind: C=Connection D=Driver', ['Kind'])
PF('POVAL', 200, 'A', 'Value', ['Value'])
PF('POCHGU', 10, 'A', 'Changed by', ['Changed', 'by'])
PF('POCHGT', '', 'Z', 'Changed at', ['Changed', 'at'])
A(typ='K', name='PORDB')
A(typ='K', name='POKEY')
save(os.path.join(d, 'xdbpro.pf'))

# ------------------------------------------------------------ XDBKWD
C('XDBKWD - keywords that can be used as profile options, per database')
C('type (*ALL = every type). Loaded from xdbkwd.sql; used by WRKXDBPRF')
C('for F4 and for checking. KWVALS: allowed values separated by a')
C('blank, empty = any value.')
A(kw='UNIQUE')
A(typ='R', name='XDBKWDR', kw="TEXT('XDB option keyword')")
PF('KWTYPE', 10, 'A', 'Database type or *ALL', ['Database', 'type'])
PF('KWKEY', 20, 'A', 'Keyword', ['Keyword'])
PF('KWKIND', 1, 'A', 'Kind: C=Connection D=Driver', ['Kind'])
PF('KWTEXT', 50, 'A', 'Description', ['Description'])
PF('KWVALS', 50, 'A', 'Allowed values', ['Allowed', 'values'])
A(typ='K', name='KWTYPE')
A(typ='K', name='KWKEY')
save(os.path.join(d, 'xdbkwd.pf'))

# ------------------------------------------------------------ XDBPRFD
BLU = 'COLOR(BLU)'
HI = 'DSPATR(HI)'
def MSG(name, ln, row, col):
    F(name, ln, 'A', 'O', row, col, conds=[('90', 'COLOR(RED)'), ('N90', 'COLOR(WHT)')])

C('XDBPRFD - display file of WRKXDBPRF (program XDBPRFM):')
C('work with XDB connection profiles.')
C()
C('Indicators  03 05 06 04 10 12  function keys')
C('            31-34  profile list     35-38  prompt list')
C('            71-74  option list      44 option kept for the next read')
C('            75-78  setup list       09 function key Setup')
C('            45 46 48  option in error')
C('            81-84  status of a setup line: in order, to do, failing,')
C('                   remark')
C('            40 key protected  41 all protected  47 keyword protected')
C('            51-65  field in error   90  message is an error')
C()
C('Help: F1 is the Help key.  The help specifications (H) name the help')
C('modules of panel group WRKXDBPRF, which the system looks for through')
C('the library list: XDBPRFM puts its library there while it runs.  The')
C('module names are checked against the panel group by build/chk_help.py.')
K('DSPSIZ(24 80 *DS3)', 'INDARA', 'PRINT', "CA03(03 'Exit')", "CA12(12 'Cancel')")
K('HELP', 'ALTHELP(CA01)', "HLPTITLE('XDBARDDRV profiles')")
K("HLPPNLGRP('XDB/GENERAL' WRKXDBPRF)")

C(); C('List of profiles')
A(typ='R', name='SFL1', kw='SFL')
K('SFLNXTCHG', cond='44')
F('S1OPT', 1, 'A', 'B', 9, 3, conds=[('45', 'DSPATR(RI PC)')])
F('S1RDB', 18, 'A', 'O', 9, 7)
F('S1TYPE', 6, 'A', 'O', 9, 27)
F('S1HOST', 26, 'A', 'O', 9, 35)
F('S1DBNM', 14, 'A', 'O', 9, 63)
F('S1STS', 1, 'A', 'O', 9, 79)
A(typ='R', name='CTL1', kw='SFLCTL(SFL1)')
K('SFLSIZ(0013)', 'SFLPAG(0012)', 'OVERLAY', "CF05(05 'Refresh')", "CF06(06 'Add')", "CF09(09 'Setup')")
K('SFLDSP', cond='31'); K('SFLDSPCTL', cond='32'); K('SFLCLR', cond='33'); K('SFLEND(*MORE)', cond='34')
H('XDB/LIST', ROWS(1, 3))
H('XDB/LIST/POS', ROWS(4, 5))
H('XDB/LIST/OPT', ROWS(6, 21))
F('S1RRN', 4, 'S', 'H', '', '', 'SFLRCDNBR(CURSOR)', dec=0)
T(1, 2, 'XDBPRFM', BLU)
T(1, 24, 'Work with XDB Connection Profiles', HI)
A(row=1, col=72, kw='DATE'); K('EDTCDE(Y)', BLU)
F('C1USER', 10, 'A', 'O', 2, 2, BLU)
T(2, 24, 'Library . . :', BLU)
F('C1LIB', 10, 'A', 'O', 2, 38, BLU)
A(row=2, col=72, kw='TIME'); K(BLU)
T(4, 2, 'Position to  . . . . .')
F('C1POS', 18, 'A', 'B', 4, 26)
T(4, 46, 'RDB name')
T(6, 2, 'Type options, press Enter.', BLU)
T(7, 4, '2=Change  3=Copy  4=Delete  5=Display  7=Options  8=RDB entry  9=Test', BLU)
T(8, 2, 'Opt', HI); T(8, 7, 'RDB name', HI); T(8, 27, 'Type', HI); T(8, 35, 'Host', HI)
T(8, 63, 'Database', HI); T(8, 78, 'Sts', HI)
A(typ='R', name='FOOT1')
H('XDB/LIST/KEYS', '*RCD')
# topics that belong to no place of the display: part of the help of the whole display (F2)
for m in ('XDB/RDBENTRY', 'XDB/TEST', 'XDB/SQL', 'XDB/PROBLEMS', 'XDB/LICENCE'):
    H(m, '*NONE')
T(23, 2, 'F1=Help   F3=Exit   F5=Refresh   F6=Add   F9=Setup   F12=Cancel', BLU)
MSG('F1MSG', 78, 24, 2)

C(); C('One profile: add, change, copy, display')
A(typ='R', name='PRFDTL')
K("CF04(04 'Prompt')", "CF10(10 'Password')", 'CHGINPDFT', 'RTNCSRLOC(&D1CREC &D1CFLD)')
H('XDB/PROFILE', ROWS(1, 2))
for m, r1, r2 in (('RDB', 3, 3), ('TEXT', 4, 4), ('TYPE', 5, 5), ('STS', 6, 6), ('HOST', 7, 9),
                  ('PORT', 10, 10), ('DBNM', 11, 11), ('USER', 12, 12), ('PWD', 13, 13),
                  ('TLS', 14, 14), ('TMO', 15, 15), ('ALW', 16, 16), ('CCSID', 17, 17),
                  ('TXTL', 18, 18), ('NSCL', 19, 19), ('TRC', 20, 20), ('CHG', 21, 21),
                  ('KEYS', 22, 23)):
    H('XDB/PRF/' + m, ROWS(r1, r2))
F('D1CREC', 10, 'A', 'H', '', '')
F('D1CFLD', 10, 'A', 'H', '', '')
T(1, 2, 'XDBPRFM', BLU)
F('D1TITLE', 40, 'A', 'O', 1, 21, HI)
def IN(label, name, ln, dt, row, err, *kws, dec='', hint='', key=False):
    T(row, 2, label)
    prot = '40' if key else '41'
    F(name, ln, dt, 'B', row, 19, *kws, dec=dec,
      conds=[(prot, 'DSPATR(PR)'), ('N' + prot, 'DSPATR(UL)'), (str(err), 'DSPATR(RI PC)')])
    if hint:
        T(row, 19 + int(ln) + 3, hint, BLU)
IN('RDB name . . . .', 'D1RDB', 18, 'A', 3, 51, key=True, hint='Name of the RDB directory entry')
IN('Description  . .', 'D1TEXT', 50, 'A', 4, 50, 'CHECK(LC)')
IN('Database type  .', 'D1TYPE', 10, 'A', 5, 52, hint='PG, MYSQL, MSSQL  (F4)')
IN('Status . . . . .', 'D1STS', 1, 'A', 6, 53, hint='A=Active, D=Disabled')
IN('Host . . . . . .', 'D1HOST1', 60, 'A', 8, 54, 'CHECK(LC)')
F('D1HOST2', 60, 'A', 'B', 9, 19, 'CHECK(LC)',
  conds=[('41', 'DSPATR(PR)'), ('N41', 'DSPATR(UL)'), ('54', 'DSPATR(RI)')])
IN('Port . . . . . .', 'D1PORT', 5, 'Y', 10, 55, 'EDTCDE(Z)', dec=0, hint='0 = default port of the database type')
IN('Database . . . .', 'D1DBNM', 60, 'A', 11, 56, 'CHECK(LC)')
IN('User . . . . . .', 'D1USER', 60, 'A', 12, 57, 'CHECK(LC)')
T(13, 2, 'Password . . . :')
F('D1PWDT', 12, 'A', 'O', 13, 19, HI)
T(13, 34, 'F10=Set or remove the password', BLU)
IN('Encryption (TLS)', 'D1TLS', 10, 'A', 14, 58, hint='*OFF *PREFER *REQUIRE *VERIFY  (F4)')
IN('Connect timeout', 'D1TMO', 3, 'Y', 15, 59, 'EDTCDE(Z)', dec=0, hint='Seconds, 0 = 10')
T(16, 2, 'Allowed users  .')
for i, col, err in ((1, 19, '66'), (2, 31, '67'), (3, 43, '68')):
    F('D1ALW%d' % i, 10, 'A', 'B', 16, col,
      conds=[('41', 'DSPATR(PR)'), ('N41', 'DSPATR(UL)'), (err, 'DSPATR(RI PC)')])
T(16, 55, 'Users or groups, or *ALL', BLU)
IN('Column CCSID . .', 'D1CCSID', 5, 'Y', 17, 60, 'EDTCDE(Z)', dec=0, hint='0 = job CCSID, 1208 = UTF-8')
IN('Text col. length', 'D1TXTL', 5, 'Y', 18, 61, 'EDTCDE(Z)', dec=0, hint='For text columns without a length')
IN('Numeric scale  .', 'D1NSCL', 2, 'Y', 19, 62, 'EDTCDE(3)', dec=0, hint='Decimals of numerics without a scale')
IN('Trace  . . . . .', 'D1TRC', 1, 'A', 20, 63, hint='Y=Yes, N=No   File /tmp/xdb_<RDB>.log')
T(21, 2, 'Last change  . :')
F('D1CHG', 40, 'A', 'O', 21, 19, BLU)
F('D1KEYS', 78, 'A', 'O', 23, 2, BLU)
MSG('D1MSG', 78, 24, 2)

C(); C('Password window')
A(typ='R', name='PWDWIN')
K('WINDOW(8 6 11 66)', 'WDWBORDER((*COLOR BLU))', "CF10(10 'Remove')")
H('XDB/PASSWORD', '*RCD')
T(1, 2, 'Password of profile')
F('W1RDB', 18, 'A', 'O', 1, 22, HI)
T(3, 2, 'New password (it is not shown; second line = characters 61-120)')
F('W1P1A', 60, 'A', 'B', 4, 3, 'CHECK(LC)', 'DSPATR(ND)', 'DSPATR(PC)')
F('W1P1B', 60, 'A', 'B', 5, 3, 'CHECK(LC)', 'DSPATR(ND)')
T(6, 2, 'Repeat the new password')
F('W1P2A', 60, 'A', 'B', 7, 3, 'CHECK(LC)', 'DSPATR(ND)')
F('W1P2B', 60, 'A', 'B', 8, 3, 'CHECK(LC)', 'DSPATR(ND)')
T(9, 2, 'F1=Help   Enter=Store   F10=Remove the password   F12=Cancel', BLU)
MSG('W1MSG', 62, 10, 2)

C(); C('Prompt window: a list to choose one value from')
A(typ='R', name='PMTWIN')
K('WINDOW(5 8 14 62)', 'WDWBORDER((*COLOR BLU))')
T(13, 2, '1=Select   F1=Help   F12=Cancel', BLU)
A(typ='R', name='PMTSFL', kw='SFL')
F('PMOPT', 1, 'A', 'B', 4, 2)
F('PMVAL', 20, 'A', 'O', 4, 6)
F('PMTXT', 34, 'A', 'O', 4, 28)
A(typ='R', name='PMTCTL', kw='SFLCTL(PMTSFL)')
K('WINDOW(PMTWIN)', 'SFLSIZ(0009)', 'SFLPAG(0008)', 'OVERLAY')
K('SFLDSP', cond='35'); K('SFLDSPCTL', cond='36'); K('SFLCLR', cond='37'); K('SFLEND(*MORE)', cond='38')
# a subfile control record cannot have HLPARA(*RCD): the lines of the window above its key line
H('XDB/PROMPT', '1 1 12 60')
F('PMTITLE', 40, 'A', 'O', 1, 2, HI)
T(3, 2, 'Opt', HI); T(3, 6, 'Value', HI); T(3, 28, 'Description', HI)

C(); C('Options of a profile')
A(typ='R', name='SFL2', kw='SFL')
K('SFLNXTCHG', cond='44')
F('S2OPT', 1, 'A', 'B', 8, 3, conds=[('46', 'DSPATR(RI PC)')])
F('S2KEY', 20, 'A', 'O', 8, 7)
F('S2KIND', 1, 'A', 'O', 8, 29)
F('S2VAL', 48, 'A', 'O', 8, 32)
A(typ='R', name='CTL2', kw='SFLCTL(SFL2)')
K('SFLSIZ(0014)', 'SFLPAG(0013)', 'OVERLAY', "CF06(06 'Add')")
K('SFLDSP', cond='71'); K('SFLDSPCTL', cond='72'); K('SFLCLR', cond='73'); K('SFLEND(*MORE)', cond='74')
H('XDB/OPTIONS', ROWS(1, 4))
H('XDB/OPT/LIST', ROWS(5, 21))
F('S2RRN', 4, 'S', 'H', '', '', 'SFLRCDNBR(CURSOR)', dec=0)
T(1, 2, 'XDBPRFM', BLU)
T(1, 25, 'Options of a Connection Profile', HI)
T(3, 2, 'RDB name . . . . :')
F('C2RDB', 18, 'A', 'O', 3, 21, HI)
T(3, 42, 'Type . . :')
F('C2TYPE', 10, 'A', 'O', 3, 53, HI)
T(5, 2, 'Type options, press Enter.', BLU)
T(6, 4, '2=Change   4=Delete', BLU)
T(7, 2, 'Opt', HI); T(7, 7, 'Keyword', HI); T(7, 29, 'K', HI); T(7, 32, 'Value', HI)
A(typ='R', name='FOOT2')
H('XDB/OPT/KEYS', '*RCD')
# what the keywords do: part of the help of the whole display (F2)
CATALOGS = ('XDB/OPT/ALL', 'XDB/OPT/PG', 'XDB/OPT/MYSQL', 'XDB/OPT/MSSQL')
for m in CATALOGS:
    H(m, '*NONE')
T(22, 2, 'K: C=keyword for the database client, D=setting of the XDB driver', BLU)
T(23, 2, 'F1=Help   F3=Exit   F6=Add   F12=Cancel', BLU)
MSG('F2MSG', 78, 24, 2)

C(); C('One option')
A(typ='R', name='OPTWIN')
K('WINDOW(6 5 14 70)', 'WDWBORDER((*COLOR BLU))', "CF04(04 'Prompt')", 'RTNCSRLOC(&O1CREC &O1CFLD)')
H('XDB/OPT/KEY', '*FLD O1KEY')
H('XDB/OPT/VAL', '*FLD O1VAL1')
H('XDB/OPT/WIN', '*RCD')
for m in CATALOGS:
    H(m, '*NONE')
F('O1CREC', 10, 'A', 'H', '', '')
F('O1CFLD', 10, 'A', 'H', '', '')
F('O1TITLE', 40, 'A', 'O', 1, 2, HI)
T(3, 2, 'Keyword . . .')
F('O1KEY', 20, 'A', 'B', 3, 16, 'CHECK(LC)', conds=[('47', 'DSPATR(PR)'), ('64', 'DSPATR(RI PC)')])
T(4, 2, 'Description :')
F('O1TEXT', 50, 'A', 'O', 4, 16)
T(5, 2, 'Values  . . :')
F('O1VALS', 50, 'A', 'O', 5, 16)
T(6, 2, 'Kind  . . . :')
F('O1KINDT', 40, 'A', 'O', 6, 16)
T(8, 2, 'Value . . . .')
F('O1VAL1', 50, 'A', 'B', 8, 16, 'CHECK(LC)', conds=[('65', 'DSPATR(RI PC)')])
F('O1VAL2', 50, 'A', 'B', 9, 16, 'CHECK(LC)', conds=[('65', 'DSPATR(RI)')])
F('O1VAL3', 50, 'A', 'B', 10, 16, 'CHECK(LC)', conds=[('65', 'DSPATR(RI)')])
F('O1VAL4', 50, 'A', 'B', 11, 16, 'CHECK(LC)', conds=[('65', 'DSPATR(RI)')])
T(12, 2, 'F1=Help   F4=Prompt   F12=Cancel', BLU)
MSG('O1MSG', 66, 13, 2)

C(); C('Confirmation window')
A(typ='R', name='CNFWIN')
K('WINDOW(9 12 6 54)', 'WDWBORDER((*COLOR RED))')
H('XDB/CONFIRM', '*RCD')
F('X1TITLE', 50, 'A', 'O', 1, 2, HI)
F('X1TEXT', 50, 'A', 'O', 3, 2)
T(5, 2, 'Enter=Confirm   F1=Help   F12=Cancel', BLU)

C(); C('Result of a connection test')
A(typ='R', name='TSTWIN')
K('WINDOW(7 6 9 66)', 'WDWBORDER((*COLOR BLU))')
H('XDB/TEST', '*RCD')
T(1, 2, 'Connection test of', HI)
F('T1RDB', 18, 'A', 'O', 1, 21, HI)
F('T1L1', 62, 'A', 'O', 3, 2, conds=[('90', 'COLOR(RED)'), ('N90', 'COLOR(GRN)')])
F('T1L2', 62, 'A', 'O', 4, 2)
F('T1L3', 62, 'A', 'O', 5, 2)
F('T1L4', 62, 'A', 'O', 6, 2)
T(8, 2, 'Press Enter to continue.', BLU)

C(); C('Setup: the checks of the installation in this library')
A(typ='R', name='SFL3', kw='SFL')
F('S3OPT', 1, 'A', 'B', 9, 3, conds=[('48', 'DSPATR(RI PC)')])
F('S3TEXT', 40, 'A', 'O', 9, 7)
F('S3STAT', 30, 'A', 'O', 9, 49, conds=[('81', 'COLOR(GRN)'), ('82', 'COLOR(YLW)'),
                                         ('83', 'COLOR(RED)'), ('84', 'COLOR(TRQ)')])
F('S3IDX', 2, 'S', 'H', '', '', dec=0)
A(typ='R', name='CTL3', kw='SFLCTL(SFL3)')
K('SFLSIZ(0013)', 'SFLPAG(0012)', 'OVERLAY', "CF05(05 'Check again')", "CF10(10 'Do all')")
K('SFLDSP', cond='75'); K('SFLDSPCTL', cond='76'); K('SFLCLR', cond='77'); K('SFLEND(*MORE)', cond='78')
H('XDB/SETUP', ROWS(1, 4))
H('XDB/SETUP/OWN', ROWS(5))
H('XDB/SETUP/OPT', ROWS(6, 7))
H('XDB/SETUP/LINES', '8 1 21 47')
H('XDB/SETUP/STS', '8 48 21 80')
F('S3RRN', 4, 'S', 'H', '', '', 'SFLRCDNBR(CURSOR)', dec=0)
T(1, 2, 'XDBPRFM', BLU)
T(1, 35, 'XDB Setup', HI)
A(row=1, col=72, kw='DATE'); K('EDTCDE(Y)', BLU)
F('C3USER', 10, 'A', 'O', 2, 2, BLU)
T(2, 24, 'Library . . :', BLU)
F('C3LIB', 10, 'A', 'O', 2, 38, BLU)
A(row=2, col=72, kw='TIME'); K(BLU)
T(4, 2, 'Version  . . . . . . .')
F('C3VER', 50, 'A', 'O', 4, 26)
T(5, 2, 'Owner profile  . . . .')
F('C3OWN', 10, 'A', 'B', 5, 26)
T(5, 39, 'Owns the driver and the password list', BLU)
T(6, 2, 'Type options, press Enter.', BLU)
T(7, 4, '1=Do it   5=Show the commands', BLU)
T(8, 2, 'Opt', HI); T(8, 7, 'Check', HI); T(8, 49, 'Status', HI)
A(typ='R', name='FOOT3')
H('XDB/SETUP/KEYS', '*RCD')
for m in ('XDB/SETUP/HOW', 'XDB/PROBLEMS', 'XDB/LICENCE'):
    H(m, '*NONE')
T(22, 2, 'Every change is shown as a CL command first and runs only when you confirm.', BLU)
T(23, 2, 'F1=Help   F3=Exit   F5=Check again   F10=Do all that is to do   F12=Cancel', BLU)
MSG('F3MSG', 78, 24, 2)

C(); C('Setup: the commands of one line, and their result')
A(typ='R', name='CMDWIN')
K('WINDOW(3 3 19 72)', 'WDWBORDER((*COLOR BLU))')
H('XDB/SETUP/CMD', '*RCD')
F('M1TITLE', 68, 'A', 'O', 1, 2, HI)
F('M1N1', 68, 'A', 'O', 2, 2, BLU)
F('M1N2', 68, 'A', 'O', 3, 2, BLU)
for i in range(1, 13):
    F('M1L%02d' % i, 68, 'A', 'O', 4 + i, 2)
F('M1KEYS', 68, 'A', 'O', 18, 2, BLU)
save(os.path.join(d, 'xdbprfd.dspf'))
