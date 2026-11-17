#!/usr/bin/env python3
"""Compares the help modules that display file XDBPRFD names (prf/xdbprfd.dspf, written by
prf/gen_dds.py) with the modules of panel group WRKXDBPRF (prf/wrkxdbprf.pnlgrp).
usage: python3 chk_help.py <kitdir>      exit code 1 when a name does not match"""
import re, sys, os

kit = sys.argv[1] if len(sys.argv) > 1 else '.'
dds = open(os.path.join(kit, 'prf', 'xdbprfd.dspf')).read().split('\n')
pnl = open(os.path.join(kit, 'prf', 'wrkxdbprf.pnlgrp')).read()

# keyword text of the DDS, continuation lines (ending with -) joined
text = ''
for l in dds:
    if l[5:7] == 'A*' or len(l) < 45:
        continue
    k = l[44:]
    text += k[:-1] if k.endswith('-') else k + '\n'
used = re.findall(r"HLPPNLGRP\('([^']+)' WRKXDBPRF\)", text)
mods = re.findall(r"^:HELP NAME='([^']+)'\.", pnl, re.M)
imported = re.findall(r"^:IMHELP NAME='([^']+)'\.", pnl, re.M)

bad = 0
for m in sorted(set(used)):
    if m not in mods:
        print('display file names a module the panel group does not have:', m); bad = 1
for m in sorted(set(imported)):
    if m not in mods:
        print('the command help imports a module that does not exist:', m); bad = 1
for m in mods:
    if mods.count(m) > 1:
        print('module defined twice:', m); bad = 1
    if len(m) > 32:
        print('module name longer than 32 characters:', m); bad = 1
loose = [m for m in mods if m not in used and m not in imported
         and not m.startswith('WRKXDBPRF')]
for m in loose:
    print('module used by nothing:', m); bad = 1
only_cmd = [m for m in mods if m in imported and m not in used]
print('%d help specifications, %d modules named by the display file, %d modules in the panel group, '
      '%d of them imported by the command help' % (len(used), len(set(used)), len(mods), len(set(imported))))
if only_cmd:
    print('in the command help only (no display shows them):', ' '.join(only_cmd))
sys.exit(bad)
