#!/usr/bin/env python3
"""chk_savf.py - searches a save file (as a stream file) for names that must not leave the site.

A save file says more than the objects in it: the machine type, model and serial number and
the system name of the system that made it, the user and the system that created each object,
the system name and the source path in every compiled module.  The only sure way to know what
a file gives away is to search its bytes.

    python3 chk_savf.py <file.savf> <word> [<word> ...]

Each word is looked for in EBCDIC (as given, in upper case and in lower case), in ASCII and in
UTF-16.  The 16 bytes that end each 528-byte record are left out, so a word is also found
across a record boundary.  For each word: the number of places and the text around the first
few.  Ends with 1 when a word was found, 0 when none was, 2 on a wrong call.

The words are given on the call and are not kept in this script: it holds no name of any site.
The file header (record 1) holds the machine type and model; -h shows it.
"""
import re
import sys


def stream(path):
    raw = open(path, 'rb').read()
    count, rest = divmod(len(raw), 528)
    if rest or not count:
        sys.exit('%s: %d bytes is not a whole number of 528-byte records' % (path, len(raw)))
    return b''.join(raw[i * 528:i * 528 + 512] for i in range(count)), count


def text(part):
    t = part.decode('cp037', 'replace')
    t = ''.join(c if c.isprintable() and ord(c) < 256 else '.' for c in t)
    return re.sub(r'\.{3,}', '~', t)


def main(argv):
    header = '-h' in argv
    argv = [a for a in argv if a != '-h']
    if len(argv) < 2 and not (header and len(argv) == 1):
        print('usage: python3 chk_savf.py [-h] <file.savf> <word> [<word> ...]')
        return 2
    data, count = stream(argv[0])
    print('%s: %d records' % (argv[0], count))
    if header:
        print('  file header: %s' % text(data[:64]))
    found = 0
    for word in argv[1:]:
        forms = {word.encode('cp037'): 'EBCDIC', word.upper().encode('cp037'): 'EBCDIC',
                 word.lower().encode('cp037'): 'EBCDIC', word.encode('ascii', 'replace'): 'ASCII',
                 word.encode('utf-16-be'): 'UTF-16'}
        places = []
        for form, kind in forms.items():
            places += [(m.start(), kind) for m in re.finditer(re.escape(form), data)]
        places = sorted(set(places))
        print('  %-14s %d' % (word, len(places)))
        found += len(places)
        for at, kind in places[:3]:
            print('      record %d (%s): %s' % (at // 512 + 1, kind, text(data[max(0, at - 40):at + 60])))
        if len(places) > 3:
            print('      ...')
    return 1 if found else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
