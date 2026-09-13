#!/usr/bin/env python3
# ============================================================================
# scenarios.py - ORO's demonstration scenarios, and the launchpad pages that
# explain them (2026-09-12).
# ----------------------------------------------------------------------------
# ONE TABLE generates three things that must agree with each other and never do
# when they are written by hand:
#   <Orbiter>\Scenarios\ORO\<folder>\<name>.scn     the scenario + a plain fallback
#   <Orbiter>\Html\Scenarios\ORO\<slug>.htm         the rich page, with a picture
#   <Orbiter>\Scenarios\ORO\<folder>\Description.txt  the folder's own page
#
# WHY A GENERATOR. The old ORO_beta folder shipped seven scenarios and FOUR of them
# had an empty description - a user loaded them and was told nothing about which pill
# to turn on. The description is the deliverable here, so it lives beside the state
# it describes, in one place, and the plain-text fallback is derived from the same
# words rather than written twice and left to rot.
#
# HOW THE PICTURE WORKS (TabScenario.cpp:477): a URLDESC line with NO COMMA resolves
# to a loose file, <Orbiter>\Html\Scenarios\<path>.htm - no compiled help, no hhc.exe.
# Stock's own Images\CurrentState.jpg is 1920x1080 and its page scales it with
# width="100%", which is the whole answer to "what resolution is the launchpad".
# !! AND NOT EVERY USER GETS THAT PANE: UseHtmlInline() is false with HTML scenario
# descriptions switched off AND under WINE on the default setting, and that branch
# never even looks for URLDESC. So every scenario carries a DESC block too. FindLine
# rewinds the stream before each scan, so the two coexist happily.
#
# THE STATES. We cannot fly to a state and save it, so airborne and orbital states are
# computed by tools\scnstate.py (validated against states Orbiter itself wrote) and the
# ring orbits by tools\ringplane.py. Landed vessels need none of that: "BASE <base>:<pad>"
# plus HEADING is how 505 of the 800 stock landed blocks are written.
#
# USAGE
#   python scenarios.py            write everything
#   python scenarios.py --dry      list what would be written, touch nothing
# ============================================================================
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scnstate as S                                        # noqa: E402
import ringplane as RP                                      # noqa: E402

BS = chr(92)                       # a backslash, built rather than escaped

# The flown states (see scn_text and tools/scnfreeze.py). Absent file = every scenario
# falls back to its computed state, which is how the set was built before he flew it.
FROZEN = {}
try:
    import json as _json
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           'scenarios.frozen.json'), encoding='utf-8') as _f:
        FROZEN = _json.load(_f)
except (IOError, OSError, ValueError):
    pass

ROOT = S.orbiter_root()
SCN_DIR = os.path.join(ROOT, 'Scenarios', 'ORO')
HTM_DIR = os.path.join(ROOT, 'Html', 'Scenarios', 'ORO')
PIC_SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'beta', 'pics')
PIC_URL = '../../../Images/ORO/'   # Html\Scenarios\ORO\x.htm -> <Orbiter>\Images\ORO\


# ------------------------------------------------------------------ pictures ---
def install_pictures(dry):
    """Copy the delivered shots from the repo into <Orbiter>\\Images\\ORO.

    Generating and being able to SEE the result are one step: the pages reference
    ../../../Images/ORO/<name>, so a picture that never leaves beta/pics shows as a
    broken image in his launchpad and is missing from the staged zip. Copied VERBATIM -
    a shot that arrives 1920 wide ships exactly as it was sent, because re-encoding a
    JPEG is a second lossy pass for no gain at the size the pane draws it.
    """
    import shutil
    dst = os.path.join(ROOT, 'Images', 'ORO')
    n = 0
    for f in sorted(os.listdir(PIC_SRC)):
        if f.lower().endswith('.txt'):
            continue
        stem, ext = os.path.splitext(f)
        pub = stem + ('.jpg' if ext.lower() == '.jpeg' else ext.lower())
        if dry:
            print('  would install %s' % pub)
        else:
            if not os.path.isdir(dst):
                os.makedirs(dst)
            shutil.copy2(os.path.join(PIC_SRC, f), os.path.join(dst, pub))
        n += 1
    return n


def picture(stem):
    """The published file name for a shot, or None while it has not been taken yet.
    A page only gets an <img> for a file that exists - a missing shot must read as a
    text-only description, never as a broken-image icon."""
    if not stem:
        return None
    for ext in ('.jpg', '.jpeg', '.png'):
        if os.path.isfile(os.path.join(PIC_SRC, stem + ext)):
            return stem + ('.jpg' if ext == '.jpeg' else ext)
    return None


# ------------------------------------------------------------ time of day ---
def mjd_for_sun(lon, lat, day, elev, rising=True):
    """The MJD on 'day' when the sun stands at 'elev' degrees over (lon, lat).
    Scanned rather than solved: the whole point is that it is checkable."""
    best, bestd = None, 1e9
    prev = None
    for k in range(0, 24 * 60):
        mjd = math.floor(day) + k / 1440.0
        e = S.sun_elevation(S.Planet('Earth', mjd), lon, lat, mjd)
        if prev is not None:
            up = e > prev
            if up == rising and abs(e - elev) < bestd:
                bestd, best = abs(e - elev), mjd
        prev = e
    assert best is not None and bestd < 1.0, 'no such sun elevation on that day'
    return best


# --------------------------------------------------------------- the ships ---
def landed(name, cls, base=None, pos=None, hdg=0.0, extra=()):
    out = ['%s:%s' % (name, cls), '  STATUS Landed Earth' if base or pos else '']
    if base:
        out[1] = '  STATUS Landed %s' % ('Moon' if 'Brighton' in base else 'Earth')
        out.append('  BASE %s' % base)
    if pos:
        out.append('  POS %.7f %.7f' % pos)
    out.append('  HEADING %.2f' % hdg)
    out += ['  %s' % e for e in extra]
    out.append('END')
    return '\n'.join(x for x in out if x != '')


def airborne(name, cls, mjd, lon, lat, alt, hdg, spd, fpa=0.0, aoa=0.0, extra=()):
    pl = S.Planet('Earth', mjd)
    rp, rv, ar, _ = S.make_state(pl, lon, lat, alt, hdg, spd, fpa, aoa)
    out = ['%s:%s' % (name, cls), '  STATUS Orbiting Earth',
           '  RPOS %s' % S.fmt(rp, 3), '  RVEL %s' % S.fmt(rv, 4),
           '  AROT %s' % S.fmt(ar, 3)]
    out += ['  %s' % e for e in extra]
    out.append('END')
    return '\n'.join(out)


def in_orbit(name, cls, mjd, lon, lat, alt, inc, extra=()):
    """A circular orbit passing over (lon, lat) at 'alt', at inclination 'inc'."""
    pl = S.Planet('Earth', mjd)
    v = math.sqrt(pl.GM / (pl.R + alt))
    s = math.cos(inc * S.D2R) / math.cos(lat * S.D2R)
    assert abs(s) <= 1.0, 'that latitude is outside a %.1f deg orbit' % inc
    hdg = math.asin(s) * S.R2D                       # the ascending (north-east) pass
    rp, rv, ar, _ = S.make_state(pl, lon, lat, alt, hdg, v, surface_motion=False)
    out = ['%s:%s' % (name, cls), '  STATUS Orbiting Earth',
           '  RPOS %s' % S.fmt(rp, 3), '  RVEL %s' % S.fmt(rv, 4),
           '  AROT %s' % S.fmt(ar, 3)]
    out += ['  %s' % e for e in extra]
    out.append('END')
    return '\n'.join(out)


def ring_orbit(name, rkm, above, t0=60.0, mjd=57897.25, extra=()):
    """A DeltaGlider in Saturn's ring plane at 0.1 deg, t0 seconds from the crossing.

    The elements come from ringplane.py's own derivation (ecliptic vs equator, the
    LAN+180 handedness, and the J2 circularisation - a plain e=0 line dives 1500 km an
    orbit here). The ATTITUDE is set deliberately: nose prograde, up along the spin
    axis, so the sheet lies under the ship like ground instead of standing on edge.
    """
    cfg = S.read_cfg(os.path.join(ROOT, 'Config', 'Saturn.cfg'))
    Rpl, GM = S.fget(cfg, 'Size'), S.GGRAV * S.fget(cfg, 'Mass')
    J2 = S.fget(cfg, 'JCoeff')
    ax = RP.spin_axis(cfg, mjd)
    eps = math.acos(max(-1.0, min(1.0, ax[1])))
    lan = math.atan2(-ax[0], ax[2]) % (2.0 * math.pi)
    i_pl, th = eps * S.R2D, ((lan + math.pi) % (2.0 * math.pi)) * S.R2D

    a0 = rkm * 1e3
    n = math.sqrt(GM / a0 ** 3)
    q = 1.5 * J2 * (Rpl / a0) ** 2
    a, e = a0 / (1.0 - q), q
    di = 0.1 * S.D2R
    u0 = (math.pi - n * t0) if above else (-n * t0)
    omb = (th + u0 * S.R2D) % 360.0

    p = RP.orbit_pos((i_pl + 0.1) * S.D2R, th * S.D2R, u0, a0)
    d = 1e-5
    pa = RP.orbit_pos((i_pl + 0.1) * S.D2R, th * S.D2R, u0 + d, a0)
    pb = RP.orbit_pos((i_pl + 0.1) * S.D2R, th * S.D2R, u0 - d, a0)
    vdir = S.unit([pa[k] - pb[k] for k in range(3)])
    X, Y, Z = S.axes_from_nose_up(vdir, ax)
    arot = S.arot_from_axes(X, Y, Z)
    zoff = sum(p[k] * ax[k] for k in range(3))

    out = ['%s:DeltaGlider' % name, '  STATUS Orbiting Saturn',
           '  ELEMENTS %.1f %.9f %.9f %.6f %.6f %.6f %.5f'
           % (a, e, i_pl + 0.1, th, omb, omb, mjd),
           '  AROT %s' % S.fmt(arot, 3)]
    out += ['  %s' % x for x in extra]
    out.append('END')
    return '\n'.join(out), zoff, a0 * n * di


DG_KIT = ('PRPLEVEL 0:1.000000 1:1.000000', 'NAVFREQ 0 0 0 0', 'XPDR 0',
          'HOVERHOLD 0 1 0.0000e+00 0.0000e+00', 'GEAR 1.0000 0.0000', 'HUDMode 2',
          'AAP 0:0 0:0 0:0')
DG_FLY = ('PRPLEVEL 0:1.000000 1:1.000000', 'NAVFREQ 0 0 0 0', 'XPDR 0',
          'GEAR 0.0000 0.0000', 'HUDMode 2', 'AAP 0:0 0:0 0:0')
DG_ORB = ('PRPLEVEL 0:1.000 1:1.000', 'NAVFREQ 0 0 0 0', 'GEAR 0 0.0000',
          'AAP 0:0 0:0 0:0')

KSC = (-80.6822150, 28.5963320)      # the apron his own lights rig stands on
KSC_BASE = (-80.65165, 28.58438)     # Canaveral.cfg's own Location - among the pads,
                                     # and where tools/eclipse.py computed the 1918 event
DAY = 51982.0                        # 2001-03-14, the era the other ORO scenarios use


# ============================================================== the content ===
def build():
    S_ = []

    # ------------------------------------------------------------ 1 PILOT ---
    mjd = mjd_for_sun(KSC[0], KSC[1], DAY, 45.0, rising=True)   # proper daylight
    S_.append(dict(
        folder='1 The pilot', file='Pulling G', slug='pulling_g', pic='01_pulling_g',
        title='Pulling G',
        lead='A DeltaGlider at 6 km and 300 m/s over the Cape, trimmed and level, with '
             'the physiological model driving the vision suite from the flight itself '
             'rather than from the sliders. Pull.',
        look=['Haul back and hold it. Somewhere past about 4 G the edges of the view '
              'start to desaturate and close in; hold it longer and the tunnel shuts.',
              'The PILOT page reads the felt G on all three axes and the oxygen reserve '
              'behind them - the reserve is what decides how long you last, so a brief '
              'hard pull and a long moderate one do not feel the same.',
              'Push instead of pulling and you get the other end of it: red-out, which '
              'is blood arriving where it should not.',
              'Roll and the tilt lean follows the sideways G. The seat shake is '
              'physics-driven too - it answers to thrust, air and ground, never to a '
              'slider.'],
        setup=['PILOT > G-FORCES, and set <b>Effect source</b> to PHYSICS. In LAB the '
               'sliders are the values; in PHYSICS they are gains on what the flight '
               'produces.',
               'A gain on an axis that is not loaded reads as nothing happening - that '
               'is correct, not a broken control. The readout names the axis it is '
               'waiting for.',
               'PILOT > SCENARIOS has the same effects as scripted one-click events if '
               'you would rather watch than fly.'],
        mjd=mjd, focus='GL-01', cam='cockpit', fov=50, hud='Surface',
        ships=airborne('GL-01', 'DeltaGlider', mjd, KSC[0] + 1.2, KSC[1] - 0.4,
                       6000.0, 290.0, 300.0, extra=DG_FLY)))

    # ---------------------------------------------------------- 2 REENTRY ---
    S_.append(dict(
        folder='2 Reentry', file='Entry from the cockpit', slug='entry_cockpit',
        pic='02_entry_cockpit',
        title='Entry from the cockpit',
        lead='A DeltaGlider on its way down, seen from the seat. The plasma you get in '
             'a cockpit is not the geometry you get from outside - from in here it is a '
             'luminous sheath filling the windscreen, and the cabin lights up with it.',
        look=['The sheath tracks the RELATIVE WIND, not your eyeline: look around and '
              'it stays where the air is, which is the whole difference between a light '
              'source and a decal.',
              'The cabin wash and the flare in the glass come from one shared envelope, '
              'so a flash brightens the window and the panel in the same frame.',
              'Watch the REENTRY readout climb. The effect is keyed to real heating '
              '(Sutton-Graves), so it leads and outlives the stock billboards.',
              'Late on, in the thick air, look for the vapour cone as you go subsonic.'],
        setup=['VESSEL > REENTRY > PLASMA. The pill is the A/B; <b>VC glow</b> and '
               '<b>Cabin wash</b> are the two knobs that matter from this seat.',
               'Launchpad, Video tab, Advanced: <b>Light glow</b> post-processing ON. '
               'The white core of the plasma is delegated to the bloom - without it the '
               'fire reads amber and flat. The panel caption warns you if it is off.',
               '<b>Sun glare</b> ON as well: it is what creates the depth buffer that '
               'cuts the effect at the window frame.'],
        mjd=51982.0931790329, focus='GL-01', cam='cockpit', fov=50, hud='Surface',
        ships='\n'.join([
            'GL-01:DeltaGlider',
            '  STATUS Orbiting Earth',
            '  RPOS 2615346.209 5515432.888 -2080271.339',
            '  RVEL 7003.1844 -3631.7199 -363.2383',
            '  AROT 121.342 -48.961 -109.544',
            '  PRPLEVEL 0:0.995353 1:0.996233',
            '  NAVFREQ 0 0 0 0',
            '  XPDR 0',
            '  HOVERHOLD 0 1 0.0000e+00 0.0000e+00',
            '  HUDMode 2',
            '  AAP 0:0 0:0 0:0',
            'END'])))

    S_.append(dict(
        folder='2 Reentry', file='Entry from outside', slug='entry_outside',
        pic='03_entry_outside',
        title='Entry from outside',
        lead='Atlantis coming down, from the chase camera. Everything the cockpit view '
             'hides is here: the shock shell wrapped on the hull\'s own triangles, the '
             'fins standing off the windward edges, the lofted envelope ahead of the '
             'nose and the trail streaming back.',
        look=['The shell is built from THIS vessel\'s mesh, so it fits a shuttle the way '
              'it fits a glider - no per-vessel authoring.',
              'The envelope is lofted from the airstream, not copied from the mesh, '
              'which is why it stays smooth however close you get.',
              'The trail is one ribbon threaded through a pool of path memory, anchored '
              'to the planet rather than to the camera. Pause and orbit the ship: it '
              'stays where it was shed.',
              'Turn the pill off and stock\'s two billboards come back. That is the '
              'comparison worth making once.'],
        setup=['VESSEL > REENTRY > PLASMA, per vessel class - these numbers are tuned '
               'for Atlantis and will not be the glider\'s.',
               'Launchpad: <b>Light glow</b> ON for the white core, <b>Sun glare</b> ON '
               'for the depth clip.',
               'The FLIGHT AID on VESSEL > FLIGHT AID is a TEST RIG, not an effect: it '
               'shifts the centre of pressure so a stock hull will hold a high angle of '
               'attack. It is gated to the reentry regime.'],
        mjd=52260.8841694018, focus='STS-101', cam=('extern', 8.0, 25.0, 12.0),
        fov=45, hud='Surface', ships='\n'.join([
            'STS-101:Atlantis',
            '  STATUS Orbiting Earth',
            '  RPOS 4113308.021 4514778.707 -2070278.377',
            '  RVEL 5822.3556 -5343.4398 388.5038',
            '  AROT 137.149 -68.248 -125.250',
            '  VROT -0.0614 0.0788 -0.0001',
            '  PRPLEVEL 0:0.859928',
            '  NAVFREQ 0 0',
            '  CONFIGURATION 3',
            '  GEAR 0 0.0000',
            '  ARM_STATUS 0.5000 0.0000 0.0000 0.5000 0.5000 0.5000',
            '  MET 0.000 0.000 0.000 0.000',
            'END'])))

    mjd = DAY + 0.62
    S_.append(dict(
        folder='2 Reentry', file='Transonic', slug='transonic', pic='04_transonic',
        title='Transonic',
        lead='A DeltaGlider at 2 km over the Atlantic, just under Mach 1 and '
             'accelerating. As the flow over the hull goes supersonic the pressure drop '
             'condenses the water in the air and a collar stands off the airframe.',
        look=['The LENGTH of the cone is not a slider - it is the Mach angle, so the '
              'shroud visibly stretches back as the ship outruns its own pressure waves. '
              'That is the effect.',
              'It rides the relative wind, so at a high angle of attack the collar cants '
              'off the nose, where the shock really stands.',
              'You may get NOTHING, and that is the model working: a cone needs water in '
              'the air. The AIR readout tells you the odds it drew and why - "coast 62% '
              'formed", "dry above 14.8 km".',
              'One humidity draw is made per transonic transit, so two climbs on the '
              'same day differ. Over the sea the odds are near their best.'],
        setup=['VESSEL > REENTRY > VAPOUR CONES. <b>THE AIR</b> at the top is global - '
               'Max chance, Dry ceiling, Intermittency; the cone SHAPES below it are per '
               'vessel class.',
               'There are two independent cones. The second ships at zero opacity - turn '
               'it up and put it further aft for a second collar.',
               'TEST bypasses the humidity draw and pins Mach 1.15, if you would rather '
               'judge the shape than fly for it.'],
        mjd=mjd, focus='GL-01', cam=('extern', 6.0, 110.0, 8.0), fov=40, hud='Surface',
        ships=airborne('GL-01', 'DeltaGlider', mjd, -78.0, 28.0, 2000.0, 90.0, 315.0,
                       extra=DG_FLY)))

    # ---------------------------------------------------------- 3 ENGINES ---
    mjd = mjd_for_sun(KSC[0], KSC[1], DAY, 2.0, rising=True)
    S_.append(dict(
        # RENAMED by him 2026-09-13: "Pad to vacuum" -> "Surface to space", because he
        # took it OFF the pad. The frozen state has it on the ground 3.4 km northwest of
        # the base centre with a second glider parked beside it, at a sun elevation of
        # +52.8 deg - so the old lead's "on the pad at first light" was wrong twice over
        # and is rewritten here. The computed mjd below is now only the derivation.
        folder='3 Engines', file='Surface to space', slug='surface_to_space',
        pic='05_surface_to_space',
        title='Surface to space',
        lead='A DG-S on the ground at Canaveral with the tanks full and a clear sky '
             'overhead. Fly it straight up and watch the exhaust change shape with the '
             'air it is expanding into.',
        look=['At sea level the jet is pinched and carries a shock diamond train; as the '
              'air thins the train stretches, loses its discs one at a time and the plume '
              'blooms open. It is driven by pressure, so it works at any world.',
              'Throttle transients puff. Soot sheds off the lip and dims the jet from '
              'inside - it is drawn dark over the light, because soot is IN the fire.',
              'The bell heats and cools on its own clock. Shut down at altitude and watch '
              'it fade rather than switch off.',
              'At dawn the exhaust smoke takes the same reddening the hull takes - the '
              'particles are lit by the real sun, with a real horizon.'],
        setup=['VESSEL > THRUSTERS > EXHAUST. The <b>EXPANSION BAND</b> double slider is '
               'the engine: the low handle is where the bloom opens, the high handle is '
               'the pressure the engine is RATED for. Drag it low and you have a vacuum '
               'engine shuddering on the pad.',
               'Settings are PER ENGINE GROUP - the cycler in the header picks MAIN, '
               'HOVER, RETRO, USER or RCS, and each keeps its own band, colours and soot.',
               'VESSEL > THRUSTERS > PARTICLES drives Orbiter\'s own streams; the STOCK '
               'pills turn the built-in billboards and streams off.',
               'Launchpad: <b>Light glow</b> ON, or the bell can never bloom past its own '
               'texture. <b>Particle lighting (ORO)</b> on the same page is what lets the '
               'smoke take the dawn.'],
        mjd=mjd, focus='GL-01S', cam=('extern', 7.0, 140.0, 10.0), fov=50, hud='Surface',
        ships=landed('GL-01S', 'DG-S', base='Cape Canaveral:5', hdg=0.0,
                     extra=('RCSMODE 0', 'PRPLEVEL 0:1.000000 1:1.000000 2:1.000000',
                            'NAVFREQ 94 524 84 114', 'XPDR 0', 'GEAR 1.0000 0.0000',
                            'HUDMode 2', 'PSNGR 2 3 4', 'TANKCONFIG 1',
                            'AAP 0:0 0:0 0:0'))))

    S_.append(dict(
        folder='3 Engines', file='Attitude and the bay', slug='attitude_bay',
        pic='06_attitude_bay',
        title='Attitude and the bay',
        lead='Atlantis in orbit with the payload bay open. Two things to look at here: '
             'what the attitude thrusters do, and what the inside of an open bay '
             'reflects.',
        look=['Fire the RCS. Attitude jets are a full engine group in their own right - '
              'own plume, own particles, own colours - and they settle instantly, '
              'because a pressure-fed thruster does, unlike a big pump-fed engine.',
              'The bay is the reflection case stock cannot do: a vessel is excluded from '
              'its own environment map, so stock Full Scene never shows you the orbiter '
              'or its payload. In the ORO mode it does.',
              'Close the doors and the inside goes dark. Planet shine has no occlusion '
              'term in stock, so a closed bay flying bay-to-Earth glows sky-blue inside; '
              'here the assembly shadows itself.',
              'A faint stable glow left in a closed bay is real light through places the '
              'mesh is not watertight - an authoring fact, not a lighting bug.'],
        setup=['Launchpad, Video tab, Advanced: <b>Reflections</b> set to '
               '<b>Full Scene ORO (exp)</b>. The other three settings are pixel-exact '
               'stock; everything here lives behind the fourth.',
               '<b>Shadows</b> must be enabled for the planet-shine occlusion to have a '
               'map to work with.',
               'VESSEL > THRUSTERS, with the group cycler on RCS.'],
        mjd=51982.9555277894, focus='STS-101', cam=('extern', 5.0, -35.0, 25.0), fov=50,
        hud='Orbit', ships='\n'.join([
            'STS-101:Atlantis',
            '  STATUS Orbiting Earth',
            '  RPOS 4860858.41 1140979.54 -4384066.58',
            '  RVEL 4883.630 1390.997 5857.353',
            '  AROT 96.69 -74.93 -5.17',
            '  PRPLEVEL 0:1.000',
            '  NAVFREQ 0 0',
            '  CONFIGURATION 3',
            '  CARGODOOR 1 1.0000',
            '  GEAR 0 0.0000',
            '  CARGO_STATIC_MESH Carina_cradle',
            '  CARGO_STATIC_OFS 0.000 -1.650 0.050',
            'END'])))

    # ---------------------------------------------------------- 4 WEATHER ---
    mjd = mjd_for_sun(KSC[0], KSC[1], DAY, 3.0, rising=False)
    S_.append(dict(
        folder='4 Weather', file='Storm on the ground', slug='storm_ground',
        pic='07_storm_ground',
        title='Storm on the ground',
        # His re-flight moved this an hour and a quarter earlier and put a DG-S beside the
        # glider: the sun is +18.5 deg now, not the +2.9 deg the old "on the horizon" line
        # described.
        lead='A DeltaGlider and a DG-S parked at the Cape in the late afternoon light. '
             'Turn the rain on and stand in it.',
        look=['The sun collapses at the SOURCE rather than being darkened afterwards, so '
              'the shadows go with it. Nothing is painted over the frame.',
              'The ground soaks before it pools. Wetness lags the rain by about twenty '
              'seconds, so the apron darkens behind the storm arriving.',
              'Pools are pinned to the ground, not to you - taxi and you drive across a '
              'fixed pattern. The ship is reflected in them, and so are its beacons, its '
              'exhaust and its contrail.',
              'Look up: two real textured cloud decks, counter-drifting, sagging under '
              'their own weight. Look at the runway: paved surfaces get the film and the '
              'mirror and deliberately no puddles, because a runway is built to shed '
              'water.',
              'Lightning strikes the ground and lights the ship, the rain and the deck '
              'from one event. The thunder arrives late by the distance.'],
        setup=['WORLD > WEATHER > RAIN. The pill is instant both ways so you can A/B it; '
               'the build-up ramps over about ten seconds.',
               '<b>Rain view</b> chooses VC only, VC + panel, or all views. Each mode is '
               'gated on the thing that actually makes it correct.',
               'Launchpad: <b>Sun glare</b> ON (the depth buffer cuts the rain at the '
               'window frame), and <b>Terrain and world shadows</b> on '
               '<b>Cascaded (ORO)</b> for shadows that survive the weather.',
               'Sounds are on the same page, with the cabin\'s own mix under '
               'PILOT > VIRTUAL COCKPIT.'],
        mjd=mjd, focus='GL-01', cam=('extern', 5.0, 35.0, 8.0), fov=50, hud='Surface',
        ships=landed('GL-01', 'DeltaGlider', pos=KSC, hdg=330.0, extra=DG_KIT)))

    mjd = DAY + 0.58
    S_.append(dict(
        folder='4 Weather', file='Into the weather', slug='into_weather',
        pic='08_into_weather',
        title='Into the weather',
        lead='The same storm, from inside the cockpit at 1.5 km and 180 m/s. What the '
             'glass does with the water is a function of how hard the air is working on '
             'it.',
        look=['The runners do not fall down the windscreen - they stream out from the '
              'STAGNATION POINT, where the air first meets the airframe. On a glider that '
              'point sits about 25 degrees below the nose, so they sweep up and outward '
              'across the glass.',
              'Speed up and the radiant walks: overhead when parked, crossing the forward '
              'axis around 45 m/s, and below it at flying speeds.',
              'Push the speed further and the standing drops are torn off while the '
              'runners multiply and a thin film comes up - one sensed number, keyed to '
              'dynamic pressure rather than speed, so it is honest in thin air too.',
              'The HUD stays readable: it is drawn after the drops, so the water lenses '
              'the world and not the numbers.'],
        setup=['WORLD > WEATHER > RAIN, the <b>THE WINDSCREEN (VC)</b> group.',
               'The glass has to be declared. ORO ships the stock glider\'s windscreen '
               'already marked; for any other vessel use the <b>RAINSURFACES</b> button '
               'and click the pane in the sim.',
               '<b>Mask debug</b> shows you what ORO thinks the glass is, which is the '
               'one question a screenshot cannot answer.'],
        mjd=mjd, focus='GL-01', cam='cockpit', fov=50, hud='Surface',
        ships=airborne('GL-01', 'DeltaGlider', mjd, KSC[0] + 0.9, KSC[1] - 0.3,
                       1500.0, 300.0, 180.0, extra=DG_FLY)))

    mjd = mjd_for_sun(KSC[0], KSC[1], DAY, 1.0, rising=True)
    S_.append(dict(
        folder='4 Weather', file='Fog at first light', slug='fog_first_light',
        pic='09_fog_first_light',
        title='Fog at first light',
        lead='Dawn at the Cape with the air thick. Fog in ORO is two analytic height '
             'layers integrated along the line of sight in every shader family at once, '
             'so terrain, buildings, hulls, particles and runway lights all agree about '
             'the same air.',
        look=['The colour is not a picker - it is sun irradiance, so it is warm at dawn, '
              'grey under a storm and dark at night, and the fog does not brighten the '
              'scene when the sun goes down.',
              'The sun on every surface is attenuated by the air above it, so shadows '
              'soften and can vanish entirely as the fog thickens.',
              'The approach lights are the thing to watch: a lamp in fog is an AUREOLE '
              'that grows with the optical depth, not a brighter lamp.',
              'Layer one is the ordinary air; layer two is the storm mist, which is what '
              'makes the rain\'s gloom a real medium rather than a tint.'],
        setup=['WORLD > WEATHER > FOG. <b>Base lights</b> at the bottom of this page and '
               'of the RAIN page are the same setting behind two doors - it forces the '
               'base\'s night lighting on and gives the lamps their halo.',
               'PILOT > VIRTUAL COCKPIT > CABIN AT NIGHT dims the cabin under weather, '
               'which is what stops the panel reading as daylight inside a grey morning.'],
        mjd=mjd, focus='GL-01', cam=('extern', 6.0, 90.0, 4.0), fov=50, hud='Surface',
        ships=landed('GL-01', 'DeltaGlider', pos=KSC, hdg=330.0, extra=DG_KIT)))

    # ------------------------------------------------- 5 NIGHT AND LIGHTS ---
    mjd = mjd_for_sun(-82.4, 23.0, DAY, -25.0, rising=False)
    S_.append(dict(
        folder='5 Night and lights', file='A base at night', slug='base_at_night',
        pic='10_base_at_night',
        title='A base at night',
        lead='Habana in the small hours. Most of what is on show here is the patched '
             'client doing things stock never did - and two of them had been broken '
             'since terrain arrived.',
        look=['The monorail runs, and so do the hangrail cabins. Under any graphics '
              'client these have been frozen exports since the client split; ORO builds '
              'and animates them itself, on the core\'s own laws, and lays the track as '
              'the slope-limited upper envelope of the ground with pylons where it '
              'leaves it.',
              'Base buildings light their windows from ordinary <name>_n night textures, '
              'which stock only ever honoured for a few object types.',
              'Local lights cast real shadows: a spotlight beam is carved by a hangar, '
              'and a vessel standing in the beam throws a shadow on the ground.',
              'Inside, the cabin goes dark because it is night - and stays readable, '
              'because lit instruments are not dimmed with the walls.'],
        setup=['WORLD > WEATHER > RAIN or FOG for the <b>Base lights</b> pill; '
               'PILOT > VIRTUAL COCKPIT for <b>CABIN AT NIGHT</b>.',
               'Launchpad, Video tab, Advanced: <b>Local lights</b> at 8x Full or better, '
               '<b>Spot light shadows</b> at 4 or 6 maps, <b>Point light shadows</b> on '
               'Aimed or Cube, <b>Terrain and world shadows</b> on <b>Cascaded (ORO)</b>.',
               'The trains and the solar plant are the client\'s, not the panel\'s - '
               'there is nothing to switch on.'],
        mjd=mjd, focus='GL-01S', cam=('extern', 12.0, 30.0, 15.0), fov=55, hud='Surface',
        ships=landed('GL-01S', 'DG-S', base='Habana:2', hdg=5.0,
                     extra=('RCSMODE 0', 'PRPLEVEL 0:0.300 1:1.000 2:0.300',
                            'NAVFREQ 94 524 84 114', 'XPDR 0', 'GEAR 1.0000 0.0000',
                            'HUDMode 2', 'PSNGR 2 3 4', 'TANKCONFIG 1',
                            'AAP 0:0 0:0 0:0'))))

    S_.append(dict(
        folder='5 Night and lights', file='Lights and shadows', slug='lights_shadows',
        pic='11_lights_shadows',
        title='Lights and shadows',
        lead='An automatic twelve-case tour of the local-light and shadow work. It spawns '
             'lamp posts and extra vessels around the glider, runs each case for about '
             'half a minute with the camera orbiting, and says on screen what you should '
             'be seeing and which Launchpad row the case needs. Everything it spawns is '
             'deleted at the end; your glider is untouched.',
        look=['T skips to the next case, R goes back one, Ctrl+P pauses with the sim.',
              'The cases: one spot; the glider\'s own landing light; two spots; four and '
              'six in a fan; a cluster sharing one map; more lights than maps; a stadium '
              'flood; a point light in a ring of casters; spots ranking before points; '
              'sixteen lights at once; and the same scene by day.',
              'The title card reads your own settings back and warns about any row a case '
              'needs and does not have, so it is worth running whatever your settings are.',
              'The lamp palette is eight TINTED WHITES rather than primaries on purpose: '
              'a shadow is its own lamp\'s contribution removed, and a saturated blue '
              'lamp removes almost no brightness, so its shadow cannot be seen.'],
        setup=['For a full pass: <b>Local lights</b> 16x Full, <b>Spot light shadows</b> '
               '6 maps, <b>Point light shadows</b> Cube, <b>Terrain and world shadows</b> '
               'Cascaded (ORO).',
               'Needs the <b>LuaInline</b> module ticked in the Launchpad\'s Modules tab.',
               'If you are reporting something, say which case number it was, send an '
               'UNPAUSED screenshot, and set <b>ShadowDebug = 1</b> in D3D9Client.cfg '
               'first - the client then logs a line a second and the rig puts it straight '
               'into the caption.'],
        mjd=51982.6780413247, focus='GL-01', cam=('extern', 8.0, 30.0, 12.0), fov=50,
        hud='Surface', env=('Script testlights',),
        ships=landed('GL-01', 'DeltaGlider', pos=KSC, hdg=330.0, extra=DG_KIT)))

    # ------------------------------------------------------------ 6 SKY ---
    night = pick_night(DAY + 0.25, 55.0)   # 70 deg orbit: 51.6 cannot reach the oval
    S_.append(dict(
        folder='6 The sky', file='Night side', slug='night_side', pic='12_night_side',
        title='Night side',
        # 55 deg since 2026-09-13 (his re-flight). Verified against the frozen state
        # vectors rather than taken on trust: the orbit measures 55.00 deg to Earth's
        # equator, by the same convention the old 70 used (both retrograde - 125.00 and
        # 110.00 formally - so the number quoted is the angle to the equatorial plane).
        lead='A glider on the night side of Earth at 400 km, in a 55 degree orbit. Two '
             'effects share this pass: the aurora over the pole, and the storms in the '
             'cloud deck below you.',
        look=['The curtains are built around the MAGNETIC pole, not the spin axis, so '
              'both ovals move together as a dipole - and the colour is by ALTITUDE, '
              'because which line emits depends on how deep the particles get. Earth '
              'needs three: violet at the base, green through the body, red at the top.',
              'Twelve worlds carry auroras, and a world with none is simply one whose '
              'activity is zero - turn it up anywhere and it has one.',
              'The lightning is not decoration: the storm cells are placed from the '
              'planet\'s OWN cloud map, so a flash only ever happens where there is cloud '
              'to light up, and the storms ride the rotating deck.',
              'Flashes light the cloud image itself, in the cloud\'s own shape, and a '
              'restrike flickers down the same channel. Day side gets none: the eye '
              'cannot see a diffuse in-cloud flash against sunlit deck, and every '
              'astronaut photograph of one is a night photograph.'],
        setup=['WORLD > AURORA and WORLD > WEATHER > LIGHTNING. Both save PER BODY, so '
               'Earth\'s settings are Earth\'s.',
               'Both have a TEST toggle that bypasses the gates - a polar night and a '
               'storm are not things you should have to wait for.',
               'The night cloud deck itself is a client fix: stock renders from-above '
               'night clouds at exactly zero alpha, so the deck a flash lights up was '
               'invisible.'],
        mjd=night[0], focus='GL-01', cam=('extern', 9.0, -20.0, 18.0), fov=55,
        hud='Orbit', ships=in_orbit('GL-01', 'DeltaGlider', night[0], night[1], night[2], 400000.0,
                       70.0, extra=DG_ORB)))

    mjd = mjd_for_sun(KSC[0], KSC[1], DAY, 4.0, rising=True)
    S_.append(dict(
        folder='6 The sky', file='The sun', slug='the_sun', pic='13_the_sun',
        title='The sun',
        lead='Sunrise at the Cape, with the sun low enough to come through things. Two '
             'effects hand over to each other across an ascent: shafts want a medium to '
             'scatter in, a lens flare wants a concentrated source, so the rays are a '
             'low-altitude effect and the flare is a high one.',
        look=['God rays need an OCCLUDER. With the sun in open sky the technique can only '
              'smear it into a halo; put a building or a ridge in front of it and you get '
              'shafts across the whole sky.',
              'The sun hides behind terrain properly: the glare is tested against the '
              'ground itself, marched out to 160 km, so it sets behind a ridge instead of '
              'painting through it.',
              'Shadows at this sun angle are the cascaded atlas at its hardest - long, '
              'crisp, and cast by buildings and terrain alike.',
              'Now fly up. As the air thins the rays fade and the lens flare comes in: '
              'ghosts down the diagonal, an iris starburst, an anamorphic streak. Four '
              'optics, and it is EXTERNAL VIEWS ONLY - a healthy eye has no lens '
              'elements, so there is none in the cockpit.'],
        setup=['WORLD > GOD RAYS - the shafts and, in the second group, the LENS FLARE '
               'with its <b>Mode</b> slider for the four optics.',
               'Launchpad: <b>Sun glare</b> ON. Both effects read the sun the client has '
               'already drawn and already occluded, so without it there is nothing to '
               'read.',
               '<b>Terrain and world shadows</b> on <b>Cascaded (ORO)</b> for the long '
               'shadows.'],
        mjd=mjd, focus='GL-01', cam=('extern', 7.0, 250.0, 3.0), fov=50, hud='Surface',
        ships=landed('GL-01', 'DeltaGlider', pos=KSC, hdg=150.0, extra=DG_KIT)))

    # 20 May 2012, the annular that crossed the American southwest, seen from the
    # Edwards lakebed. tools/eclipse.py searched nine stock bases over 1960-2062 under
    # HIS framing rule - the sun no higher than 25 deg, or a camera that contains it
    # contains nothing else - and this is the best of them: 82.1% with the sun 13.4 deg
    # up, bearing 285.5. The 2045 totality at the Cape is deeper (100%, 2m42s) and was
    # built first, but its sun is 76 deg up and cannot share a frame with the ground.
    # ⚠️ THE SHOT IS COMPOSED, not left to chance: a GROUND camera at the base centre
    # looks along the sun's own bearing, and the ship is placed 150 m out along that
    # same line, so camera -> ship -> lakebed -> sun are one straight view. The ship is
    # clear of every building in Edwards.cfg (the nearest is 432 m from the centre).
    ECL_MAX = 56068.068972          # maximum eclipse at Edwards
    EDW = (-117.880203, 34.900902)  # Edwards.cfg's own Location
    S_.append(dict(
        folder='6 The sky', file='Eclipse over Edwards', slug='eclipse_edwards',
        pic='14_eclipse_edwards',
        title='Eclipse over Edwards',
        # He re-framed this one and started it earlier: the frozen date is 44.8 minutes
        # before maximum, not the fifteen the old lead claimed.
        lead='The lakebed at Edwards, the evening of 20 May 2012, three quarters of an '
             'hour before '
             'the deepest point of the annular eclipse that crossed the American '
             'southwest. The sun goes 82 per cent covered while it is only thirteen '
             'degrees up, so the sun, the ship and the base are all in the same frame.',
        look=['It is an ANNULAR eclipse: the moon is near its far point and its disc is '
              'six per cent SMALLER than the sun\'s, so even on the centre line it can '
              'never cover it - what people further north saw was a ring. Edwards is off '
              'that line, so here it is a deep crescent instead.',
              'ORO needed nothing special for that. The obscuration is computed at the '
              'CAMERA against every body in the system as two overlapping angular discs, '
              'so annular, partial and total are the same arithmetic - and so is an '
              'occulter nearer or further than the geometric umbra tip.',
              'The eye adapts ASYMMETRICALLY - about eighteen seconds opening up, a '
              'second and a bit closing down - which is why coming out of one dazzles and '
              'going in merely gropes. That is the whole reason this is modelled as an '
              'eye behind the camera and not as a filter over the frame.',
              'Two readouts, because they genuinely diverge: the sun can be most of the '
              'way covered while a fully adapted eye is doing almost nothing. Correct, '
              'and it would otherwise read as broken.',
              'Watch the LAKEBED and the hangars rather than the sky. Colour drains as the '
              'cones give up, and on a surface this flat and this pale it is obvious.',
              'The sun bears 285 degrees and the camera starts pointed at it. This is also '
              'where the Shuttle came home, which is worth a moment.'],
        setup=['WORLD &gt; ECLIPSE, global scope - the same pilot sits behind every canopy.',
               'The TEST toggle runs the whole cycle in forty seconds, if you would rather '
               'not fly to one.',
               'Time-accelerate to the deepest point if you like, but DROP BACK TO 1x for '
               'the last minute: the eye model runs on REAL time, so at 100x the '
               'adaptation never happens and you see a dimmer instead of an eclipse.',
               'The shadow an eclipse casts on a planet seen from orbit is the client\'s '
               'own and needs nothing from ORO.'],
        mjd=ECL_MAX - 15.0 / 1440.0, focus='GL-01',
        cam=('ground', EDW[0], EDW[1], 3.0, 285.5, 7.0), fov=50,
        hud='Surface', ships=landed('GL-01', 'DeltaGlider', pos=(-117.881786, 34.901262), hdg=195.0,
                     extra=DG_KIT)))

    a_txt, a_z, a_vz = ring_orbit('GL-01', 110000.0, above=True, extra=DG_ORB)
    S_.append(dict(
        folder='6 The sky', file='Rings - the crossing', slug='rings_crossing',
        pic='15_rings_crossing',
        title='Rings - the crossing',
        lead='A glider inside Saturn\'s B ring, %.0f metres above the ring plane and '
             'falling through it at %.0f metres a second. One minute to the crossing, '
             'from the cockpit.' % (a_z, a_vz),
        look=['The orbit is inclined a tenth of a degree to the equator, which is what '
              'makes this watchable: at 25 degrees a ring crossing is a blink.',
              'You are over the LIT face, so the sheet under you carries its own texture. '
              'Broad grooves come in below about 600 km, the fine ones and the grain much '
              'closer, and the density is read as height so every ridge has a lit and a '
              'shaded side.',
              'The grain co-rotates at the orbital rate for YOUR radius - it is a swarm, '
              'not a decal.',
              'Then you go through, and the face inverts: the thick B ring goes dark and '
              'the Cassini Division goes bright, because you are now looking at light '
              'that came THROUGH the ring. Every Cassini image of the unlit face shows it.',
              'Saturn\'s own globe is on its night side from here, which is what makes the '
              'sunlit sheet read.'],
        setup=['WORLD > RINGS. The pill at zero is stock ARITHMETICALLY, so one click is '
               'the A/B; <b>Detail</b>, <b>Contrast</b> and <b>Relief</b> shape the '
               'close-up and each is an exact A/B at zero.',
               'The effect follows the CAMERA, not the ship - pull the external view out '
               'and back in and the detail follows you.',
               'Nothing to set in the Launchpad. The rings are not on the terrain shader, '
               'so none of its budgets apply.'],
        mjd=57897.25, focus='GL-01', cam='cockpit', fov=60, hud='Orbit', ships=a_txt))

    b_txt, b_z, b_vz = ring_orbit('GL-01', 119800.0, above=False, extra=DG_ORB)
    S_.append(dict(
        folder='6 The sky', file='Rings - the wide view', slug='rings_wide',
        pic='16_rings_wide',
        title='Rings - the wide view',
        lead='The same flight in the Cassini Division, where the sheet is thin - %.0f '
             'metres BELOW the plane and rising through it at %.0f metres a second, '
             'starting wide so the whole planet is in frame.' % (-b_z, b_vz),
        look=['The ring\'s SHADOW lies across the southern hemisphere. Stock draws no '
              'ring shadow on a planet at all - grepped for, in every planet, cloud and '
              'haze shader and in the core\'s own renderer - and this one carries the '
              'ring\'s structure, so the Cassini Division reads as a bright line inside '
              'the dark band.',
              'It is seasonal for free, off the 26.7 degree obliquity. This is May 2017, '
              'a day from Saturn\'s northern summer solstice, which is why the band is so '
              'wide.',
              'Anything inside that shadow is shaded by what the ring lets through, the '
              'ship included - watch the hull as you cross it.',
              'You are under the UNLIT face here, so the thin Division is the bright part '
              'and the dense ring beside it is dark: the same inversion the other scenario '
              'shows from the other side.',
              'The sheet goes opaque as it turns edge-on, which is the honest '
              'grazing-angle thickness - the main rings are ten to thirty metres thick, so '
              'a uniform slab would be a physics error.'],
        setup=['WORLD > RINGS. <b>Density</b> scales the optical depth, and because the '
               'trim sits inside tau it dims the sun through the rings for free.',
               'Stock\'s own ring shader samples its texture with a smoothstep where the '
               'texture is authored linear, which displaces ring structure by up to 6,300 '
               'km - more than the Cassini Division is wide. ORO samples it linearly, so '
               'the features sit where they belong.',
               'Press F1 for the cockpit if you would rather ride it through.'],
        mjd=57897.25, focus='GL-01', cam=('externto', 'Saturn'), fov=75, hud='Orbit', ships=b_txt))

    return S_


def pick_night(day, lat):
    """An MJD and a longitude where 'lat' is in deep darkness - for the orbital night."""
    best = None
    for k in range(0, 24 * 60, 5):
        mjd = math.floor(day) + k / 1440.0
        pl = S.Planet('Earth', mjd)
        for lon in range(-180, 180, 5):
            e = S.sun_elevation(pl, lon, lat, mjd)
            if best is None or e < best[3]:
                best = (mjd, lon, lat, e)
    assert best[3] < -15.0, 'no deep night found at that latitude'
    return best


# ================================================================== output ===
FOLDERS = [
    dict(dir='1 The pilot', slug='folder_pilot', pic='01_pulling_g',
         title='The pilot',
         lead='Everything in ORO that happens to the person in the seat rather than to '
              'the world outside it: the vision suite under G, the tilt, the seat, the '
              'heartbeat, and the cabin at night.',
         body='<p>The whole family can run two ways. In <b>LAB</b> the sliders ARE the '
              'values, which is how a look gets found. In <b>PHYSICS</b> they become '
              'gains on a model of a real pilot - proper acceleration at the head, body '
              'axes by posture, cardiovascular lag, and an oxygen reserve that decides '
              'how long you last.</p>'
              '<p>PILOT &gt; SCENARIOS will induce and recover from any of it on one '
              'click, with sound, if you would rather watch than fly.</p>'),
    dict(dir='2 Reentry', slug='folder_reentry', pic='03_entry_outside',
         title='Reentry',
         lead='The plasma, the trail, the vapour cone - what a hull does to the air it is '
              'arriving through, from both seats.',
         body='<p>The heating is real (Sutton-Graves, which peaks higher and earlier than '
              'the stock model) and it drives everything. What you SEE differs completely '
              'by viewpoint, and deliberately: from outside it is a shock shell wrapped on '
              'the vessel\'s own triangles with fins, an envelope and a ribbon trail; from '
              'the seat it is a luminous sheath, because a draw list built from hull '
              'points starves when the eye is inside the hull.</p>'
              '<p>Tuning is PER VESSEL CLASS. The numbers that suit a glider do not suit a '
              'shuttle, and that is a fact about the airframe, not about the pilot.</p>'),
    dict(dir='3 Engines', slug='folder_engines', pic='05_surface_to_space',
         title='Engines',
         lead='Exhaust that answers to the air it is expanding into: plume shape, shock '
              'diamonds, soot, the bell\'s own heat, the throat fire, and Orbiter\'s own '
              'particle streams driven from live sliders.',
         body='<p>Everything here is <b>per engine group</b> - MAIN, HOVER, RETRO, USER '
              'and RCS each carry their own expansion band, colours, soot and particles, '
              'because a vacuum-rated main and a sea-level hover are not the same engine. '
              'A single thruster can override its group where an addon has grouped things '
              'oddly.</p>'
              '<p>ORO can also turn Orbiter\'s own exhaust billboards and streams off, per '
              'vessel, so its plume replaces them rather than stacking with them - or '
              'leave both on, which is legal and will be saved.</p>'),
    dict(dir='4 Weather', slug='folder_weather', pic='07_storm_ground',
         title='Weather',
         lead='The surface storm: rain you stand in, wet ground that pools and mirrors, a '
              'textured cloud ceiling, lightning to the ground, drops on the glass, fog '
              'with real depth, and the sounds.',
         body='<p>Most of this is not drawn by ORO at all - it is the patched client\'s '
              'own ground and vessel shaders, driven by ORO. That is what lets the storm '
              'collapse the SUN AT ITS SOURCE instead of darkening the finished frame, so '
              'the shadows go with the light.</p>'
              '<p>It is a switch, not a forecast: you turn the storm on where you are. A '
              'weather model that finds the rain from the planet\'s own cloud map is the '
              'next chapter, and the machinery that reads those maps is already here, '
              'placing the lightning.</p>'),
    dict(dir='5 Night and lights', slug='folder_night', pic='10_base_at_night',
         title='Night and lights',
         lead='What the patched client does after dark: local lights that cast real '
              'shadows, cascaded shadows over the whole scene, base night textures, lamp '
              'haloes in fog, a cabin that goes dark, and the animated base objects '
              'brought back to life.',
         body='<p>Almost none of this has a pill in the panel - it lives in the '
              'Launchpad\'s Video tab under Advanced, and in the client. The <b>Shadows</b> '
              'box there carries vessel self-shadows, the terrain and world shadow mode, '
              'the cascade detail and reach, and the spot and point light shadow maps.</p>'
              '<p>Several of these are stock defects rather than new features: runway '
              'lights that shone through hills, base structures that cast no depth, '
              'night textures that only worked for some object types, and monorails that '
              'have been frozen under every graphics client since the client split.</p>'),
    dict(dir='6 The sky', slug='folder_sky', pic='16_rings_wide',
         title='The sky',
         lead='Auroras, lightning seen from orbit, eclipses, god rays and the lens flare, '
              'and Saturn\'s rings as a physical sheet you can fly through.',
         body='<p>These are the effects with the least to configure and the most to look '
              'at. Auroras and lightning save <b>per body</b> - an unlisted world is '
              'simply one whose activity is zero, so turn it up anywhere and it has '
              'one.</p>'
              '<p>The eclipse is the odd one: it models the OBSERVER rather than the '
              'light, because the client already darkens a planet under a moon\'s shadow. '
              'What no renderer models is the eye behind the camera.</p>'),
]

ROOT_PAGE = dict(
    slug='index', pic='00_banner', title='ORO - Orbiter Realism Overhaul',
    lead='Atmospheric, physiological and visual immersion for Orbiter. These scenarios '
         'are a tour: each one puts you somewhere an effect can be seen, and its '
         'description says exactly what to switch on.',
    body='<p><b>The control panel</b> is Ctrl+F4, or Custom Functions. It opens on a menu '
         'of three: WORLD, VESSEL, PILOT. Every page has a <b>HELP</b> button that '
         'explains its own controls, and the strip at the top is the master switch - '
         '<b>Ctrl+G</b> arms and disarms everything from anywhere, and hands the sim back '
         'exactly as it found it.</p>'
         '<p><b>ORO needs its patched D3D9Client</b>, which the installer put in for you '
         'and the uninstaller puts back. Four rows in the Launchpad\'s Video tab, under '
         'Advanced, are worth setting before you start:</p>'
         '<ul>'
         '<li><b>Sun glare</b> - ON. It is what creates the depth buffer that cuts effects '
         'correctly against hulls, terrain and window frames. Without it several effects '
         'degrade silently.</li>'
         '<li><b>Light glow</b> post-processing - ON. Plasma and hot metal delegate their '
         'white to the bloom; without it a reentry reads amber and flat.</li>'
         '<li><b>Terrain and world shadows</b> - <b>Cascaded (ORO)</b>. One shadow atlas '
         'for the whole scene, vessels and buildings and terrain alike.</li>'
         '<li><b>Local lights</b>, <b>Spot light shadows</b> and <b>Point light shadows</b> '
         '- as high as your card is happy with.</li>'
         '</ul>'
         '<p>Nothing here is required reading. Load one, turn the pill on, and turn it off '
         'again - almost every effect in ORO is built so that off is stock, exactly.</p>')

STYLE = ('<style type="text/css">\n'
         'body{font-family:Arial;font-size:12px;margin:6px}\n'
         'p{margin-top:0;margin-bottom:0.6em}\n'
         'h1{font-size:150%;font-weight:normal;margin-bottom:0.4em;color:#000080;'
         'background-color:#E6E6FF;padding:0.1em}\n'
         'h2{font-size:112%;font-weight:bold;margin:0.9em 0 0.3em 0;color:#000080}\n'
         'ul{margin:0 0 0.6em 0;padding-left:1.4em}\n'
         'li{margin-bottom:0.3em}\n'
         'img{margin:0.4em 0}\n'
         '.set{background-color:#F2F2F2;border-left:4px solid #B0B0C8;padding:0.5em 0.8em;'
         'margin-bottom:0.6em}\n'
         '</style>')


def esc(t):
    # ' > ' is a menu path (WORLD > WEATHER > RAIN), not markup. A tag never has a
    # space before its '>', so this cannot touch one - and plain() turns it back.
    return t.replace(' > ', ' &gt; ')


def html(title, lead, pic, look, setup, body=''):
    title, lead, body = esc(title), esc(lead), esc(body)
    look = [esc(x) for x in look]
    setup = [esc(x) for x in setup]
    out = ['<!DOCTYPE HTML PUBLIC "-//IETF//DTD HTML//EN">', '<HTML>', '<HEAD>',
           '<meta http-equiv="Content-Type" content="text/html; charset=iso-8859-1">',
           STYLE, '</HEAD>', '<BODY>', '<h1>%s</h1>' % title, '<p>%s</p>' % lead]
    f = picture(pic)
    if f:
        out.append('<img src="%s%s" width="100%%" alt="%s" />' % (PIC_URL, f, title))
    if body:
        out.append(body)
    if look:
        out.append('<h2>What to look for</h2><ul>')
        out += ['<li>%s</li>' % x for x in look]
        out.append('</ul>')
    if setup:
        out.append('<h2>What to set</h2><div class="set"><ul>')
        out += ['<li>%s</li>' % x for x in setup]
        out.append('</ul></div>')
    out += ['</BODY>', '</HTML>', '']
    return '\n'.join(out)


def plain(lead, look, setup, body=''):
    """The DESC fallback - what a user with HTML descriptions off, or under WINE on the
    default setting, actually sees. Same words, no markup."""
    def strip(t):
        for a, b in (('<b>', ''), ('</b>', ''), ('<i>', ''), ('</i>', ''),
                     ('<p>', ''), ('</p>', ''), ('<ul>', ''), ('</ul>', ''),
                     ('<li>', ''), ('</li>', ''), ('&gt;', '>'), ('&lt;', '<'),
                     ('&amp;', '&')):
            t = t.replace(a, b)
        return t
    out = [strip(lead)]
    if body:
        out += ['', strip(body)]
    if look:
        out += ['', 'WHAT TO LOOK FOR'] + ['  - ' + strip(x) for x in look]
    if setup:
        out += ['', 'WHAT TO SET'] + ['  - ' + strip(x) for x in setup]
    return '\n'.join(out)


def camera_block(cam, target, fov):
    if cam == 'cockpit':
        return ['  TARGET %s' % target, '  MODE Cockpit', '  FOV %.2f' % fov]
    if isinstance(cam, tuple) and cam[0] == 'ground':
        # A GROUND OBSERVER, the stock lunar-eclipse scenario's own construction
        # (Camera.cpp: TRACKMODE Ground <body> + GROUNDLOCATION + GROUNDDIRECTION,
        # which clears tgtlock so the DIRECTION wins over the target). It is the only
        # way to aim the camera at something the scenario knows the bearing of - the
        # external camera's POS angles are relative to the target's own frame.
        _, glon, glat, galt, az, el = cam
        return ['  TARGET %s' % target, '  MODE Extern', '  TRACKMODE Ground Earth',
                '  GROUNDLOCATION %.5f %.5f %.2f' % (glon, glat, galt),
                '  GROUNDDIRECTION %.2f %.2f' % (az, el),
                '  FOV %.2f' % fov]
    if isinstance(cam, tuple) and cam[0] == 'externto':
        return ['  TARGET %s' % target, '  MODE Extern',
                '  TRACKMODE TargetTo %s' % cam[1], '  POS 9.00 0.00 0.00',
                '  FOV %.2f' % fov]
    rd, ph, th = cam[1], cam[2], cam[3]
    return ['  TARGET %s' % target, '  MODE Extern',
            '  POS %.2f %.2f %.2f' % (rd, ph, th),
            '  TRACKMODE TargetRelative', '  FOV %.2f' % fov]


def scn_text(s):
    # ⚠️ THE FLOWN STATE WINS (2026-09-13, his ruling: "All my changes must remain as the
    # true scenarios now"). He flew all sixteen, moved vessels and cameras, added a second
    # ship to two of them and re-saved - and Orbiter's own save is a better answer than the
    # computation for those, because it is the state he judged on screen. So the three
    # blocks Orbiter writes - the environment Date, CAMERA and SHIPS - come from
    # scenarios.frozen.json when that scenario has been frozen (tools/scnfreeze.py), and
    # the table keeps the PROSE plus the derivation that built the scenario in the first
    # place. Nothing here computes a state that then loses to the file beside it.
    fz = FROZEN.get(s['slug'], {})
    head = ['BEGIN_URLDESC', 'ORO' + BS + s['slug'], 'END_URLDESC', '',
            'BEGIN_DESC', plain(s['lead'], s['look'], s['setup']), 'END_DESC', '']
    if fz.get('tail'):
        return '\n'.join(head + fz['tail'] + [''])
    out = head + ['BEGIN_ENVIRONMENT', '  System Sol', '  Date MJD %.7f' % s['mjd']]
    out += ['  %s' % e for e in s.get('env', ())]
    out += ['END_ENVIRONMENT', '', 'BEGIN_FOCUS', '  Ship %s' % s['focus'], 'END_FOCUS',
            '', 'BEGIN_CAMERA']
    out += camera_block(s['cam'], s['focus'], s['fov'])
    # HIS RULE, 2026-09-13, and it is global: Orbit on the left, Surface on the right,
    # in every scenario. No Map MFD anywhere. Per-scenario MFD choices are gone.
    out += ['END_CAMERA', '', 'BEGIN_HUD', '  TYPE %s' % s['hud'], 'END_HUD', '',
            'BEGIN_MFD Left', '  TYPE Orbit', 'END_MFD', '',
            'BEGIN_MFD Right', '  TYPE Surface', 'END_MFD', '',
            # An EMPTY VC block is what makes the virtual cockpit the internal view: a
            # scenario with no BEGIN_VC drops a user switching in from outside into the
            # 2D panel instead, and most of ORO's cockpit work is VC-only.
            'BEGIN_VC', 'END_VC', '',
            'BEGIN_SHIPS', s['ships'], 'END_SHIPS', '']
    return '\n'.join(out)


# ⚠️ THE GUARD, and it exists because it was needed too late (2026-09-13). These files
# are GENERATED but they live in the sim tree, where they are also the obvious thing to
# hand-edit while judging a scenario - and a regeneration silently overwrote a set of his
# edits. So the generator keeps a manifest of what it last wrote, and REFUSES to overwrite
# a file whose bytes have changed since: exactly the byte-identical rule the uninstaller
# uses to decide what it may delete. An edited file is kept and named.
# Settle a hand edit by putting the value in the TABLE above; --force overrides.
MANIFEST = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'scenarios.manifest')
_manifest = {}
_kept = []


def _load_manifest():
    global _manifest
    _manifest = {}
    if os.path.isfile(MANIFEST):
        for line in open(MANIFEST, encoding='utf-8'):
            line = line.rstrip('\n')
            if ' *' in line:
                h, p = line.split(' *', 1)
                _manifest[p] = h


def _save_manifest():
    # Entries for files that no longer exist are dropped: a scenario that is renamed or
    # retired would otherwise leave its row behind for ever (the Baikonur eclipse and
    # "Pad to vacuum" both did, 2026-09-13). A dangling row is harmless to the guard,
    # which only looks up a path it is about to write - but a manifest that lists files
    # nobody can find is one more thing to wonder about later.
    with open(MANIFEST, 'w', encoding='utf-8', newline='\n') as f:
        for p in sorted(_manifest):
            if os.path.isfile(os.path.join(ROOT, p.replace('/', os.sep))):
                f.write('%s *%s\n' % (_manifest[p], p))


def write(path, text, dry, force=False):
    import hashlib
    rel = os.path.relpath(path, ROOT).replace(chr(92), '/')
    blob = text.encode('latin-1', 'replace').replace(b'\n', b'\r\n')
    h = hashlib.sha256(blob).hexdigest()
    if dry:
        print('  would write %-58s %5d bytes' % (rel, len(blob)))
        return
    if not force and os.path.isfile(path) and rel in _manifest:
        cur = hashlib.sha256(open(path, 'rb').read()).hexdigest()
        if cur != _manifest[rel]:
            _kept.append(rel)
            return
    d = os.path.dirname(path)
    if not os.path.isdir(d):
        os.makedirs(d)
    with open(path, 'wb') as f:
        f.write(blob)
    _manifest[rel] = h


def main(argv):
    dry = '--dry' in argv
    force = '--force' in argv
    _load_manifest()
    scns = build()
    print('ORO scenarios: %d in %d folders' % (len(scns), len(FOLDERS)))

    write(os.path.join(HTM_DIR, 'index.htm'),
          html(ROOT_PAGE['title'], ROOT_PAGE['lead'], ROOT_PAGE['pic'], [], [],
               ROOT_PAGE['body']), dry, force)
    write(os.path.join(SCN_DIR, 'Description.txt'),
          'BEGIN_URLDESC\nORO' + BS + 'index\nEND_URLDESC\n\nBEGIN_DESC\n'
          + plain(ROOT_PAGE['lead'], [], [], ROOT_PAGE['body']) + '\nEND_DESC\n', dry, force)

    for f in FOLDERS:
        write(os.path.join(HTM_DIR, f['slug'] + '.htm'),
              html(f['title'], f['lead'], f['pic'], [], [], f['body']), dry, force)
        write(os.path.join(SCN_DIR, f['dir'], 'Description.txt'),
              'BEGIN_URLDESC\nORO' + BS + f['slug'] + '\nEND_URLDESC\n\nBEGIN_DESC\n'
              + plain(f['lead'], [], [], f['body']) + '\nEND_DESC\n', dry, force)

    npic = 0
    for s in scns:
        write(os.path.join(HTM_DIR, s['slug'] + '.htm'),
              html(s['title'], s['lead'], s['pic'], s['look'], s['setup']), dry, force)
        write(os.path.join(SCN_DIR, s['folder'], s['file'] + '.scn'), scn_text(s), dry, force)
        if picture(s['pic']):
            npic += 1
    ninst = install_pictures(dry)
    if not dry:
        _save_manifest()
    if _kept:
        print('\nKEPT - you edited these since they were generated, so they were NOT touched:')
        for k in _kept:
            print('   %s' % k)
        print('   (put the change in the table in this file to make it permanent,')
        print('    or re-run with --force to discard it)')
    print('pictures: %d installed to Images%sORO, %d of %d scenario pages carry one '
          '(a page with no picture ships text-only)' % (ninst, BS, npic, len(scns)))
    missing = [s['pic'] for s in scns if not picture(s['pic'])]
    if missing:
        print('  still to come: %s' % ', '.join(missing))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
