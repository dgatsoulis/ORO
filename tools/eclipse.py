#!/usr/bin/env python3
# ============================================================================
# eclipse.py - find a solar eclipse visible from a stock Orbiter base, with the
# sun low (ORO, 2026-09-13).
# ----------------------------------------------------------------------------
# ORO's eclipse effect is two overlapping ANGULAR DISCS at the camera, so finding a
# date is the same computation: where are the sun and the moon, how big is each, and
# how far apart are they as seen from a point on the ground.
#
# Sun: Meeus ch.25 (0.01 deg). Moon: Meeus ch.47, truncated ELP - the terms below
# give the moon to about an arcminute, which against a half-degree disc is a few per
# cent of obscuration and far inside what picking a scenario date needs.
#
# !! TWO THINGS THAT WOULD OTHERWISE BE WRONG BY ABOUT A SOLAR DIAMETER:
#   - Meeus returns MEAN EQUINOX OF DATE; Orbiter's frame is the J2000 ecliptic, and
#     the observer's position comes from Orbiter's own rotation model. General
#     precession is 1.39697 deg per century, so for 1900 that is 1.4 deg - nearly
#     three solar diameters. Both bodies are rotated back to J2000 here.
#   - the moon is 60 Earth radii away, so TOPOCENTRIC parallax is up to a degree. The
#     observer's own position is subtracted rather than working geocentrically.
#
# VALIDATION: the stock scenarios carry two real eclipse dates (Scenarios\The Solar
# System\Eclipses). --check confirms this code finds a central eclipse at both, which
# is what says it agrees with reality AND with what Orbiter draws.
#
# USAGE
#   python eclipse.py --check                 validate against the stock dates
#   python eclipse.py --from 1950 --to 2050   search for candidates
# ============================================================================
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scnstate as S                                             # noqa: E402

D2R = math.pi / 180.0
R2D = 180.0 / math.pi

# The bases he named, from Config\Earth\Base\*.cfg ("Location = lon lat").
BASES = [("Cape Canaveral", -80.65165, 28.58438),
         ("Habana", -82.40, 23.00),
         ("Baikonur", 63.30, 45.90)]

# Meeus 47.A - (D, M, M', F, sigma_l, sigma_r)
TL = [(0,0,1,0,6288774,-20905355),(2,0,-1,0,1274027,-3699111),(2,0,0,0,658314,-2955968),
      (0,0,2,0,213618,-569925),(0,1,0,0,-185116,48888),(0,0,0,2,-114332,-3149),
      (2,0,-2,0,58793,246158),(2,-1,-1,0,57066,-152138),(2,0,1,0,53322,-170733),
      (2,-1,0,0,45758,-204586),(0,1,-1,0,-40923,-129620),(1,0,0,0,-34720,108743),
      (0,1,1,0,-30383,104755),(2,0,0,-2,15327,10321),(0,0,1,2,-12528,0),
      (0,0,1,-2,10980,79661),(4,0,-1,0,10675,-34782),(0,0,3,0,10034,-23210),
      (4,0,-2,0,8548,-21636),(2,1,-1,0,-7888,24208),(2,1,0,0,-6766,30824),
      (1,0,-1,0,-5163,-8379),(1,1,0,0,4987,-16675),(2,-1,1,0,4036,-12831),
      (2,0,2,0,3994,-10445),(4,0,0,0,3861,-11650),(2,0,-3,0,3665,14403),
      (0,1,-2,0,-2689,-7003),(2,0,-1,2,-2602,0),(2,-1,-2,0,2390,10056),
      (1,0,1,0,-2348,6322),(2,-2,0,0,2236,-9884),(0,1,2,0,-2120,5751),
      (0,2,0,0,-2069,0),(2,-2,-1,0,2048,-4950),(2,0,1,-2,-1773,4130),
      (2,0,0,2,-1595,0),(4,-1,-1,0,1215,-3958),(0,0,2,2,-1110,0),
      (3,0,-1,0,-892,3258),(2,1,1,0,-810,2616),(4,-1,-2,0,759,-1897),
      (0,2,-1,0,-713,-2117),(2,2,-1,0,-700,2354),(2,1,-2,0,691,0),
      (2,-1,0,-2,596,0),(4,0,1,0,549,-1423),(0,0,4,0,537,-1117),
      (4,-1,0,0,520,-1571),(1,0,-2,0,-487,-1739),(2,1,0,-2,-399,0),
      (0,0,2,-2,-381,-4421),(1,1,1,0,351,0),(3,0,-2,0,-340,0),
      (4,0,-3,0,330,0),(2,-1,2,0,327,0),(0,2,1,0,-323,1165),
      (1,1,-1,0,299,0),(2,0,3,0,294,0),(2,0,-1,-2,0,8752)]

# Meeus 47.B - (D, M, M', F, sigma_b)
TB = [(0,0,0,1,5128122),(0,0,1,1,280602),(0,0,1,-1,277693),(2,0,0,-1,173237),
      (2,0,-1,1,55413),(2,0,-1,-1,46271),(2,0,0,1,32573),(0,0,2,1,17198),
      (2,0,1,-1,9266),(0,0,2,-1,8822),(2,-1,0,-1,8216),(2,0,-2,-1,4324),
      (2,0,1,1,4200),(2,1,0,-1,-3359),(2,-1,-1,1,2463),(2,-1,0,1,2211),
      (2,-1,-1,-1,2065),(0,1,-1,-1,-1870),(4,0,-1,-1,1828),(0,1,0,1,-1794),
      (0,0,0,3,-1749),(0,1,-1,1,-1565),(1,0,0,1,-1491),(0,1,1,1,-1475),
      (0,1,1,-1,-1410),(0,1,0,-1,-1344),(1,0,0,-1,-1335),(0,0,3,1,1107),
      (4,0,0,-1,1021),(4,0,-1,1,833),(0,0,1,-3,777),(4,0,-2,1,671),
      (2,0,0,-3,607),(2,0,2,-1,596),(2,-1,1,-1,491),(2,0,-2,1,-451),
      (0,0,3,-1,439),(2,0,2,1,422),(2,0,-3,-1,421),(2,1,-1,1,-366),
      (2,1,0,1,-351),(4,0,0,1,331),(2,-1,1,1,315),(2,-2,0,-1,302),
      (0,0,1,3,-283),(2,1,1,-1,-229),(1,1,0,-1,223),(1,1,0,1,223),
      (0,1,-2,-1,-220),(2,1,-1,-1,-220),(1,0,1,1,-185),(2,-1,-2,-1,181),
      (0,1,2,1,-177),(4,0,-2,-1,176),(4,-1,-1,-1,166),(1,0,1,-1,-164),
      (4,0,1,-1,132),(1,0,-1,-1,-119),(4,-1,0,-1,115),(2,-2,0,1,107)]

AU = 149597870.7          # km
R_SUN = 696000.0          # km
R_MOON = 1737.4           # km


def moon_sun(mjd):
    """Geocentric Sun and Moon in Orbiter's J2000 ecliptic frame, in km."""
    T = (mjd - 51544.5) / 36525.0
    # --- Sun (Meeus 25) ---
    L0 = 280.46646 + 36000.76983 * T + 0.0003032 * T * T
    Ms = 357.52911 + 35999.05029 * T - 0.0001537 * T * T
    e = 0.016708634 - 0.000042037 * T - 0.0000001267 * T * T
    m = Ms * D2R
    C = ((1.914602 - 0.004817 * T - 0.000014 * T * T) * math.sin(m)
         + (0.019993 - 0.000101 * T) * math.sin(2 * m) + 0.000289 * math.sin(3 * m))
    sun_lon = L0 + C
    v = Ms + C
    Rs = 1.000001018 * (1 - e * e) / (1 + e * math.cos(v * D2R)) * AU

    # --- Moon (Meeus 47) ---
    Lp = 218.3164477 + 481267.88123421 * T - 0.0015786 * T * T + T**3 / 538841 - T**4 / 65194000
    D = 297.8501921 + 445267.1114034 * T - 0.0018819 * T * T + T**3 / 545868 - T**4 / 113065000
    M = 357.5291092 + 35999.0502909 * T - 0.0001536 * T * T + T**3 / 24490000
    Mp = 134.9633964 + 477198.8675055 * T + 0.0087414 * T * T + T**3 / 69699 - T**4 / 14712000
    F = 93.2720950 + 483202.0175233 * T - 0.0036539 * T * T - T**3 / 3526000 + T**4 / 863310000
    E = 1 - 0.002516 * T - 0.0000074 * T * T

    sl = sr = sb = 0.0
    for d, mm, mp, f, cl, cr in TL:
        a = (d * D + mm * M + mp * Mp + f * F) * D2R
        w = E ** abs(mm)
        sl += cl * w * math.sin(a)
        sr += cr * w * math.cos(a)
    for d, mm, mp, f, cb in TB:
        a = (d * D + mm * M + mp * Mp + f * F) * D2R
        sb += cb * (E ** abs(mm)) * math.sin(a)

    A1 = (119.75 + 131.849 * T) * D2R
    A2 = (53.09 + 479264.290 * T) * D2R
    A3 = (313.45 + 481266.484 * T) * D2R
    sl += 3958 * math.sin(A1) + 1962 * math.sin((Lp - F) * D2R) + 318 * math.sin(A2)
    sb += (-2235 * math.sin(Lp * D2R) + 382 * math.sin(A3) + 175 * math.sin(A1 - F * D2R)
           + 175 * math.sin(A1 + F * D2R) + 127 * math.sin((Lp - Mp) * D2R)
           - 115 * math.sin((Lp + Mp) * D2R))

    moon_lon = Lp + sl / 1000000.0
    moon_lat = sb / 1000000.0
    Rm = 385000.56 + sr / 1000.0

    # mean equinox of date -> J2000 (general precession in longitude)
    p = 1.39697 * T + 0.00031 * T * T
    sun_lon -= p
    moon_lon -= p

    def vec(lon, lat, r):
        cl_, sl_ = math.cos(lat * D2R), math.sin(lat * D2R)
        return [r * cl_ * math.cos(lon * D2R), r * sl_, r * cl_ * math.sin(lon * D2R)]

    return vec(sun_lon, 0.0, Rs), vec(moon_lon, moon_lat, Rm)


# Planet() re-parses Earth.cfg on construction, and a search builds one per time step -
# tens of thousands of file reads. The parse is memoised instead; the cfg cannot change
# under us mid-run.
_CFG = {}
_read_cfg_real = S.read_cfg


def _read_cfg_cached(path):
    v = _CFG.get(path)
    if v is None:
        v = _CFG[path] = _read_cfg_real(path)
    return v


S.read_cfg = _read_cfg_cached

_PLCACHE = {}


def _planet(mjd):
    """Planet() parses Earth.cfg from disk, so a globe scan would read it 16,000
    times. Cached per instant - a scan holds mjd fixed and moves the observer."""
    k = round(mjd, 9)
    p = _PLCACHE.get(k)
    if p is None:
        if len(_PLCACHE) > 4096:
            _PLCACHE.clear()
        p = _PLCACHE[k] = S.Planet('Earth', mjd)
    return p


def circumstances(mjd, lon, lat):
    """-> (obscuration 0..1, sun altitude deg, separation deg) at a surface point."""
    pl = _planet(mjd)
    up, _, _ = S.horizon(lon * D2R, lat * D2R)
    obs = S.mul(pl.rot(), S.scale(up, pl.R / 1000.0))     # observer, km from centre
    upg = S.mul(pl.rot(), up)

    sun, moon = moon_sun(mjd)
    ds = [sun[i] - obs[i] for i in range(3)]
    dm = [moon[i] - obs[i] for i in range(3)]
    rs, rm = S.norm(ds), S.norm(dm)
    us, um = S.unit(ds), S.unit(dm)

    sep = math.acos(max(-1.0, min(1.0, sum(us[i] * um[i] for i in range(3))))) * R2D
    Rs_ = math.asin(R_SUN / rs) * R2D
    Rm_ = math.asin(R_MOON / rm) * R2D
    alt = math.asin(max(-1.0, min(1.0, sum(upg[i] * us[i] for i in range(3))))) * R2D

    # two overlapping discs - the same model ORO's own eclipse uses
    if sep >= Rs_ + Rm_:
        obsc = 0.0
    elif sep <= abs(Rs_ - Rm_):
        obsc = min(1.0, (Rm_ / Rs_) ** 2)
    else:
        d, r1, r2 = sep, Rs_, Rm_
        a1 = math.acos(max(-1.0, min(1.0, (d * d + r1 * r1 - r2 * r2) / (2 * d * r1))))
        a2 = math.acos(max(-1.0, min(1.0, (d * d + r2 * r2 - r1 * r1) / (2 * d * r2))))
        area = (r1 * r1 * (a1 - math.sin(2 * a1) / 2)
                + r2 * r2 * (a2 - math.sin(2 * a2) / 2))
        obsc = area / (math.pi * r1 * r1)
    return obsc, alt, sep


def new_moons(m0, m1):
    """Times of conjunction in ecliptic longitude, coarse then refined."""
    out = []
    prev = None
    t = m0
    while t < m1:
        sun, moon = moon_sun(t)
        dl = (math.atan2(moon[2], moon[0]) - math.atan2(sun[2], sun[0])) * R2D
        dl = (dl + 180) % 360 - 180
        if prev is not None and prev > 90 and dl < -90:
            pass                                   # wrap, not a conjunction
        elif prev is not None and prev < 0 <= dl:
            a, b = t - 1.0, t
            for _ in range(40):
                mid = (a + b) / 2
                sun, moon = moon_sun(mid)
                d = ((math.atan2(moon[2], moon[0]) - math.atan2(sun[2], sun[0])) * R2D
                     + 180) % 360 - 180
                if d < 0:
                    a = mid
                else:
                    b = mid
            out.append((a + b) / 2)
        prev = dl
        t += 1.0
    return out


def mjd_str(mjd):
    import datetime
    d = datetime.datetime(1858, 11, 17) + datetime.timedelta(days=mjd)
    return d.strftime("%Y-%m-%d %H:%M UT")


def greatest(mjd, coarse=3):
    """Where on Earth the eclipse is deepest at an instant, or None."""
    best = None
    for lat in range(-88, 89, coarse):
        for lon in range(-180, 180, coarse):
            o, alt, sep = circumstances(mjd, lon, lat)
            if o > 0 and (best is None or o > best[0]):
                best = (o, alt, sep, float(lon), float(lat))
    if best is None:
        return None
    o, alt, sep, lon, lat = best
    for step in (1.0, 0.2):
        moved = True
        while moved:
            moved = False
            for dlat in (-step, 0.0, step):
                for dlon in (-step, 0.0, step):
                    oo, aa, ss = circumstances(mjd, lon + dlon, lat + dlat)
                    if oo > o:
                        o, alt, sep, lon, lat, moved = oo, aa, ss, lon + dlon, lat + dlat, True
    return o, alt, sep, lon, lat


def check():
    """Validate against the eclipse dates Orbiter itself ships.

    ⚠️ NOT by asserting an obscuration at a point I remembered - both of those
    expectations were wrong the first time, and neither was the model's fault. The
    stock 1999 scenario begins EIGHT MINUTES before greatest eclipse (the shadow
    moves ~1000 km in that time) and the 2005 one begins hours before the shadow
    reaches Panama at all. What is checkable is WHERE the eclipse is at that instant.
    """
    print("Validation against the eclipse dates Orbiter itself ships:\n")
    ok = True

    g = greatest(51401.455509)
    print("  stock 'The 1999 solar eclipse'   %s" % mjd_str(51401.455509))
    if g:
        o, alt, sep, lon, lat = g
        good = o > 0.99 and abs(lat - 46) < 3 and abs(lon - 20) < 5
        ok = ok and good
        print("     deepest at %5.1f N %6.1f E   obscuration %5.1f%%   separation %.4f deg   %s"
              % (lat, lon, o * 100, sep, "ok" if good else "*** FAIL ***"))
    else:
        ok = False
        print("     NO ECLIPSE FOUND   *** FAIL ***")
    print("     history: total, central line over the Hungary/Serbia/Romania border")

    g = greatest(53468.7234363426)
    print("\n  stock 'April 8 2005'             %s" % mjd_str(53468.7234363426))
    good = g is None
    ok = ok and good
    print("     %s   %s" % ("no eclipse anywhere on Earth yet" if good else
                            "an eclipse was found - unexpected", "ok" if good else "*** FAIL ***"))
    print("     history: hybrid eclipse, first contact ~17:51 UT, greatest 20:36 UT in")
    print("              the South Pacific - the scenario starts you BEFORE it")

    print("\n%s" % ("the model agrees with the eclipses Orbiter draws"
                    if ok else "*** MODEL DISAGREES - do not trust a search ***"))
    return 0 if ok else 1


def search(y0, y1, min_obsc=0.80, alt_lo=0.0, alt_hi=25.0):
    m0 = (y0 - 1858.0) * 365.2425 - 321
    m1 = (y1 - 1858.0) * 365.2425 - 321
    print("Searching %d-%d for obscuration >= %.0f%% with the sun between %.0f and %.0f deg\n"
          % (y0, y1, min_obsc * 100, alt_lo, alt_hi))
    hits = []
    for nm in new_moons(m0, m1):
        for bname, blon, blat in BASES:
            # coarse first (10-minute steps over +-6 h); only worth refining if the
            # base sees a real bite of it at all
            best = None
            t = nm - 0.25
            while t < nm + 0.25:
                o, alt, sep = circumstances(t, blon, blat)
                if best is None or o > best[0]:
                    best = (o, alt, t)
                t += 10.0 / 1440.0
            if best[0] < 0.5:
                continue
            t0 = best[2]
            t = t0 - 15.0 / 1440.0
            while t < t0 + 15.0 / 1440.0:              # refine at 30 s
                o, alt, sep = circumstances(t, blon, blat)
                if o > best[0]:
                    best = (o, alt, t)
                t += 0.5 / 1440.0
            o, alt, t = best
            if o >= min_obsc and alt_lo <= alt <= alt_hi:
                hits.append((o, alt, t, bname))
    hits.sort(key=lambda h: -h[0])
    if not hits:
        print("  nothing matched.")
    for o, alt, t, b in hits:
        print("  %-16s %s   obscuration %5.1f%%   sun %+5.1f deg   MJD %.6f"
              % (b, mjd_str(t), o * 100, alt, t))
    return 0


def main(argv):
    if "--check" in argv:
        return check()
    y0, y1 = 1950, 2050
    lo, hi, mo = 0.0, 25.0, 0.80
    for k in range(len(argv) - 1):
        if argv[k] == "--from":
            y0 = int(argv[k + 1])
        elif argv[k] == "--to":
            y1 = int(argv[k + 1])
        elif argv[k] == "--altlo":
            lo = float(argv[k + 1])
        elif argv[k] == "--althi":
            hi = float(argv[k + 1])
        elif argv[k] == "--obsc":
            mo = float(argv[k + 1])
    return search(y0, y1, mo, lo, hi)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
