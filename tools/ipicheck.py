#!/usr/bin/env python3
# ============================================================================
# ipicheck.py - every uniform ORO pushes must be one its entry point USES.
# ----------------------------------------------------------------------------
# Why this exists (2026-09-13): PSGloom stopped referencing fTime on 2026-09-06,
# when the windscreen runners moved from a rate x clock to the host-integrated
# fRunPh. The compiler then strips the unused uniform out of the constant table,
# so the SetFloat("fTime") left behind in OroModule.cpp FAILED - and the client
# logs a D3D9ERROR every time it fails. It shipped in ORO-beta-260906.zip and put
#
#   16,252 lines into one twenty-minute flight
#
# of a 17,134-line Orbiter.log, in the log a tester then sends us, while client
# patch (q) exists precisely to keep that log worth reading. Nothing warned: it is
# not a compile error, not a link error, and the effect looks perfectly correct,
# because the value was never used.
#
# So: read the entry point each pIPI* handle is created with, brace-match that
# function in the shader, strip comments, and check every pushed name appears in
# the CODE. Comments do not count - fTime was mentioned three times in PSGloom's
# comments while being referenced zero times in its body.
#
# The reverse direction is reported too but is NOT a failure: a uniform an entry
# point reads while nothing pushes it is legal (it may be pushed by another effect
# sharing the file, or deliberately left at zero), and it fails silently rather
# than loudly, so it is worth SEEING but not worth refusing a build over.
#
# USAGE   python ipicheck.py            (exit 1 if any push is dead)
# ============================================================================
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ORO = os.path.abspath(os.path.join(HERE, '..'))


def strip_comments(t):
    t = re.sub(r"/\*.*?\*/", "", t, flags=re.S)
    return re.sub(r"//[^\n]*", "", t)


def entry_body(src, name):
    m = re.search(r"^[A-Za-z0-9_]+\s+" + name + r"\s*\(", src, re.M)
    if not m:
        return None
    i = src.index("{", m.end())
    d = 0
    for k in range(i, len(src)):
        if src[k] == "{":
            d += 1
        elif src[k] == "}":
            d -= 1
            if d == 0:
                return src[m.start():k + 1]
    return src[m.start():]


def main():
    cpp = open(os.path.join(ORO, 'OroModule.cpp'), encoding='utf-8', errors='replace').read()
    hlsl = open(os.path.join(ORO, 'orofx.hlsl'), encoding='utf-8', errors='replace').read()

    # handle -> entry point, from the CreateIPInterface calls themselves
    entry = dict(re.findall(r"(\w+)\s*=\s*\w+->CreateIPInterface\s*\(\s*\"[^\"]+\"\s*,\s*\"(\w+)\"",
                            cpp))
    if not entry:
        print('no CreateIPInterface calls found - has the call shape changed?')
        return 2

    bodies = {}
    for h, e in entry.items():
        b = entry_body(hlsl, e)
        if b is None:
            print('*** %s: entry point %s is not in orofx.hlsl' % (h, e))
            return 2
        bodies[h] = strip_comments(b)

    pushes = {}
    for h, u, ln in [(m.group(1), m.group(2), cpp[:m.start()].count('\n') + 1)
                     for m in re.finditer(r"(\w+)->Set(?:Float|Vector|Int|Bool|Temp\w*)"
                                          r"\s*\(\s*\"(\w+)\"", cpp)]:
        pushes.setdefault(h, {}).setdefault(u, []).append(ln)

    bad = 0
    print('%-14s %-14s %s' % ('handle', 'entry point', 'pushes'))
    for h in sorted(pushes):
        if h not in bodies:
            continue
        dead = [(u, l) for u, l in sorted(pushes[h].items())
                if not re.search(r"\b" + u + r"\b", bodies[h])]
        print('  %-12s %-14s %3d   %s'
              % (h, entry[h], len(pushes[h]),
                 'ok' if not dead else '*** %d DEAD ***' % len(dead)))
        for u, lns in dead:
            bad += 1
            print('      %-12s pushed at OroModule.cpp:%s but %s never uses it'
                  % (u, ','.join(str(x) for x in lns), entry[h]))

    # the other direction: read but never pushed. Quiet, and legal.
    decl = set(re.findall(r"uniform\s+extern\s+\w+\s+(\w+)", hlsl))
    quiet = []
    for h in sorted(bodies):
        for u in sorted(decl):
            if re.search(r"\b" + u + r"\b", bodies[h]) and u not in pushes.get(h, {}):
                quiet.append('%s reads %s, nothing pushes it' % (entry[h], u))
    if quiet:
        print('\nread but not pushed (silent, and legal - just so it is seen):')
        for q in quiet:
            print('   ' + q)

    print('\n%s' % ('*** %d DEAD PUSH(ES) - each one is a D3D9ERROR per frame ***' % bad
                    if bad else 'every push lands on a uniform its entry point uses'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
