#!/usr/bin/env python3
"""
gen_notices.py - writes doc/THIRD-PARTY-NOTICES.txt from the sources in pg/.

usage: python3 build/gen_notices.py <kitdir> [<unchanged PostgreSQL source tree>]

The notices are taken from the files themselves, word for word, so that the
text that travels with a package is the text the authors wrote.  Run it again
whenever the content of pg/ changes (a newer libpq); it runs on any system
with Python 3 and is not part of the IBM i build.  With the source tree of
the PostgreSQL release pg/ was taken from as second argument, the list of
changed files below is checked against it.
"""
import hashlib
import os
import re
import sys

kit = sys.argv[1] if len(sys.argv) > 1 else "."
pg = os.path.join(kit, "pg")

# a comment block is a licence notice when it grants rights in these words
GRANT = re.compile(r"Redistribution and use in source and binary forms|"
                   r"Permission to use, copy, modify,? and(/or)? distribute|"
                   r"Permission is hereby granted")
# files of this kit that are derived from PostgreSQL sources but live outside pg/
DERIVED = ["ile/ile_bitutils.c", "ile/pg_config.h", "ile/pg_config_os.h",
           "ile/pg_config_paths.h", "build/pg_config_paths.h"]
# files of pg/ that differ from the PostgreSQL release they were taken from
CHANGED = ["pg/src/include/mb/pg_wchar.h", "pg/src/include/port.h",
           "pg/src/interfaces/libpq/fe-connect.c", "pg/src/interfaces/libpq/fe-lobj.c",
           "pg/src/port/pg_strong_random.c"]
# where a notice ends: the last words of the disclaimers in use
END = re.compile(r"SUCH DAMAGE\.$|PERFORMANCE OF THIS SOFTWARE\.$")


def comments(text):
    """the /* ... */ blocks of a C file, each as clean lines"""
    for m in re.finditer(r"/\*(.*?)\*/", text, re.S):
        lines = []
        for ln in m.group(1).split("\n"):
            ln = re.sub(r"^\s*\*? ?", "", ln.rstrip().expandtabs(4), count=1)
            lines.append(ln.rstrip())
        while lines and not lines[0]:
            lines.pop(0)
        while lines and not lines[-1]:
            lines.pop()
        yield lines


def notice_of(lines):
    """the notice inside a comment block: from its first copyright line on"""
    for i, ln in enumerate(lines):
        if re.search(r"copyright\s*\(c\)|copyright\s+[12]", ln, re.I) and "Portions Copyright" not in ln:
            # a file header in front of the notice (name, description) is left out
            lines = lines[i:]
            break
    for i in range(len(lines) - 1, -1, -1):
        if END.search(lines[i]):
            return lines[:i + 1]        # and what follows the disclaimer (file name, version line)
    return lines


files = []
for d, _, names in os.walk(pg):
    for n in names:
        if n.endswith((".c", ".h")):
            files.append(os.path.join(d, n))
files.sort()

groups = {}          # text of a notice -> files that carry it
changed = list(CHANGED)
if len(sys.argv) > 2:
    real = []
    for f in files:
        rel = os.path.relpath(f, pg)
        up = os.path.join(sys.argv[2], rel)
        if not os.path.exists(up) or open(up, "rb").read() != open(f, "rb").read():
            real.append("pg/" + rel.replace(os.sep, "/"))
    if real != sorted(CHANGED):
        sys.exit("the list CHANGED in this script is not what differs from %s:\n  %s" % (sys.argv[2], "\n  ".join(real)))
    print("changed files checked against %s: %d" % (sys.argv[2], len(real)))
for f in files:
    rel = os.path.relpath(f, kit).replace(os.sep, "/")
    text = open(f, encoding="utf-8", errors="replace").read()
    for blk in comments(text):
        body = "\n".join(blk)
        if not GRANT.search(body):
            continue
        if "PostgreSQL Global Development Group" in body and "Regents" not in body and "Copyright (c) 19" not in body:
            continue
        body = "\n".join(notice_of(blk))
        key = hashlib.sha1(re.sub(r"\s+", " ", body).encode()).hexdigest()
        groups.setdefault(key, [body, []])[1].append(rel)

out = []
w = out.append
w("XDBARDDRV - notices for the code of other authors in this package")
w("=" * 66)
w("")
w("This file belongs to every copy of the package: to the sources and to the")
w("objects save file, whose service program LIBPQ is built from the code")
w("named here.  It is written by build/gen_notices.py from the source files;")
w("the notices below are the words of their authors.")
w("")
w("Everything in the package that is not named in this file was written for")
w("XDBARDDRV; its licence is in the file LICENSE.")
w("")
w("")
w("1. PostgreSQL")
w("-------------")
w("")
w("Applies to the directory pg/ (the client library libpq of PostgreSQL and")
w("the files of src/common, src/port and src/include it needs), to the files")
for d in DERIVED:
    w("    " + d)
w("which are taken or adapted from PostgreSQL sources, and to the service")
w("program LIBPQ built from them.  These files of pg/ were changed for ILE C:")
for c in changed:
    w("    " + c)
w("")
for ln in open(os.path.join(pg, "COPYRIGHT"), encoding="utf-8").read().rstrip().split("\n"):
    w("    " + ln if ln else "")
w("")
w("")
w("2. Notices inside single files of pg/")
w("-------------------------------------")
w("")
w("Some files of PostgreSQL come from other projects and carry the notice of")
w("their first authors in addition to the one above.")
n = 0
for key in sorted(groups, key=lambda k: groups[k][1][0]):
    body, fl = groups[key]
    n += 1
    w("")
    w("2.%d  %s" % (n, ", ".join(fl)))
    w("")
    for ln in body.split("\n"):
        w("    " + ln if ln else "")
w("")
w("")
w("3. Other")
w("--------")
w("")
w("pg/src/common/wchar.c contains a character width function by Markus Kuhn")
w("(2001-09-08), placed in the public domain by its author.")
w("")
w("The tables in pg/src/include/common/unicode_*_table.h are generated by")
w("PostgreSQL from the Unicode Character Database of Unicode, Inc.")
w("")

dst = os.path.join(kit, "doc", "THIRD-PARTY-NOTICES.txt")
with open(dst, "w", encoding="utf-8", newline="\n") as fh:
    fh.write("\n".join(out))
print("%s: %d lines, %d notices in %d files, %d changed files" %
      (dst, len(out), n, sum(len(g[1]) for g in groups.values()), len(changed)))
