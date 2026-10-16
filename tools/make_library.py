#!/usr/bin/env python3
"""Build flash_data/LIB: VMU programs (VMS + VMI) and INDEX.TXT ("FILE.VMS|Title", sorted).

usage: make_library.py OUT_DIR SOURCE...
A SOURCE is a directory searched recursively for .VMI files (programs only, data files
skipped) or a single .vms file, optionally given as path=Title to name it.
"""
import os, shutil, struct, sys

def vmi_title(vmi):
    b = open(vmi, 'rb').read()
    return b[4:36].split(b'\0')[0].decode('shift_jis', 'replace').strip(), struct.unpack('<H', b[0x64:0x66])[0]

def main():
    out, sources = sys.argv[1], sys.argv[2:]
    os.makedirs(out, exist_ok=True)
    entries = []
    for src in sources:
        if os.path.isdir(src):
            for root, _, files in os.walk(src):
                for f in files:
                    if not f.upper().endswith('.VMI'):
                        continue
                    vmi = os.path.join(root, f)
                    vms = vmi[:-4] + vmi[-4:].replace('I', 'S').replace('i', 's')
                    title, mode = vmi_title(vmi)
                    if not mode & 2 or not os.path.exists(vms):
                        continue  # data file, not a program
                    base = os.path.splitext(f)[0].upper()[:8]
                    shutil.copy(vms, os.path.join(out, base + '.VMS'))
                    shutil.copy(vmi, os.path.join(out, base + '.VMI'))
                    entries.append((title, base + '.VMS'))
        else:
            path, _, title = src.partition('=')
            base = os.path.splitext(os.path.basename(path))[0].upper()[:8]
            shutil.copy(path, os.path.join(out, base + '.VMS'))
            vmi = os.path.splitext(path)[0] + '.VMI'
            if os.path.exists(vmi):
                shutil.copy(vmi, os.path.join(out, base + '.VMI'))
            entries.append((title or base, base + '.VMS'))
    entries.sort(key=lambda e: e[0].lower())
    with open(os.path.join(out, 'INDEX.TXT'), 'w') as f:
        for title, name in entries:
            f.write(f'{name}|{title}\n')
    print(f'{len(entries)} programs in {out}')

if __name__ == '__main__':
    main()
