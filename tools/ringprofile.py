# =============================================================================
# ringprofile.py - ORO's planetary-ring radial profile: derive, verify, author.
#
# ORO renders rings from a normalised 1D profile per body: N x 1 RGBA, linear in
# RADIUS, RGB = brightness, ALPHA = encoded optical depth. This script is three
# things at once, which is why it exists before any C++:
#
#   1. THE REFERENCE IMPLEMENTATION of the derivation OroRings.cpp performs at
#      runtime. Port from here; the C++ must agree with this script's output.
#   2. THE OFFLINE VALIDATOR. `--report` prints the profile against the real ring
#      radii, so a body can be checked without launching anything. (The 2026-08-09
#      method: settle the format and the data offline, then write the C++.)
#   3. The OPTIONAL authoring tool. `--write` emits Textures\<Body>_ring_oro.dds,
#      the hand-refined override ORO prefers when present. Not required - the
#      runtime derivation stands on its own for any ringed planet.
#
# ----------------------------------------------------------------------------
# WHERE THE DATA COMES FROM, and it is already on disk for every ringed planet:
#
#   <Body>_ring_<size>.dds   8192 x 1 A8R8G8B8. Excellent brightness (8 km/texel
#                            on Saturn). Its ALPHA IS DEAD - all 255 - which is
#                            why the stock shader fakes opacity as red * 0.75.
#                            Saturn only, in the stock install.
#   <Body>_ring.tex          Three concatenated DDS surfaces (64/128/256 square),
#                            DXT3. Coarse, but its ALPHA IS LIVE: RingTechPS feeds
#                            it to SrcAlpha, so it is real, author-intended
#                            OPACITY. EVERY ringed planet ships one - it is the
#                            documented format - and for a fictional addon ring
#                            the author's own alpha is ground truth by definition.
#
# So brightness comes from the best source available and tau from the .tex alpha,
# and no planet needs anything authored by us.
#
# ----------------------------------------------------------------------------
# !! THE PROFILE IS LINEAR IN RADIUS, AND THE STOCK SHADER DISAGREES WITH ITSELF.
#
# RingTech2PS does `len = saturate(smoothstep(gTexOff.x, gTexOff.y, len))` and uses
# that as the texture coordinate - the CUBIC 3t^2-2t^3, not a linear ramp. But the
# shipped texture is authored LINEAR in radius. Measured 2026-09-11 on
# Saturn_ring_8192.dds: the B ring outer edge (117,507 km, the sharpest feature in
# the rings) sits where the LINEAR mapping predicts, texel 5368 - local peak
# gradient 18.0 against a whole-profile mean of 2.4 - and the smoothstep mapping
# predicts texel 5943, where the gradient is 1.6, i.e. a flat stretch. Four other
# features agree. The consequence is that stock displaces ring structure by up to
# 6,300 km, more than the width of the Cassini Division (4,543 km).
#
# This script therefore works in LINEAR radius throughout, and ORO's shader must
# sample linearly. Doing so also fixes the stock displacement for free.
# =============================================================================

import argparse, math, os, struct, sys

TAU_MAX = 5.0          # encode: a = 255 * sqrt(tau/TAU_MAX); decode: tau = 5*a*a
STOCK_NSECT = 16       # !! the .tex scan MUST use the mesh the texture was authored
                       # against (RingMgr::CreateRing, 8 + res*4 at the top LOD), not
                       # ORO's raised section count - cos(pi/nsect) shifts every band.

# Real radii, km. Used only to VERIFY a derived profile, never to build one.
FEATURES = {
    'Saturn': [('C ring inner', 74658), ('C/B boundary', 91975), ('B outer edge', 117507),
               ('Cassini Div outer', 122050), ('Encke gap', 133578), ('A ring outer', 136774),
               ('F ring', 140180)],
    'Uranus': [('ring 6', 41837), ('ring 5', 42235), ('ring 4', 42571), ('alpha', 44718),
               ('beta', 45661), ('eta', 47176), ('gamma', 47627), ('delta', 48300),
               ('epsilon', 51149)],
}


# ---------------------------------------------------------------- DDS decoding

def _dxt1_colours(cb, dxt1):
    c0, c1 = struct.unpack_from('<HH', cb, 0)
    def rgb(c):
        return (((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)
    e0, e1 = rgb(c0), rgb(c1)
    if dxt1 and c0 <= c1:
        return [e0, e1, tuple((e0[k] + e1[k]) // 2 for k in range(3)), (0, 0, 0)]
    return [e0, e1,
            tuple((2 * e0[k] + e1[k]) // 3 for k in range(3)),
            tuple((e0[k] + 2 * e1[k]) // 3 for k in range(3))]


def decode_dds(buf, off=0):
    """One DDS surface at buf[off:] -> (w, h, rgba list of (r,g,b,a), bytes consumed).
    Handles A8R8G8B8/X8R8G8B8 and DXT1/3/5 - the same set OroParticles.cpp's
    LoadDDSRGBA handles, which is what the C++ will reuse."""
    if buf[off:off + 4] != b'DDS ':
        raise ValueError('not a DDS at offset %d' % off)
    h, w = struct.unpack_from('<II', buf, off + 12)
    pfflags, fourcc, bits = struct.unpack_from('<III', buf, off + 80)
    amask = struct.unpack_from('<I', buf, off + 104)[0]
    data = off + 128
    px = [(0, 0, 0, 255)] * (w * h)
    if pfflags & 0x4:                                     # DDPF_FOURCC
        dxt1 = fourcc == 0x31545844
        dxt3 = fourcc == 0x33545844
        dxt5 = fourcc == 0x35545844
        if not (dxt1 or dxt3 or dxt5):
            raise ValueError('unsupported FourCC 0x%08X' % fourcc)
        bsz = 8 if dxt1 else 16
        bw, bh = (w + 3) // 4, (h + 3) // 4
        for byi in range(bh):
            for bxi in range(bw):
                blk = buf[data + (byi * bw + bxi) * bsz:][:bsz]
                cb = blk if dxt1 else blk[8:]
                pal = _dxt1_colours(cb, dxt1)
                cidx = struct.unpack_from('<I', cb, 4)[0]
                a5 = None
                if dxt5:
                    a0, a1 = blk[0], blk[1]
                    a5 = [a0, a1]
                    if a0 > a1:
                        a5 += [((6 - k) * a0 + (k + 1) * a1) // 7 for k in range(6)]
                    else:
                        a5 += [((4 - k) * a0 + (k + 1) * a1) // 5 for k in range(4)] + [0, 255]
                    aidx = int.from_bytes(blk[2:8], 'little')
                for i in range(16):
                    x, y = bxi * 4 + (i % 4), byi * 4 + (i // 4)
                    if x >= w or y >= h:
                        continue
                    c = pal[(cidx >> (2 * i)) & 3]
                    if dxt3:
                        a = ((blk[i >> 1] >> ((i & 1) * 4)) & 0xF) * 17
                    elif dxt5:
                        a = a5[(aidx >> (3 * i)) & 7]
                    else:
                        a = 255
                    px[y * w + x] = (c[0], c[1], c[2], a)
        used = 128 + bw * bh * bsz
    elif (pfflags & 0x40) and bits == 32:                 # DDPF_RGB, B,G,R,A in memory
        for i in range(w * h):
            b, g, r, a = buf[data + i * 4:data + i * 4 + 4]
            px[i] = (r, g, b, a if amask else 255)
        used = 128 + w * h * 4
    else:
        raise ValueError('unsupported pixel format (flags 0x%X, %d bpp)' % (pfflags, bits))
    return w, h, px, used


def load_tex(path):
    """A .tex is a CONCATENATION of DDS surfaces (LoadPlanetTextures, D3D9Util.cpp:1565).
    Return the LARGEST - LoadDDSRGBA would take the first, which is the 64x64."""
    buf = open(path, 'rb').read()
    off, best = 0, None
    while off + 128 <= len(buf) and buf[off:off + 4] == b'DDS ':
        w, h, px, used = decode_dds(buf, off)
        if best is None or w * h > best[0] * best[1]:
            best = (w, h, px)
        off += used
    if best is None:
        raise ValueError('no DDS surfaces in %s' % path)
    return best


# ------------------------------------------------------------- profile sources

def resample(src, n):
    """Linear resample of a value list onto n samples, endpoints preserved."""
    m = len(src)
    if m == n:
        return list(src)
    out = []
    for i in range(n):
        t = i * (m - 1) / (n - 1.0)
        j = min(int(t), m - 2)
        f = t - j
        out.append(src[j] * (1 - f) + src[j + 1] * f)
    return out


def bright_from_1d(px, w):
    """The hi-res profile is already linear in radius, one row."""
    return [(px[i][0] + px[i][1] + px[i][2]) / 3.0 for i in range(w)]


def scan_tex(px, w, h, irad, orad):
    """Radial scan of the legacy 2D ring texture -> (bright, alpha) linear in radius.

    RingMgr::CreateRing maps the OUTER node (radius nrad) to tv=0 and the INNER node
    (radius ir) to tv=1, with tu running fo..1-fo along the arc. At u = 0.5 the quad's
    bilinear interpolation sits at the ARC MIDPOINT, whose radius is nrad*cos(alpha) =
    orad at v=0 and ir*cos(alpha) at v=1. So the centre column is a clean radial scan:
        r(v) = orad - v * (orad - irad*cos(alpha))
    Validated on both stock bodies: it puts Saturn's Cassini Division and B ring core,
    and Uranus's nine narrow rings, at their real radii to within 1-2%.
    """
    alpha = math.pi / STOCK_NSECT
    r_at_v1 = irad * math.cos(alpha)
    mid = w // 2
    rs, bs, als = [], [], []
    for row in range(h):
        v = row / (h - 1.0)
        rs.append(orad - v * (orad - r_at_v1))
        r, g, b, a = px[row * w + mid]
        bs.append((r + g + b) / 3.0)
        als.append(a)
    # the scan runs outer -> inner; flip so index 0 is the INNER edge, then resample
    # onto a uniform radius grid spanning [irad, orad].
    rs, bs, als = rs[::-1], bs[::-1], als[::-1]

    def at(rad):
        if rad <= rs[0]:  return bs[0], als[0]
        if rad >= rs[-1]: return bs[-1], als[-1]
        lo = 0
        while lo + 1 < len(rs) and rs[lo + 1] < rad:
            lo += 1
        f = (rad - rs[lo]) / max(1e-9, rs[lo + 1] - rs[lo])
        return (bs[lo] * (1 - f) + bs[lo + 1] * f, als[lo] * (1 - f) + als[lo + 1] * f)

    n = h
    out_b, out_a = [], []
    for i in range(n):
        rad = irad + (orad - irad) * i / (n - 1.0)
        b, a = at(rad)
        out_b.append(b)
        out_a.append(a)
    return out_b, out_a


def tau_from_alpha(a):
    """The legacy alpha IS opacity at normal incidence: tau = -ln(1 - a)."""
    f = min(0.999, max(0.0, a / 255.0))
    return max(0.0, min(TAU_MAX, -math.log(1.0 - f)))    # max() kills -0.0 at a=0


def gate_tau(bright, tau):
    """!! THE TWO SOURCES DISAGREE ABOUT WHERE THE EDGES ARE, and left alone that is
    visible. Brightness comes from an 8192-texel source and tau from a 256-texel one,
    so Saturn's Encke gap arrives BLACK (the fine source resolves its 325 km) while
    still carrying tau 0.42 (the coarse source cannot) - a gap you can see through
    that nonetheless shadows the planet. Same at the C ring's inner edge and the A
    ring's outer edge.

    The rule: the COARSE source sets the LEVEL, the FINE source sets the EDGES. Where
    there is no material there is no optical depth, and brightness is the higher-
    resolution truth about where the material is.

    !! NORMALISED AGAINST THE PROFILE'S OWN MAXIMUM, never an absolute. A dark ring
    that is optically thick - Uranus, albedo 0.03 - must not be gated away; that is
    the exact failure mode this is not allowed to have. Referencing the ring's own
    brightest point makes the gate inert unless a texel is far darker than the rest
    of THAT ring: measured, it changes nothing on Uranus and clears Saturn's gaps.
    """
    bmax = max(bright) or 1.0
    bref = 0.20 * bmax
    for i in range(len(tau)):
        g = min(1.0, bright[i] / bref) if bref > 0 else 1.0
        tau[i] *= g


def tau_from_bright(b, bmax):
    """LAST RESORT - only when no alpha-bearing source exists. A lit ring's brightness
    saturates with optical depth, so invert that; it cannot tell a dark thick ring from
    a bright thin one, which is why it is the fallback and not the method."""
    f = 0.98 * (b / bmax if bmax > 0 else 0.0)
    return min(TAU_MAX, -math.log(max(1e-3, 1.0 - f)))


# ----------------------------------------------------------------- the derivation

def read_cfg(root, body):
    path = os.path.join(root, 'Config', body + '.cfg')
    size = irad = orad = None
    for line in open(path, 'r', errors='replace'):
        line = line.split(';')[0]
        if '=' not in line:
            continue
        k, v = [s.strip() for s in line.split('=', 1)]
        try:
            if   k.lower() == 'size':          size = float(v)
            elif k.lower() == 'ringminradius': irad = float(v)
            elif k.lower() == 'ringmaxradius': orad = float(v)
        except ValueError:
            pass
    if size is None or irad is None or orad is None:
        raise SystemExit('%s: not a ringed planet (needs Size, RingMinRadius, RingMaxRadius)' % body)
    return size / 1000.0, irad, orad            # planet radius in km


def derive(root, body, n=None, use_override=False):
    """The runtime derivation, exactly as OroRingProfile.cpp does it."""
    Rkm, irad, orad = read_cfg(root, body)
    tex_dir = os.path.join(root, 'Textures')
    src = {'brightness': None, 'tau': None}

    # 1. THE OVERRIDE - ORO's own format; the file IS the profile. Off by default here
    #    because this tool is what WRITES that file, and re-reading its own output would
    #    hide a change in the sources. The C++ honours it by default.
    if use_override:
        p = os.path.join(tex_dir, '%s_ring_oro.dds' % body)
        if os.path.exists(p):
            w, h, px, _ = decode_dds(open(p, 'rb').read())
            b = [(px[i][0] + px[i][1] + px[i][2]) / 3.0 for i in range(w)]
            t = [TAU_MAX * (px[i][3] / 255.0) ** 2 for i in range(w)]      # decode tau = 5*a*a
            nn = n if n else w
            return dict(body=body, Rkm=Rkm, irad=irad, orad=orad, n=nn,
                        bright=resample(b, nn), tau=resample(t, nn),
                        src={'brightness': os.path.basename(p), 'tau': os.path.basename(p)},
                        override=True)

    hi = None
    for size in (8192, 4096, 2048):
        p = os.path.join(tex_dir, '%s_ring_%d.dds' % (body, size))
        if os.path.exists(p):
            w, h, px, _ = decode_dds(open(p, 'rb').read())
            hi = (w, px)
            src['brightness'] = os.path.basename(p)
            break

    legacy = None
    p = os.path.join(tex_dir, '%s_ring.tex' % body)
    if os.path.exists(p):
        w, h, px = load_tex(p)
        legacy = scan_tex(px, w, h, irad, orad)
        src['tau'] = os.path.basename(p)

    if n is None:
        n = 8192 if hi else 1024

    if hi:
        bright = resample(bright_from_1d(hi[1], hi[0]), n)
    elif legacy:
        bright = resample(legacy[0], n)
        src['brightness'] = os.path.basename(p)
    else:
        bright = [180.0] * n                     # no texture at all - neutral ring
        src['brightness'] = '(synthesised)'

    if legacy:
        tau = [tau_from_alpha(a) for a in resample(legacy[1], n)]
        gate_tau(bright, tau)
    else:
        bmax = max(bright) or 1.0
        tau = [tau_from_bright(b, bmax) for b in bright]
        src['tau'] = '(inverted from brightness - no alpha source)'

    return dict(body=body, Rkm=Rkm, irad=irad, orad=orad, n=n,
                bright=bright, tau=tau, src=src, override=False)


# --------------------------------------------------------------------- output

def radius_km(p, i):
    return p['Rkm'] * (p['irad'] + (p['orad'] - p['irad']) * i / (p['n'] - 1.0))


def texel_at(p, rkm):
    t = (rkm / p['Rkm'] - p['irad']) / (p['orad'] - p['irad'])
    return int(t * (p['n'] - 1) + 0.5)                  # matches OroRingProfile_Texel


def report(p):
    print('== %s ==  R %.0f km, rings %.3f..%.3f Rp = %.0f..%.0f km, %d texels (%.1f km each)'
          % (p['body'], p['Rkm'], p['irad'], p['orad'],
             p['Rkm'] * p['irad'], p['Rkm'] * p['orad'], p['n'],
             p['Rkm'] * (p['orad'] - p['irad']) / (p['n'] - 1.0)))
    print('   brightness from : %s' % p['src']['brightness'])
    print('   optical depth   : %s' % p['src']['tau'])
    print()
    print('   %-20s %10s %8s %7s %7s' % ('known feature', 'radius km', 'texel', 'bright', 'tau'))
    for name, rkm in FEATURES.get(p['body'], []):
        i = texel_at(p, rkm)
        if 0 <= i < p['n']:
            print('   %-20s %10d %8d %7.0f %7.2f' % (name, rkm, i, p['bright'][i], p['tau'][i]))
        else:
            print('   %-20s %10d %8s %7s %7s' % (name, rkm, 'outside', '-', '-'))
    print()
    print('   profile (inner -> outer):')
    ramp = ' .:-=+*#%@'
    rows = 24
    for k in range(rows):
        i = k * (p['n'] - 1) // (rows - 1)
        b, t = p['bright'][i], p['tau'][i]
        print('     %9.0f km  b|%s| %3.0f   tau|%s| %5.2f'
              % (radius_km(p, i),
                 ramp[min(9, int(b * 10 / 256))] * 3, b,
                 ramp[min(9, int(t / TAU_MAX * 10))] * 3, t))


def write_dds(path, p):
    """A8R8G8B8, N x 1, full mip chain down to 1 texel.
    !! THE WHOLE CHAIN MATTERS. The client loads with D3DFMT_FROM_FILE and uses the
    file's own mips; ship a chain whose lower levels are stale and the ring changes
    appearance with distance instead of just softening."""
    n = p['n']
    lvl = []
    rgba = []
    # int(x + 0.5), not round(): Python's round() is banker's rounding and the C++ is
    # (int)(x + 0.5). Same rule in both so the encoded bytes diff to zero.
    for i in range(n):
        b = max(0, min(255, int(p['bright'][i] + 0.5)))
        a = max(0, min(255, int(255.0 * math.sqrt(max(0.0, p['tau'][i]) / TAU_MAX) + 0.5)))
        rgba.append((b, b, b, a))
    lvl.append(rgba)
    while len(lvl[-1]) > 1:                       # box filter in the ENCODED domain
        s = lvl[-1]
        lvl.append([tuple((s[2 * i][c] + s[2 * i + 1][c]) // 2 for c in range(4))
                    for i in range(len(s) // 2)])

    flags = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | 0x20000        # CAPS HEIGHT WIDTH PITCH PF MIPS
    caps = 0x1000 | 0x8 | 0x400000                          # TEXTURE COMPLEX MIPMAP
    hdr = bytearray(128)
    hdr[0:4] = b'DDS '
    struct.pack_into('<7I', hdr, 4, 124, flags, 1, n, n * 4, 0, len(lvl))
    struct.pack_into('<2I', hdr, 76, 32, 0x41)              # pf size, ALPHAPIXELS|RGB
    struct.pack_into('<6I', hdr, 84, 0, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    struct.pack_into('<I', hdr, 108, caps)
    body = bytearray()
    for s in lvl:
        for (r, g, b, a) in s:
            body += bytes((b, g, r, a))                     # B,G,R,A in memory
    open(path, 'wb').write(bytes(hdr) + bytes(body))

    w2, h2, px2, _ = decode_dds(open(path, 'rb').read())     # read it back - prove it
    assert (w2, h2) == (n, 1), 'readback size %dx%d' % (w2, h2)
    assert px2[0][3] == rgba[0][3] and px2[-1][3] == rgba[-1][3], 'alpha did not round-trip'
    print('   wrote %s  (%d texels, %d mips, %d bytes) - readback OK'
          % (os.path.basename(path), n, len(lvl), 128 + len(body)))


def dump(path, p):
    """The format tools/ringprofile_test writes, so the two can be diffed."""
    with open(path, 'w') as f:
        f.write('# body=%s n=%d Rkm=%.6f irad=%.9f orad=%.9f override=%d srcB=%s srcT=%s\n'
                % (p['body'], p['n'], p['Rkm'], p['irad'], p['orad'], 1 if p['override'] else 0,
                   p['src']['brightness'], p['src']['tau']))
        for i in range(p['n']):
            b = max(0, min(255, int(p['bright'][i] + 0.5)))
            a = max(0, min(255, int(255.0 * math.sqrt(max(0.0, p['tau'][i]) / TAU_MAX) + 0.5)))
            f.write('%d %.9f %.9f %08X\n' % (i, p['bright'][i], p['tau'][i],
                                             (a << 24) | (b << 16) | (b << 8) | b))


def compare(path, p):
    """Diff a C++ dump against THIS derivation. The C++ is a port of this file; any
    disagreement is a transcription error, and this is where it is caught rather than
    in his sim. Refuses if the dump was built for different radii."""
    lines = open(path).read().splitlines()
    hdr = dict(kv.split('=', 1) for kv in lines[0][2:].split(' ') if '=' in kv)
    for k, v in (('Rkm', p['Rkm']), ('irad', p['irad']), ('orad', p['orad'])):
        if abs(float(hdr[k]) - v) > 1e-6:
            raise SystemExit('%s: dump %s=%s but cfg says %s - not the same derivation'
                             % (p['body'], k, hdr[k], v))
    if int(hdr['n']) != p['n']:
        raise SystemExit('%s: dump has %s texels, python %d' % (p['body'], hdr['n'], p['n']))
    db = dt = 0.0
    rgba_bad = 0
    for i, line in enumerate(lines[1:]):
        j, b, t, rgba = line.split()
        db = max(db, abs(float(b) - p['bright'][i]))
        dt = max(dt, abs(float(t) - p['tau'][i]))
        bb = max(0, min(255, int(p['bright'][i] + 0.5)))
        aa = max(0, min(255, int(255.0 * math.sqrt(max(0.0, p['tau'][i]) / TAU_MAX) + 0.5)))
        if int(rgba, 16) != ((aa << 24) | (bb << 16) | (bb << 8) | bb):
            rgba_bad += 1
    ok = db < 1e-6 and dt < 1e-6 and rgba_bad == 0
    print('   compare vs %s: %d texels, max |d bright| %.2e, max |d tau| %.2e, rgba mismatches %d  -> %s'
          % (os.path.basename(path), p['n'], db, dt, rgba_bad, 'AGREE' if ok else 'DISAGREE'))
    print('   sources  cpp: %s / %s   py: %s / %s' % (hdr.get('srcB'), hdr.get('srcT'),
                                                    p['src']['brightness'], p['src']['tau']))
    if not ok:
        raise SystemExit(1)


def main():
    ap = argparse.ArgumentParser(description="ORO ring radial profile: derive, verify, author.")
    ap.add_argument('body', nargs='+', help='planet name(s), e.g. Saturn Uranus')
    ap.add_argument('--orbiter', default=r'Z:\Orbiter-2024', help='Orbiter root')
    ap.add_argument('--n', type=int, default=None, help='profile texels (default: source-driven)')
    ap.add_argument('--override', action='store_true', help='honour an existing _ring_oro.dds (the C++ default)')
    ap.add_argument('--write', action='store_true', help='write Textures\\<Body>_ring_oro.dds')
    ap.add_argument('--dump', metavar='FILE', help='write the profile in ringprofile_test format')
    ap.add_argument('--compare', metavar='FILE', help='diff against a ringprofile_test dump; exit 1 on disagreement')
    ap.add_argument('--quiet', action='store_true', help='skip the report')
    a = ap.parse_args()
    for body in a.body:
        p = derive(a.orbiter, body, a.n, a.override)
        if not a.quiet:
            report(p)
        if a.write:
            write_dds(os.path.join(a.orbiter, 'Textures', '%s_ring_oro.dds' % body), p)
        if a.dump:
            dump(a.dump, p)
        if a.compare:
            compare(a.compare, p)
        print()


if __name__ == '__main__':
    main()
