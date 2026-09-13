#!/usr/bin/env python3
# ============================================================================
# ringplane.py - orbital elements for an orbit lying IN a planet's ring plane,
# and the physics of the near field there (ORO rings, round 2, 2026-09-12).
# ----------------------------------------------------------------------------
# Why this exists: a scenario's ELEMENTS line is referenced to the ECLIPTIC, a ring
# plane is the planet's EQUATOR, and Orbiter's frame is left-handed - so the node of
# an in-plane orbit is lan_ecl + 180 degrees, not lan_ecl. Guessing the other one puts
# a DeltaGlider 99,441 km out of Saturn's ring plane, which is exactly the kind of
# error a flight cannot diagnose: you load the scenario, see nothing, and have no way
# to tell a bad scenario from a broken effect.
#
# So this reproduces Orbiter's own two pieces of arithmetic and then VERIFIES the
# answer by sweeping the whole orbit:
#   CelestialBody::UpdatePrecession  (Src/Orbiter/Celbody.cpp) -> the spin axis in the
#                                     ecliptic frame, which IS the ring plane's normal
#   Elements::Update                 (Src/Orbiter/Element.cpp) -> the position a
#                                     scenario's ELEMENTS line actually produces
#
# !! AND A SECOND TRAP, found on the second flight (2026-09-12): a scenario's ELEMENTS
# line is turned into a state vector by PURE KEPLER (Element.cpp) and the ship is then
# integrated in the planet's J2 field when the Launchpad's "Nonspherical gravity sources"
# is on (Psys.cpp). So "e = 0" is NOT a circular orbit at Saturn (J2 = 0.01645): the ship
# starts 52 m/s too slow for its radius, sits at the apoapsis of an e = 0.0058 orbit, and
# dives 700 km per orbit. The elements that come out circular under J2 are a Kepler
# PERIAPSIS at r with the J2-circular speed - by vis-viva EXACTLY a = r / (1 - q), e = q,
# q = 1.5 J2 (R/r)^2 (the first-order r (1 + q) is 4 km off at Saturn, and it was the
# first thing written here). Both sets are printed; use the J2 set unless the sim has
# nonspherical gravity switched off.
#
# USAGE
#   python ringplane.py Saturn                      the whole report
#   python ringplane.py Saturn --r 119800           at a chosen radius in km
#   python ringplane.py Saturn --r 119800 --vz 5    and a plane crossing at 5 m/s
#   python ringplane.py Uranus --mjd 57897.25
# Reads Config\<Planet>.cfg from the Orbiter tree above this file.
# ============================================================================
import math
import os
import re
import sys

D2R = math.pi / 180.0
R2D = 180.0 / math.pi
GGRAV = 6.67259e-11          # Orbiter's constant (Orbiter.h)


# ---------------------------------------------------------------- the cfg ---
def orbiter_root():
    # tools/ -> ORO/ -> samples/ -> Orbitersdk/ -> <Orbiter>
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.abspath(os.path.join(here, '..', '..', '..', '..'))


def read_cfg(path):
    vals = {}
    with open(path, 'r', errors='replace') as f:
        for line in f:
            line = line.split(';')[0]
            m = re.match(r'\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)', line)
            if m:
                vals[m.group(1)] = m.group(2).strip()
    return vals


def fget(cfg, key, default=0.0):
    try:
        return float(cfg[key].split()[0])
    except (KeyError, ValueError, IndexError):
        return default


# ------------------------------------------------------- Orbiter's matrices ---
def matmul(A, B):
    return [[sum(A[i][k] * B[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def matvec(A, v):
    return [sum(A[i][k] * v[k] for k in range(3)) for i in range(3)]


def spin_axis(cfg, mjd):
    """CelestialBody::UpdatePrecession - the rotation axis in Orbiter's ecliptic frame."""
    eps_ref = fget(cfg, 'PrecessionObliquity')
    lan_ref = fget(cfg, 'PrecessionLAN')
    Lrel0 = fget(cfg, 'LAN')
    mjd_rel = fget(cfg, 'LAN_MJD', 51544.5)
    eps_rel = fget(cfg, 'Obliquity')
    prec_T = fget(cfg, 'PrecessionPeriod')

    prec_omega = (2.0 * math.pi / prec_T) if prec_T else 0.0
    Lrel = Lrel0 + prec_omega * (mjd - mjd_rel)
    sinl, cosl = math.sin(Lrel), math.cos(Lrel)
    se, ce = math.sin(eps_rel), math.cos(eps_rel)
    R = [[cosl, -sinl * se, -sinl * ce],
         [0.0, ce, -se],
         [sinl, cosl * se, cosl * ce]]
    if eps_ref:
        sr, cr = math.sin(eps_ref), math.cos(eps_ref)
        sl2, cl2 = math.sin(lan_ref), math.cos(lan_ref)
        R_ref = matmul([[cl2, 0.0, -sl2], [0.0, 1.0, 0.0], [sl2, 0.0, cl2]],
                       [[1.0, 0.0, 0.0], [0.0, cr, -sr], [0.0, sr, cr]])
        R = matmul(R_ref, R)
    return matvec(R, [0.0, 1.0, 0.0])


def orbit_pos(i, theta, u, r=1.0):
    """Elements::Update's position, for argument of latitude u (= true anomaly + omega)."""
    sini, cosi = math.sin(i), math.cos(i)
    sint, cost = math.sin(theta), math.cos(theta)
    sinu, cosu = math.sin(u), math.cos(u)
    return [r * (cost * cosu - sint * sinu * cosi),
            r * sinu * sini,
            r * (sint * cosu + cost * sinu * cosi)]


# ------------------------------------------------------------------- main ---
def main(argv):
    body = argv[1] if len(argv) > 1 else 'Saturn'
    mjd = 57897.25
    rkm = None
    vz = None
    di_deg = None
    t0 = None
    above = False
    if '--above' in argv:
        above = True
    for k in range(2, len(argv) - 1):
        if argv[k] == '--mjd':
            mjd = float(argv[k + 1])
        elif argv[k] == '--r':
            rkm = float(argv[k + 1])
        elif argv[k] == '--vz':
            vz = float(argv[k + 1])
        elif argv[k] == '--di':
            di_deg = float(argv[k + 1])
        elif argv[k] == '--t0':
            t0 = float(argv[k + 1])

    path = os.path.join(orbiter_root(), 'Config', body + '.cfg')
    if not os.path.isfile(path):
        print('no such planet config: %s' % path)
        return 1
    cfg = read_cfg(path)
    Rpl = fget(cfg, 'Size')
    M = fget(cfg, 'Mass')
    J2 = 0.0
    if 'JCoeff' in cfg:
        try:
            J2 = float(cfg['JCoeff'].split()[0])
        except ValueError:
            J2 = 0.0
    irad = fget(cfg, 'RingMinRadius')
    orad = fget(cfg, 'RingMaxRadius')
    GM = GGRAV * M

    ax = spin_axis(cfg, mjd)
    eps_ecl = math.acos(max(-1.0, min(1.0, ax[1])))
    lan_ecl = math.atan2(-ax[0], ax[2]) % (2.0 * math.pi)

    print('%s   MJD %.5f' % (body, mjd))
    print('  radius        %.4e m   GM %.6e' % (Rpl, GM))
    if orad > irad > 0:
        print('  rings         %.3f .. %.3f Rp  =  %.0f .. %.0f km'
              % (irad, orad, irad * Rpl / 1e3, orad * Rpl / 1e3))
    else:
        print('  rings         none declared in the config')
    print('  spin axis     %.9f %.9f %.9f' % tuple(ax))
    print('  eps_ecl       %.9f rad = %.6f deg' % (eps_ecl, eps_ecl * R2D))
    print('  lan_ecl       %.9f rad = %.6f deg' % (lan_ecl, lan_ecl * R2D))

    # an orbit whose angular momentum lies along the spin axis
    i_deg = eps_ecl * R2D
    th_deg = ((lan_ecl + math.pi) % (2.0 * math.pi)) * R2D
    print('\nIN-PLANE ELEMENTS:  i = %.6f   LAN(theta) = %.6f' % (i_deg, th_deg))

    def worst_out_of_plane(i_d, th_d):
        w = 0.0
        for k in range(361):
            p = orbit_pos(i_d * D2R, th_d * D2R, k * D2R)
            w = max(w, abs(sum(p[j] * ax[j] for j in range(3))))
        return w

    w_ok = worst_out_of_plane(i_deg, th_deg)
    w_bad = worst_out_of_plane(i_deg, lan_ecl * R2D)
    ref = (rkm * 1e3) if rkm else (0.5 * (irad + orad) * Rpl if orad > irad else Rpl)
    print('  VERIFIED: worst out-of-plane over a full orbit = %.3e of a radius (%.4f m at %.0f km)'
          % (w_ok, w_ok * ref, ref / 1e3))
    print('  (LAN = lan_ecl instead would be %.4f of a radius = %.0f km - the trap)'
          % (w_bad, w_bad * ref / 1e3))

    if rkm:
        a = rkm * 1e3
        n = math.sqrt(GM / a ** 3)
        # Psys.cpp at latitude 0: omega^2 = n^2 (1 + 1.5 J2 (R/r)^2), with its 1e-10 cutoff
        j2t = J2 * (Rpl / a) ** 2
        q = 1.5 * j2t if abs(j2t) > 1e-10 else 0.0
        om = n * math.sqrt(1.0 + q)
        print('\nAT r = %.0f km (%.3f Rp):' % (rkm, a / Rpl))
        print('  period      %.0f s = %.2f h      speed %.1f m/s' % (2 * math.pi / n, 2 * math.pi / n / 3600.0, n * a))
        print('  Keplerian shear -(3/2) n dx:  %.4f m/s at 100 m,  %.4f m/s at 1 km'
              % (1.5 * n * 100, 1.5 * n * 1000))
        if q:
            # exact: v^2 = GM (1+q)/r at radial velocity 0  =>  1/a = (1-q)/r, e = 1 - r/a = q
            a_j2 = a / (1.0 - q)
            e_j2 = q
            print('  J2 = %.5f:  circular speed under J2 is %.2f m/s, %.2f m/s more than Kepler (%.3f%%)'
                  % (J2, om * a, (om - n) * a, (om / n - 1.0) * 100.0))
            print('  !! a plain "e = 0" ELEMENTS line dives ~%.0f km per orbit here. USE THESE INSTEAD:'
                  % (2.0 * a * e_j2 / 1e3))
            print('     circular under J2:  a = %.1f   e = %.9f   with L = omegab (start at periapsis)'
                  % (a_j2, e_j2))
        else:
            print('  no J2 in the cfg: a plain e = 0 line is circular here')
        if vz or di_deg:
            # the inclination can be given either way round: as a crossing SPEED, or
            # (what a scenario brief actually says) as an angle to the equator.
            if di_deg:
                di = di_deg * D2R
                vz = a * n * di
            else:
                di = vz / (a * n)
            exc = a * di
            print('\n  mutual inclination %.7f deg  ->  plane crossing at %.2f m/s'
                  % (di * R2D, vz))
            print('  max excursion %.0f m  (%.0f km)' % (exc, exc / 1e3))
            if t0 is not None:
                # start t0 seconds BEFORE a node. Which node is a LOOK decision, not a
                # detail: at a solstice the sun is on one side of the ring plane, so
                # starting ABOVE approaches the LIT face (detail, texture, relief) with
                # the globe's near side in darkness, and starting BELOW approaches the
                # UNLIT face (the backlit inversion) with the globe lit. The mutual
                # inclination is 0.1 deg, so the node line is a free choice either way.
                us = [(math.pi - n * t0) if above else (-n * t0)]
            else:
                us = [math.asin(max(-1.0, min(1.0, z / exc)))
                      for z in (-600.0, -150.0, 0.0, 150.0, 600.0)]
            # !! ITERATE OVER u0, NEVER OVER z0. asin comes back in [-90, +90], so
            # recovering the argument of latitude from the height collapses u0 = 179.4
            # (above the plane and FALLING toward the descending node) into 0.58
            # (above it and RISING away) - the same height, the opposite flight, and
            # nothing downstream can tell. It printed exactly that for one run.
            for u0 in us:
                z0 = exc * math.sin(u0)
                # periapsis AT the start point (omegab = theta + u0, L = omegab) so the
                # J2-corrected (a, e) puts the ship at r with the circular speed there
                om_b = (th_deg + u0 * R2D) % 360.0
                print('    starting %+8.1f m %s the plane (%.1f s before the node):'
                      % (z0, 'ABOVE' if z0 > 0 else 'below', abs(t0 if t0 is not None else u0 / n)))
                print('      ELEMENTS %.1f %.9f %.9f %.6f %.6f %.6f %.5f'
                      % (a_j2 if q else a, e_j2 if q else 0.0, i_deg + di * R2D,
                         th_deg, om_b, om_b, mjd))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
