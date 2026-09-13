#!/usr/bin/env python
# ---------------------------------------------------------------------------
# scnfreeze.py - his flown scenarios become the source of truth for STATE.
#
# WHY THIS EXISTS. tools/scenarios.py computes every scenario's state from first
# principles, which is what made the set buildable without flying it. Then he flew all
# sixteen, adjusted cameras, moved vessels, added a DG-S, and re-saved - and Orbiter's
# own save is now a BETTER answer than the computation for those scenarios, because it is
# the state he judged on screen. His ruling, 2026-09-13: "All my changes must remain as
# the true scenarios now."
#
# So the division of labour changes. The TABLE in scenarios.py still owns the prose - the
# lead, the look list, the setup list, the picture, the file name - so a description can
# still never drift from the scenario beside it. THIS file owns the three blocks Orbiter
# writes: the environment Date, the CAMERA block and the SHIPS block, captured verbatim.
# The generator reads scenarios.frozen.json and emits those blocks unchanged.
#
# The computed path is NOT deleted: it stays in the table as the derivation, with the
# geometry comments that explain why each scenario is where it is, and it is what a NEW
# scenario is built from. Freezing simply says "this one has been flown, and the flight
# wins".
#
# RUN IT after he re-flies and re-saves a scenario:
#     python tools/scnfreeze.py            # capture the live files
#     python tools/scnfreeze.py --check    # verify the frozen data still matches them
#
# ⚠️ IT READS THE LIVE TREE, so run it only when the sim tree holds what he intends to
# ship. The manifest guard in scenarios.py protects the files themselves; this protects
# the ability to REGENERATE them.
# ---------------------------------------------------------------------------
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scnstate as S                                        # noqa: E402

ROOT = S.orbiter_root()
SCN_DIR = os.path.join(ROOT, 'Scenarios', 'ORO')
FROZEN = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'scenarios.frozen.json')


def block(lines, name):
    """The body between BEGIN_<name> and END_<name>, verbatim, as a list of lines."""
    try:
        a = lines.index('BEGIN_' + name)
    except ValueError:
        return None
    for b in range(a + 1, len(lines)):
        if lines[b] == 'END_' + name:
            return lines[a + 1:b]
    return None


def capture(path):
    """slug -> everything after END_DESC, verbatim.

    ⚠️ THE SPLIT IS AT END_DESC, AND THAT IS DELIBERATE. A first cut froze three named
    blocks - the environment Date, CAMERA and SHIPS - and SILENTLY DROPPED the rest: two
    scenarios lost the MFD configuration he had set (the Orbit MFD's PROJ/FRAME/REF, the
    Surface MFD's SPDMODE), because the generator re-emits an MFD block as a bare TYPE
    line. Caught by diffing the regenerated files against his before anything was written.
    Freezing the whole tail removes the question: the generator owns the description, and
    everything Orbiter itself writes is his.
    """
    with open(path, 'r', encoding='latin-1') as f:
        lines = [ln.rstrip('\r\n') for ln in f]

    url = block(lines, 'URLDESC')
    if not url:
        return None, None
    slug = url[0].strip().split(chr(92))[-1]

    try:
        e = lines.index('END_DESC')
    except ValueError:
        print('  SKIPPED (no END_DESC): %s' % os.path.basename(path))
        return None, None
    tail = lines[e + 1:]
    while tail and not tail[0].strip():          # the blank line after END_DESC is ours
        tail.pop(0)
    while tail and not tail[-1].strip():         # and the trailing one is re-added on write
        tail.pop()

    # THE HOUSE RULES STILL HOLD even though the block is frozen now: Orbit left, Surface
    # right, an empty VC block before the ships (his 2026-09-13 ruling). The generator used
    # to guarantee these by construction; freezing would have quietly retired them, so the
    # freeze refuses a scenario that breaks one instead.
    def mfd(side):
        for i, ln in enumerate(tail):
            if ln.strip() == 'BEGIN_MFD ' + side:
                for j in range(i + 1, len(tail)):
                    if tail[j].strip().startswith('TYPE '):
                        return tail[j].split()[-1]
                    if tail[j].strip() == 'END_MFD':
                        break
        return None

    bad = []
    if mfd('Left') != 'Orbit':
        bad.append('left MFD is %s, not Orbit' % mfd('Left'))
    if mfd('Right') != 'Surface':
        bad.append('right MFD is %s, not Surface' % mfd('Right'))
    if 'BEGIN_VC' not in [t.strip() for t in tail]:
        bad.append('no BEGIN_VC block')
    if bad:
        print('  !! %s: %s' % (os.path.basename(path), '; '.join(bad)))

    return slug, {
        'file': os.path.relpath(path, SCN_DIR).replace(os.sep, '/'),
        'tail': tail,
    }


def scan():
    out = {}
    for dirpath, _, names in os.walk(SCN_DIR):
        for n in sorted(names):
            if not n.lower().endswith('.scn'):
                continue
            slug, data = capture(os.path.join(dirpath, n))
            if slug:
                out[slug] = data
            else:
                print('  SKIPPED (no URLDESC): %s' % n)
    return out


def main(argv):
    live = scan()
    if '--check' in argv:
        if not os.path.isfile(FROZEN):
            print('no frozen data yet - run without --check first')
            return 1
        with open(FROZEN, encoding='utf-8') as f:
            old = json.load(f)
        bad = 0
        for slug in sorted(set(list(old) + list(live))):
            if slug not in old:
                print('  NEW      %s' % slug); bad += 1
            elif slug not in live:
                print('  MISSING  %s (frozen but no scenario on disk)' % slug); bad += 1
            elif old[slug] != live[slug]:
                print('  CHANGED  %s' % slug); bad += 1
        print('%d of %d frozen scenarios differ from the live tree' % (bad, len(live)))
        return 1 if bad else 0

    blob = json.dumps(live, indent=1, sort_keys=True, ensure_ascii=True) + '\n'
    with open(FROZEN, 'w', encoding='utf-8', newline='\n') as f:
        f.write(blob)
    print('froze %d scenarios -> %s' % (len(live), os.path.relpath(FROZEN, ROOT)))
    for slug in sorted(live):
        print('   %-22s %s' % (slug, live[slug]['file']))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
