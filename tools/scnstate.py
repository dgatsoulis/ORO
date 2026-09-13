#!/usr/bin/env python3
# ============================================================================
# scnstate.py - author an Orbiter scenario state by hand (ORO, 2026-09-12).
# ----------------------------------------------------------------------------
# Why this exists: a scenario's airborne vessel is written as RPOS / RVEL / AROT
# in the ECLIPTIC frame relative to the planet's centre, and there is no
# lat/lon/altitude form for anything that is not Landed. We cannot fly to a state
# and save it (he runs every test), so every new in-flight scenario has to be
# computed - and a wrong frame is the kind of error a flight cannot diagnose: the
# ship appears underground, or pointing backwards, and you cannot tell a bad
# scenario from a broken effect.
#
# So this reproduces Orbiter's own arithmetic, from the core, and then CHECKS
# itself against states Orbiter WROTE:
#   CelestialBody::Setup / UpdatePrecession / UpdateRotation  (Celbody.cpp)
#       -> Dphi, R_ecl, rotation(t)   the planet's orientation at an MJD
#   Body::LocalToEquatorial                                   (Body.cpp)
#       -> lng = atan2(z, x), lat = asin(y / r)
#   Vessel::SetGlobalOrientation / the AROT writer              (Vessel.cpp)
#       -> AROT is Euler angles of the vessel's global rotation matrix, whose
#          COLUMNS are the vessel's own axes (x right, y up, z nose).
#   FlightRecorder.cpp:135                                     (the surface velocity)
#       -> a surface-fixed point moves at (-w r cos(lat) sin(lng), 0, +w r cos(lat) cos(lng))
#
# LANDED vessels need none of this: "BASE <base>:<pad>" + HEADING is how 505 of the
# 800 stock landed blocks are written, and Orbiter places them itself. Use that.
#
# USAGE
#   python scnstate.py --check                      validate against stock scenarios
#   python scnstate.py Earth --lon -80.6 --lat 28.5 --alt 5000 --hdg 90 --spd 250
#       [--fpa 0] [--aoa 0] [--mjd 51982.5]         -> the three scenario lines
#   python scnstate.py Earth --read "x y z" --mjd M   -> lon/lat/alt of an RPOS
# ============================================================================
import math
import os
import re
import sys

D2R = math.pi / 180.0
R2D = 180.0 / math.pi
GGRAV = 6.67259e-11
MJD_J2000 = 51544.5          # Elements::Elements default epoch (Jepoch2MJD(2000))


def orbiter_root():
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


# ----------------------------------------------------------- linear algebra ---
def mul(M, v):
    return [sum(M[i][k] * v[k] for k in range(3)) for i in range(3)]


def matmul(A, B):
    return [[sum(A[i][k] * B[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def det3(M):
    return (M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1])
            - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0])
            + M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]))


def add(a, b):
    return [a[i] + b[i] for i in range(3)]


def scale(a, s):
    return [a[i] * s for i in range(3)]


def norm(a):
    return math.sqrt(sum(x * x for x in a))


def unit(a):
    n = norm(a)
    return [x / n for x in a]


# --------------------------------------------------------------- the planet ---
class Planet:
    """Orbiter's own rotation model for one body, read from Config\\<name>.cfg."""

    def __init__(self, name, mjd0):
        cfg = read_cfg(os.path.join(orbiter_root(), 'Config', name + '.cfg'))
        self.name = name
        self.cfg = cfg
        self.R = fget(cfg, 'Size')
        self.M = fget(cfg, 'Mass')
        self.GM = GGRAV * self.M
        self.rot_T = fget(cfg, 'SidRotPeriod', 86400.0)
        self.eps_rel = fget(cfg, 'Obliquity')
        self.Lrel0 = fget(cfg, 'LAN')
        self.mjd_rel = fget(cfg, 'LAN_MJD', MJD_J2000)
        self.prec_T = fget(cfg, 'PrecessionPeriod')
        self.eps_ref = fget(cfg, 'PrecessionObliquity')
        self.lan_ref = fget(cfg, 'PrecessionLAN')
        self.mjd0 = mjd0

        # Celbody.cpp:152 - the scenario's start MJD is merged into the rotation offset
        self.Dphi = (fget(cfg, 'SidRotOffset')
                     + (mjd0 - MJD_J2000) * (86400.0 / self.rot_T) * 2.0 * math.pi
                     + self.Lrel0 * math.cos(self.eps_rel))
        self.Dphi = math.fmod(self.Dphi, 2.0 * math.pi)
        self._precess(mjd0)

    def _precess(self, mjd):
        """CelestialBody::UpdatePrecession"""
        prec_omega = (2.0 * math.pi / self.prec_T) if self.prec_T else 0.0
        self.Lrel = self.Lrel0 + prec_omega * (mjd - self.mjd_rel)
        sinl, cosl = math.sin(self.Lrel), math.cos(self.Lrel)
        se, ce = math.sin(self.eps_rel), math.cos(self.eps_rel)
        Rrr = [[cosl, -sinl * se, -sinl * ce],
               [0.0, ce, -se],
               [sinl, cosl * se, cosl * ce]]
        if self.eps_ref:
            sr, cr = math.sin(self.eps_ref), math.cos(self.eps_ref)
            sl2, cl2 = math.sin(self.lan_ref), math.cos(self.lan_ref)
            R_ref = matmul([[cl2, 0.0, -sl2], [0.0, 1.0, 0.0], [sl2, 0.0, cl2]],
                           [[1.0, 0.0, 0.0], [0.0, cr, -sr], [0.0, sr, cr]])
            Rrr = matmul(R_ref, Rrr)
        self.axis = mul(Rrr, [0.0, 1.0, 0.0])
        self.eps_ecl = math.acos(max(-1.0, min(1.0, self.axis[1])))
        self.lan_ecl = math.atan2(-self.axis[0], self.axis[2])
        sL, cL = math.sin(self.lan_ecl), math.cos(self.lan_ecl)
        se, ce = math.sin(self.eps_ecl), math.cos(self.eps_ecl)
        self.R_ecl = [[cL, -sL * se, -sL * ce],
                      [0.0, ce, -se],
                      [sL, cL * se, cL * ce]]
        cos_poff = cL * Rrr[0][0] + sL * Rrr[2][0]
        sin_poff = -(cL * Rrr[0][2] + sL * Rrr[2][2])
        self.rotation_off = math.atan2(sin_poff, cos_poff)

    def rot(self, t=0.0):
        """CelestialBody::GetRotation - planet-fixed -> ecliptic, t seconds after load."""
        r = (self.Dphi + t * (2.0 * math.pi / self.rot_T)
             - self.Lrel * math.cos(self.eps_ecl) + self.rotation_off)
        cr, sr = math.cos(r), math.sin(r)
        return matmul(self.R_ecl, [[cr, 0.0, -sr], [0.0, 1.0, 0.0], [sr, 0.0, cr]])

    def omega(self):
        return 2.0 * math.pi / self.rot_T


# ------------------------------------------------------- geo <-> local frame ---
def loc_from_geo(lng, lat, rad):
    """inverse of Body::LocalToEquatorial"""
    cl, sl = math.cos(lat), math.sin(lat)
    return [rad * cl * math.cos(lng), rad * sl, rad * cl * math.sin(lng)]


def geo_from_loc(v):
    rad = norm(v)
    return math.atan2(v[2], v[0]), math.asin(v[1] / rad), rad


def horizon(lng, lat):
    """up / north / east at a surface point, in the planet's own frame."""
    cl, sl = math.cos(lat), math.sin(lat)
    cg, sg = math.cos(lng), math.sin(lng)
    up = [cl * cg, sl, cl * sg]
    north = [-sl * cg, cl, -sl * sg]
    east = [-sg, 0.0, cg]
    return up, north, east


# -------------------------------------------------------------- the state ---
def arot_from_axes(X, Y, Z):
    """AROT for a vessel whose own axes (x right, y up, z nose) are X, Y, Z in the
    ecliptic frame. The vessel's rotation matrix holds them as COLUMNS, and the AROT
    writer in Vessel.cpp:6161 is the inverse of SetGlobalOrientation."""
    Rv = [[X[0], Y[0], Z[0]],
          [X[1], Y[1], Z[1]],
          [X[2], Y[2], Z[2]]]
    d = det3(Rv)
    assert abs(d - 1.0) < 1e-6, 'axes are not a proper rotation (det %.6f)' % d
    return [math.atan2(Rv[1][2], Rv[2][2]) * R2D,
            -math.asin(max(-1.0, min(1.0, Rv[0][2]))) * R2D,
            math.atan2(Rv[0][1], Rv[0][0]) * R2D]


def axes_from_nose_up(nose, up_hint):
    """Orthonormal (X, Y, Z) for a vessel pointing 'nose' with 'up_hint' overhead."""
    Z = unit(nose)
    Y = unit([up_hint[i] - Z[i] * sum(up_hint[k] * Z[k] for k in range(3)) for i in range(3)])
    # left-handed frame (x right, y up, z nose): fix the sign by testing the determinant
    X = [Y[1] * Z[2] - Y[2] * Z[1], Y[2] * Z[0] - Y[0] * Z[2], Y[0] * Z[1] - Y[1] * Z[0]]
    if det3([[X[0], Y[0], Z[0]], [X[1], Y[1], Z[1]], [X[2], Y[2], Z[2]]]) < 0:
        X = [-x for x in X]
    return X, Y, Z


def sun_dir(mjd):
    """Unit vector from Earth toward the Sun in Orbiter's ecliptic frame.

    From mean elements - the planet modules own the real ephemeris, and a tenth of a
    degree is far more than enough to answer "is this point in darkness".
    """
    T = (mjd - MJD_J2000) / 36525.0
    L = 100.46435 + 35999.37285 * T          # Earth's mean longitude
    pi_ = 102.94719 + 1.7195269 * T          # longitude of perihelion
    e = 0.01671022
    M = (L - pi_) * D2R
    C = (2 * e - 0.25 * e ** 3) * math.sin(M) + 1.25 * e * e * math.sin(2 * M)
    lam = (L + C * R2D + 180.0) % 360.0      # Earth -> Sun is the opposite heading
    return [math.cos(lam * D2R), 0.0, math.sin(lam * D2R)]


def sun_elevation(pl, lng_d, lat_d, mjd, t=0.0):
    """Sun elevation in degrees at a surface point - negative is night."""
    up, _, _ = horizon(lng_d * D2R, lat_d * D2R)
    upg = mul(pl.rot(t), up)
    s = sun_dir(mjd)
    return math.asin(max(-1.0, min(1.0, sum(upg[i] * s[i] for i in range(3))))) * R2D


def make_state(pl, lng_d, lat_d, alt, hdg_d, spd, fpa_d=0.0, aoa_d=0.0, t=0.0,
               surface_motion=True):
    """-> (RPOS, RVEL, AROT) for a vessel flying over the planet.

    lng/lat/hdg/fpa/aoa in degrees, alt in metres above the mean radius, spd is
    the GROUND-relative speed (Orbiter has no wind, so that is the airspeed).
    Wings level by deliberate choice - a bank angle is one more convention to get
    wrong and nothing here needs one.
    """
    lng, lat = lng_d * D2R, lat_d * D2R
    hdg, pitch = hdg_d * D2R, (fpa_d + aoa_d) * D2R
    rad = pl.R + alt
    loc = loc_from_geo(lng, lat, rad)
    up, north, east = horizon(lng, lat)

    # velocity: ground-relative, plus the surface's own motion (FlightRecorder.cpp:135)
    fwd_h = add(scale(north, math.cos(hdg)), scale(east, math.sin(hdg)))
    vdir = add(scale(fwd_h, math.cos(fpa_d * D2R)), scale(up, math.sin(fpa_d * D2R)))
    # surface_motion False = the speed given IS the inertial speed: that is what an
    # ORBITAL state wants, where "ground relative plus rotation" would be wrong by the
    # 465 m/s the ground is already doing.
    if surface_motion:
        vref = pl.omega() * rad * math.cos(lat)
        v_surf = [-vref * math.sin(lng), 0.0, vref * math.cos(lng)]
        v_loc = add(scale(vdir, spd), v_surf)
    else:
        v_loc = scale(vdir, spd)

    # attitude: nose pitched out of the horizontal plane, wings level
    zL = add(scale(fwd_h, math.cos(pitch)), scale(up, math.sin(pitch)))
    yL = add(scale(fwd_h, -math.sin(pitch)), scale(up, math.cos(pitch)))
    xL = add(scale(north, -math.sin(hdg)), scale(east, math.cos(hdg)))

    Rp = pl.rot(t)
    X, Y, Z = mul(Rp, xL), mul(Rp, yL), mul(Rp, zL)
    # the vessel's rotation matrix has its own axes as COLUMNS
    Rv = [[X[0], Y[0], Z[0]],
          [X[1], Y[1], Z[1]],
          [X[2], Y[2], Z[2]]]
    d = det3(Rv)
    assert abs(d - 1.0) < 1e-9, 'attitude frame is not a proper rotation (det %.6f)' % d

    arot = [math.atan2(Rv[1][2], Rv[2][2]) * R2D,
            -math.asin(max(-1.0, min(1.0, Rv[0][2]))) * R2D,
            math.atan2(Rv[0][1], Rv[0][0]) * R2D]
    return mul(Rp, loc), mul(Rp, v_loc), arot, Z


def read_state(pl, rpos, t=0.0):
    """-> lng, lat, alt for an RPOS, so a state can be checked or reverse engineered."""
    Rp = pl.rot(t)
    inv = [[Rp[j][i] for j in range(3)] for i in range(3)]   # rotation: inverse = transpose
    lng, lat, rad = geo_from_loc(mul(inv, rpos))
    return lng * R2D, lat * R2D, rad - pl.R


def fmt(v, p=3):
    return ' '.join(('%.*f' % (p, x)) for x in v)


# ------------------------------------------------------------------ checks ---
def check():
    """Validate the frame against states ORBITER wrote, not against my own arithmetic."""
    ok = True

    def say(name, got, want, tol, unit_=''):
        nonlocal ok
        good = abs(got - want) <= tol
        ok = ok and good
        print('  %-42s %12.4f  (expected %s%.4f%s)  %s'
              % (name, got, '~', want, unit_, 'ok' if good else '*** FAIL ***'))

    # 1. "Cruising above Florida" (stock, 2024 Edition): a DG-S airborne over Florida.
    print('stock "Cruising above Florida" GL-01S:')
    pl = Planet('Earth', 54723.6753959915)
    lon, lat, alt = read_state(pl, [-5059189.96, 1893400.13, 3405379.21])
    print('    RPOS reads back as  lon %.3f  lat %.3f  alt %.0f m' % (lon, lat, alt))
    say('longitude over Florida', lon, -81.0, 3.0, ' deg')
    say('latitude over Florida', lat, 28.5, 3.0, ' deg')
    say('altitude (a cruising DG-S)', alt / 1000.0, 14.7, 3.0, ' km')

    # 2. the same scenario's ISS - a check that the frame holds far from the ground
    lon, lat, alt = read_state(pl, [-3821532.83, -5105579.24, 2181334.85])
    say('ISS altitude', alt / 1000.0, 350.0, 120.0, ' km')

    # 3. round trip: geo -> RPOS -> geo, at a point and date we will actually use
    print('round trip (KSC, an ORO scenario date):')
    pl2 = Planet('Earth', 51982.6780413247)
    rp, rv, ar, nose = make_state(pl2, -80.682, 28.596, 5000.0, 90.0, 250.0)
    lon, lat, alt = read_state(pl2, rp)
    say('longitude', lon, -80.682, 1e-6, ' deg')
    say('latitude', lat, 28.596, 1e-6, ' deg')
    say('altitude', alt, 5000.0, 1e-3, ' m')

    # 4. the attitude really does point where it was asked to: nose . velocity = 1
    #    at zero angle of attack, once the surface's own motion is removed.
    Rp = pl2.rot()
    inv = [[Rp[j][i] for j in range(3)] for i in range(3)]
    v_loc = mul(inv, rv)
    lng, lat_r = -80.682 * D2R, 28.596 * D2R
    up, north, east = horizon(lng, lat_r)
    vref = pl2.omega() * (pl2.R + 5000.0) * math.cos(lat_r)
    v_gnd = [v_loc[0] + vref * math.sin(lng), v_loc[1], v_loc[2] - vref * math.cos(lng)]
    say('ground speed', norm(v_gnd), 250.0, 1e-6, ' m/s')
    say('nose . ground velocity', sum(unit(mul(inv, nose))[i] * unit(v_gnd)[i] for i in range(3)),
        1.0, 1e-9)
    say('heading (east)', math.atan2(sum(unit(v_gnd)[i] * east[i] for i in range(3)),
                                     sum(unit(v_gnd)[i] * north[i] for i in range(3))) * R2D,
        90.0, 1e-6, ' deg')

    # 5. AROT -> matrix -> AROT, through Orbiter's own SetGlobalOrientation
    x, y, z = (a * D2R for a in ar)
    sx, cx, sy, cy, sz, cz = (math.sin(x), math.cos(x), math.sin(y),
                              math.cos(y), math.sin(z), math.cos(z))
    Rb = [[cy * cz, cy * sz, -sy],
          [sx * sy * cz - cx * sz, sx * sy * sz + cx * cz, sx * cy],
          [cx * sy * cz + sx * sz, cx * sy * sz - sx * cz, cx * cy]]
    back = [math.atan2(Rb[1][2], Rb[2][2]) * R2D,
            -math.asin(max(-1.0, min(1.0, Rb[0][2]))) * R2D,
            math.atan2(Rb[0][1], Rb[0][0]) * R2D]
    say('AROT round trip (worst axis)', max(abs(back[i] - ar[i]) for i in range(3)), 0.0,
        1e-9, ' deg')

    print('\n%s' % ('ALL CHECKS PASSED' if ok else '*** SOME CHECKS FAILED ***'))
    return 0 if ok else 1


# -------------------------------------------------------------------- main ---
def main(argv):
    if '--check' in argv:
        return check()
    body = argv[1] if len(argv) > 1 and not argv[1].startswith('--') else 'Earth'
    o = {'--lon': 0.0, '--lat': 0.0, '--alt': 0.0, '--hdg': 0.0, '--spd': 0.0,
         '--fpa': 0.0, '--aoa': 0.0, '--mjd': 51982.0}
    rd = None
    for k in range(1, len(argv) - 1):
        if argv[k] in o:
            o[argv[k]] = float(argv[k + 1])
        elif argv[k] == '--read':
            rd = [float(x) for x in argv[k + 1].split()]
    pl = Planet(body, o['--mjd'])
    if rd:
        lon, lat, alt = read_state(pl, rd)
        print('lon %.6f  lat %.6f  alt %.1f m' % (lon, lat, alt))
        return 0
    rp, rv, ar, _ = make_state(pl, o['--lon'], o['--lat'], o['--alt'],
                               o['--hdg'], o['--spd'], o['--fpa'], o['--aoa'])
    print('  STATUS Orbiting %s' % body)
    print('  RPOS %s' % fmt(rp, 3))
    print('  RVEL %s' % fmt(rv, 4))
    print('  AROT %s' % fmt(ar, 3))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
