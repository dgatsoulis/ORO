#!/usr/bin/env python3
# ============================================================================
# ringslab.py - the ring NEAR FIELD's two closed forms, checked against brute-force
# numeric integration (ORO rings, round 2, 2026-09-12).
# ----------------------------------------------------------------------------
# OroRings.cpp integrates the ring's vertical profile analytically in two places, and
# both are six lines with a sign and a branch in them:
#
#   NearSlabTau(tauv, z0, w, s1, s2)  the optical depth of the slab [s1,s2] along a ray
#                                     that starts at height z0 and climbs at w per metre
#   NearSunT(tauv, z)                 the same integral's TAIL, from a height out along
#                                     the sun line - how much ring the sunlight crossed
#
# A wrong sign or a wrong branch in either produces "the fog looks wrong", which is not
# a bug report anyone can act on and which a screenshot cannot localise. So they are
# checked here against a 400k-point midpoint rule over random cases, plus the two
# identities that have to hold exactly (the column integrates to tau; at the midplane
# exactly half of it is above you).
#
# This is the same discipline as tools/ringprofile_test for round 1: verify the maths
# offline, before it reaches his sim.
#
#   python ringslab.py
# ============================================================================
import math
import random

CORE_H = 10.0        # NEAR_CORE_H
HALO_H = 250.0       # NEAR_HALO_H at `Dust halo` 1.0
HALO_F = 0.10        # NEAR_HALO_F


# --- the model, transcribed from OroRings.cpp -------------------------------
def kap(tau, Hc, Hh, f, z):
    Ac = (1.0 - f) * tau / (2.0 * Hc)
    Ah = (f * tau / (2.0 * Hh)) if f > 0 else 0.0
    return Ac * math.exp(-abs(z) / Hc) + (Ah * math.exp(-abs(z) / Hh) if Ah > 0 else 0.0)


def NearF(u, H):
    return H * (1.0 - math.exp(-u / H)) if u >= 0.0 else -H * (1.0 - math.exp(u / H))


def slab_closed(tau, Hc, Hh, f, z0, w, s1, s2):
    if tau <= 0.0 or s2 <= s1:
        return 0.0
    if abs(w) < 1e-7:
        return kap(tau, Hc, Hh, f, z0) * (s2 - s1)
    Ac = (1.0 - f) * tau / (2.0 * Hc)
    Ah = (f * tau / (2.0 * Hh)) if f > 0 else 0.0
    u1, u2 = z0 + w * s1, z0 + w * s2
    t = Ac * (NearF(u2, Hc) - NearF(u1, Hc))
    if Ah > 0:
        t += Ah * (NearF(u2, Hh) - NearF(u1, Hh))
    t /= w
    return t if t > 0 else 0.0


def NearGsun(z, H):
    return math.exp(-z / H) if z >= 0.0 else (2.0 - math.exp(z / H))


def sunT_closed(tau, Hc, Hh, f, z, sunN):
    if tau <= 0.0:
        return 1.0
    aw = max(abs(sunN), 0.02)
    zs = z if sunN >= 0.0 else -z
    Ac = (1.0 - f) * tau / (2.0 * Hc)
    Ah = (f * tau / (2.0 * Hh)) if f > 0 else 0.0
    t = Ac * Hc * NearGsun(zs, Hc)
    if Ah > 0:
        t += Ah * Hh * NearGsun(zs, Hh)
    return math.exp(-t / aw)


# --- brute force ------------------------------------------------------------
def slab_numeric(tau, Hc, Hh, f, z0, w, s1, s2, n=400000):
    h = (s2 - s1) / n
    return sum(kap(tau, Hc, Hh, f, z0 + w * (s1 + (i + 0.5) * h)) for i in range(n)) * h


def sunT_numeric(tau, Hc, Hh, f, z, sunN, n=200000, reach=60000.0):
    aw = max(abs(sunN), 0.02)
    w = aw if sunN >= 0.0 else -aw
    h = reach / n
    tot = sum(kap(tau, Hc, Hh, f, z + w * ((i + 0.5) * h)) for i in range(n)) * h
    return math.exp(-tot)


def main():
    random.seed(4)
    fail = 0

    print('--- the column identity: kappa integrated over all z must be tau ---')
    for tau in (0.08, 0.5, 1.8, 4.0):
        for f in (0.0, 0.10, 0.20):
            n, span = 400000, 40000.0
            h = span / n
            col = sum(kap(tau, CORE_H, HALO_H, f, -span / 2 + (i + 0.5) * h) for i in range(n)) * h
            rel = abs(col - tau) / tau
            if rel > 2e-4:
                fail += 1
            print('  tau %.2f  halo share %.2f  ->  column %.6f  (rel err %.2e)%s'
                  % (tau, f, col, rel, '   FAIL' if rel > 2e-4 else ''))

    print('\n--- NearSlabTau: closed form vs a 400k-point midpoint rule ---')
    worst = 0.0
    for _ in range(40):
        c = (random.uniform(0.01, 4.0), CORE_H, random.uniform(62.5, 500.0),
             random.choice([0.0, 0.05, 0.10, 0.20]), random.uniform(-800.0, 800.0),
             random.choice([random.uniform(-1, 1), random.uniform(-0.02, 0.02)]),
             random.choice([0.6, 3.0, 12.0, 50.0, 180.0, 450.0]), 0.0)
        c = c[:7] + (c[6] * random.uniform(2.0, 5.0),)
        a, b = slab_closed(*c), slab_numeric(*c)
        worst = max(worst, abs(a - b) / max(b, 1e-12))
    if worst > 1e-3:
        fail += 1
    print('  worst relative error over 40 random cases: %.3e%s'
          % (worst, '   FAIL' if worst > 1e-3 else '   (the midpoint rule\'s own error)'))

    print('\n--- NearSunT: closed form vs numeric ---')
    worst2 = 0.0
    for _ in range(30):
        tau = random.uniform(0.01, 4.0)
        Hh = random.uniform(62.5, 500.0)
        f = random.choice([0.0, 0.10, 0.20])
        z = random.uniform(-600.0, 600.0)
        sn = random.choice([random.uniform(0.05, 1.0), random.uniform(-1.0, -0.05)])
        worst2 = max(worst2, abs(sunT_closed(tau, CORE_H, Hh, f, z, sn)
                                 - sunT_numeric(tau, CORE_H, Hh, f, z, sn)))
    if worst2 > 1e-3:
        fail += 1
    print('  worst absolute transmittance error over 30 cases: %.3e%s'
          % (worst2, '   FAIL' if worst2 > 1e-3 else ''))

    print('\n--- the midplane identity: exactly half the column is above you ---')
    sn = math.sin(math.radians(26.73))
    for tau in (0.08, 1.8):
        got = sunT_closed(tau, CORE_H, HALO_H, HALO_F, 0.0, sn)
        want = math.exp(-(tau / 2.0) / sn)
        if abs(got - want) > 1e-9:
            fail += 1
        print('  tau %.2f  ->  %.9f   want exp(-tau/2/sin 26.73) = %.9f%s'
              % (tau, got, want, '   FAIL' if abs(got - want) > 1e-9 else ''))

    print('\n--- what it should look like (visibility = the mean free path) ---')
    for nm, tau in (('B ring', 1.8), ('A ring', 0.5), ('C ring', 0.10),
                    ('Cassini Division', 0.08), ('a real gap', 0.01)):
        k = kap(tau, CORE_H, HALO_H, HALO_F, 0.0)
        print('  %-18s tau %.2f  ->  kappa %.5f /m  ->  see %8.1f m' % (nm, tau, k, 1.0 / k))

    print('\n--- the sun, from just under the sheet at Saturn\'s 26.73 deg tilt ---')
    for nm, tau in (('B ring', 1.8), ('A ring', 0.5), ('Cassini Division', 0.08)):
        print('  %-18s -> %5.1f%% of the sunlight gets through'
              % (nm, sunT_closed(tau, CORE_H, HALO_H, HALO_F, -30.0, sn) * 100.0))

    print('\n--- the sheet overlap: what a downward ray picks up within the 900 m reach ---')
    for nm, tau, z0 in (('B ring, 200 m up', 1.8, 200.0), ('B ring, 2 km up', 1.8, 2000.0),
                        ('Cassini, 100 m up', 0.08, 100.0), ('Cassini, 2 km up', 0.08, 2000.0)):
        t = slab_closed(tau, CORE_H, HALO_H, HALO_F, z0, -1.0, 0.6, 900.0)
        print('  %-20s tau_path %7.3f -> opacity %.3f   (the sheet alone is %.3f)'
              % (nm, t, 1.0 - math.exp(-t), 1.0 - math.exp(-tau)))

    print('\n%s' % ('ALL CHECKS PASSED' if fail == 0 else '%d CHECK(S) FAILED' % fail))
    return 1 if fail else 0


if __name__ == '__main__':
    raise SystemExit(main())
