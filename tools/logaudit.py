#!/usr/bin/env python3
# ============================================================================
# logaudit.py - find any ORO log line that can be written more than once a second.
# ----------------------------------------------------------------------------
# Why (2026-09-13): a stale SetFloat left one D3D9ERROR per frame in the 260906
# release - 16,252 lines in a twenty-minute flight, in the log a tester then sends
# us. That one was the CLIENT logging our failed call, not a log call of ours, but
# it is the same disease: something writes at frame rate and nobody notices until
# a user opens the file.
#
# THE RULE: nothing ORO writes may fire per frame. A diagnostic writes AT MOST one
# line per REAL second, and only above the configured debug level.
#
# This finds every oapiWriteLog* call, works out the function it sits in, and
# decides whether that function can run at frame rate. A call inside one of those
# must carry a guard: a one-shot latch (a static bool / "once" / "warned" flag),
# a real-time throttle, or a debug-level test.
#
# Heuristic, deliberately noisy in the safe direction: it would rather flag a
# guarded call for a human to read than miss an unguarded one.
#
# It also refuses a RAW oapiWriteLog anywhere but inside OroLog() itself: the level
# and the rate limit only work if everything goes through the one door.
#
# KNOWN AND SETTLED (checked 2026-09-13, so a future run does not re-litigate them):
#   OroTileTree::Open      - logs a missing/!bad archive, but Open() early-returns on
#                            a cached result FAILURE INCLUDED, so it fires once per
#                            body, not once per call.
#   SampleHullPoints       - the caller sets e.hullSampled = true unconditionally, so
#                            the walk runs once per slot enlistment.
#
# USAGE   python logaudit.py            (exit 1 if anything is unguarded)
#         python logaudit.py --all      list every call site, guarded or not
# ============================================================================
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ORO = os.path.abspath(os.path.join(HERE, '..'))

# Functions that run at frame rate, directly or by being called from one.
# clbkPreStep runs per step; the render proc and DrawOverlay run per frame;
# clbkProcessKeyboardImmediate runs EVERY frame including paused (invariant 1).
PER_FRAME = re.compile(
    r"clbkPreStep|clbkPostStep|clbkProcessKeyboardImmediate|DrawOverlay|Draw\w*Poly"
    r"|^Update|^Sense|^Build|^Project|^Push|^Emit|^Fill|Render|Paint|^Cm\w|Sample|Tile",
    re.I)

# ⚠️ NO WORD BOUNDARIES on the latch words. The two pool-full warnings are guarded by
# s_aurPoolLogged / s_ltgPoolLogged, and "\blogged\b" does not match inside a camelCase
# name - so the two best-guarded calls in the project were the ones being flagged.
GUARD = re.compile(
    r"\bstatic\b|once|warn|report|\bdone\b|Done|first|logged|latch|primed"
    r"|nextLog|lastLog|logT|dbgT|s_dbg|ShadowDebug|MaskDebug|dropDbg|g_dbg"
    r"|DIAG|>= *nextD|LogEvery|ORO_LOG_EVERY", re.I)


# Settled verdicts. A call whose FUNCTION is listed here is guarded by something this
# script cannot see - a cached early-return, or a latch at the call site. Each carries
# the reason, so the next person reads a finding rather than re-deriving one. A NEW
# call site in a per-frame function is still flagged; this only silences these two.
ACCEPTED = {
    "Open": "OroTileTree::Open early-returns on a cached result, FAILURE INCLUDED, "
            "so a missing archive logs once per body - not once per call",
    "SampleHullPoints": "the caller sets e.hullSampled = true unconditionally, so the "
                        "walk runs once per slot enlistment",
    "BuildShell": "both call sites are gated on !e.shellBuilt, which BuildShell sets - "
                  "once per slot, at first heat",
    "UpdateBellGlow": "the gimbal derivation sits inside if (bellVessel != hObj) - it "
                      "runs when the bell's vessel CHANGES, not per frame",
    "UpdateParticles": "the capability lines are inside if (!prtTexTried); the upload "
                       "failure clears prtTexMode, which gates its own loop",
    "SenseRain": "the rain-shield load is inside a class-change latch "
                 "(if (_stricmp(cls, s_shieldClass) != 0), which then copies cls)",
    "BuildRainCloudTex": "once per session by construction - its own header says so, "
                         "and the caller is a detail-level change",
}


def functions(src):
    """(name, start, end) for every top-level function body, by column-0 braces."""
    out = []
    lines = src.split("\n")
    open_at = None
    name = "?"
    for i, l in enumerate(lines):
        if l.startswith("{") and open_at is None:
            open_at = i
            for k in range(i - 1, max(0, i - 6), -1):
                m = re.search(r"([A-Za-z_~][\w:<>~]*)\s*\(", lines[k])
                if m:
                    name = m.group(1)
                    break
        elif l.startswith("}") and open_at is not None:
            out.append((name, open_at, i))
            open_at = None
            name = "?"
    return out


def main():
    show_all = "--all" in sys.argv
    total = flagged = 0
    for fn in sorted(f for f in os.listdir(ORO) if f.endswith(".cpp")):
        src = open(os.path.join(ORO, fn), encoding="utf-8", errors="replace").read()
        lines = src.split("\n")
        funcs = functions(src)
        rows = []
        for i, l in enumerate(lines):
            raw = "oapiWriteLog" in l
            if not raw and "OroLog(" not in l and "ORO_LOG_EVERY" not in l:
                continue
            total += 1
            if raw and fn != "OroModule.cpp":
                # OroModule.cpp holds the ONE oapiWriteLog, inside OroLog itself.
                print("--- %s" % fn)
                print("  %5d  RAW oapiWriteLog - must go through OroLog()" % (i + 1))
                flagged += 1
                continue
            owner = next((f for f in funcs if f[1] <= i <= f[2]), ("<file scope>", 0, 0))
            # ⚠️ TEST THE BARE NAME. The captured name is qualified
            # (OroModule::UpdateFog), and the ^-anchored alternatives below can never
            # match one - so every Update*/Sense*/Build*/Push* in the module class was
            # silently exempt until a planted offender went unreported.
            hot = bool(PER_FRAME.search(owner[0].split("::")[-1]))
            # a guard anywhere in the 14 lines above the call, inside the function.
            # ORO_LOG_EVERY needs no other guard - it IS the rate limit.
            lo = max(owner[1], i - 14)
            guarded = ("ORO_LOG_EVERY" in l
                       or bool(GUARD.search("\n".join(lines[lo:i + 1]))))
            bare = owner[0].split("::")[-1]
            ok = ACCEPTED.get(bare)
            if show_all or (hot and not guarded and not ok):
                rows.append((i + 1, owner[0], hot, guarded, l.strip()[:72]))
            if hot and not guarded and not ok:
                flagged += 1
            elif hot and not guarded and ok and show_all:
                print("  (accepted: %s - %s)" % (bare, ok))
        if rows:
            print("--- %s" % fn)
            for ln, ow, hot, g, txt in rows:
                tag = "UNGUARDED" if (hot and not g) else ("guarded" if hot else "cold")
                print("  %5d  %-26s %-10s %s" % (ln, ow[:26], tag, txt))
    print("\n%d log call sites, %d unguarded in a per-frame path" % (total, flagged))
    return 1 if flagged else 0


if __name__ == "__main__":
    sys.exit(main())
