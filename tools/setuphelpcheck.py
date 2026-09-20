#!/usr/bin/env python3
"""setuphelpcheck.py - every control of the D3D9Client Advanced Setup dialog has a help entry.

ORO patch (am) gives the dialog context help (the title-bar "?" and F1) from a table in
OVP/D3D9Client/SetupHelp.cpp. A table is a claim; this script checks it against the dialog
resource (IDD_D3D9SETUP in D3D9Client.rc) so a control added later cannot ship silent, and
so a stale entry for a control that no longer exists is noticed. It also insists the table
is plain ASCII, because the dialog font and the compiler's code page are.

Usage:  python tools/setuphelpcheck.py [clone-root]
        default clone root: C:/OrbiterDev/orbiter
Exit code 0 = clean, 1 = findings.
"""
import re, sys, os

root = sys.argv[1] if len(sys.argv) > 1 else 'C:/OrbiterDev/orbiter'
rc_path = os.path.join(root, 'OVP/D3D9Client/D3D9Client.rc')
cpp_path = os.path.join(root, 'OVP/D3D9Client/SetupHelp.cpp')

def read(p):
    with open(p, 'rb') as f:
        return f.read()

# ---- the dialog: every control id (not IDC_STATIC) and every group-box caption ----
rc = read(rc_path).decode('latin-1')
m = re.search(r'IDD_D3D9SETUP DIALOGEX.*?^\{(.*?)^\}', rc, re.S | re.M)
if not m:
    print('FAIL: IDD_D3D9SETUP block not found in', rc_path); sys.exit(1)
block = m.group(1)
rc_ids = []
rc_groups = []
for line in block.splitlines():
    s = line.strip()
    if not s:
        continue
    g = re.match(r'GROUPBOX\s+"([^"]*)"', s)
    if g:
        rc_groups.append(g.group(1))
        continue
    ids = re.findall(r'\b(IDC_[A-Z0-9_]+|IDOK|IDCANCEL)\b', s)
    ids = [i for i in ids if i != 'IDC_STATIC']
    if ids:
        rc_ids.append(ids[0])
if not rc_ids:
    print('FAIL: no control ids parsed from the dialog block'); sys.exit(1)

# ---- the table ----
raw = read(cpp_path)
non_ascii = [i for i, b in enumerate(raw) if b > 0x7f]
cpp = raw.decode('latin-1')
entries = re.findall(r'\{\s*(IDC_[A-Z0-9_]+|IDOK|IDCANCEL)\s*,\s*"', cpp)
aliases = re.findall(r'\{\s*(IDC_[A-Z0-9_]+)\s*,\s*(IDC_[A-Z0-9_]+)\s*\}', cpp)
groups = re.findall(r'\{\s*"([^"]+)"\s*,\s*\n?\s*"', cpp)

covered = set(entries) | {a for a, t in aliases}
findings = []
for i in rc_ids:
    if i not in covered:
        findings.append('control %s in the dialog has no help entry' % i)
for a, t in aliases:
    if t not in entries:
        findings.append('alias %s -> %s points at an id with no entry' % (a, t))
for e in entries:
    if e not in rc_ids:
        findings.append('entry %s names a control the dialog does not have (stale?)' % e)
seen = set()
for e in entries:
    if e in seen:
        findings.append('entry %s appears twice' % e)
    seen.add(e)
for g in rc_groups:
    if g not in groups:
        findings.append('group box "%s" has no note' % g)
for g in groups:
    if g not in rc_groups:
        findings.append('group note "%s" names a group box the dialog does not have' % g)
if non_ascii:
    line = raw[:non_ascii[0]].count(b'\n') + 1
    findings.append('%d non-ASCII byte(s) in SetupHelp.cpp, first at line %d' % (len(non_ascii), line))

print('dialog: %d controls, %d group boxes; table: %d entries, %d aliases, %d group notes'
      % (len(rc_ids), len(rc_groups), len(entries), len(aliases), len(groups)))
if findings:
    for f in findings:
        print('FAIL:', f)
    sys.exit(1)
print('OK: every control and group box of IDD_D3D9SETUP has help text, and the table is ASCII.')
