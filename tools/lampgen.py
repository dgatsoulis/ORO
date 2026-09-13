# lampgen.py - the two lamp vessels of ORO's lights test rig (Script\testlights.lua)
#
# Writes, into an Orbiter 2024 tree:
#   Meshes\ORO\lamp_spot.msh      a 6 m post with a floodlight head (the SPOT lamp)
#   Meshes\ORO\lamp_post.msh      a 10 m mast with a globe (the POINT lamp)
#   Config\Vessels\ORO_LampSpot.cfg / ORO_LampPost.cfg   config-only vessel classes (no DLL)
#
# The lit glass is an EMISSIVE material in the mesh and the light itself is an emitter the
# script adds at runtime - the lighting rule from the local-lights arc (no glare sprite).
# Each lamp is its own vessel so every OTHER lamp is a caster in its map; the emitter's own
# vessel is excluded by the client. The vessel origin sits 0.5 m above the base plate and
# the three touchdown points are the plate, so a landed status with arot.x > 4 puts the
# plate on the ground (Vesselstatus.cpp: the "derive it" branch of InitLanded).
#
# Orbiter meshes are left-handed (x right, y up, z forward); a triangle is front-facing
# when cross(b-a, c-a) points along its outward normal - every face here is emitted that
# way, checked, and flat-shaded (one normal per face, vertices duplicated per face).
#
#   python tools\lampgen.py [OrbiterRoot]      default root Z:\Orbiter-2024
#
# ASCII only. Regenerate after editing; the outputs are what ship.
import math, os, sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else r"Z:\Orbiter-2024"


class Mesh(object):
    def __init__(self):
        self.groups = []
        self.materials = []

    def material(self, name, diff, amb, spec, power, emis):
        self.materials.append((name, diff, amb, spec, power, emis))
        return len(self.materials)                       # 1-based, as the file wants it

    def group(self, label, mat):
        g = {"label": label, "mat": mat, "vtx": [], "tri": []}
        self.groups.append(g)
        return g

    def write(self, path):
        lines = ["MSHX1", "GROUPS %d" % len(self.groups)]
        for g in self.groups:
            lines.append("LABEL %s" % g["label"])
            lines.append("MATERIAL %d" % g["mat"])
            lines.append("TEXTURE 0")
            lines.append("GEOM %d %d" % (len(g["vtx"]), len(g["tri"])))
            for v in g["vtx"]:
                lines.append("%.4f %.4f %.4f %.4f %.4f %.4f %.3f %.3f" % v)
            for t in g["tri"]:
                lines.append("%d %d %d" % t)
        lines.append("MATERIALS %d" % len(self.materials))
        for m in self.materials:
            lines.append(m[0])
        for name, diff, amb, spec, power, emis in self.materials:
            lines.append("MATERIAL %s" % name)
            lines.append("%.3f %.3f %.3f %.3f" % diff)
            lines.append("%.3f %.3f %.3f %.3f" % amb)
            lines.append("%.3f %.3f %.3f %.3f %.3f" % (spec + (power,)))
            lines.append("%.3f %.3f %.3f %.3f" % emis)
        lines.append("TEXTURES 0")
        data = ("\r\n".join(lines) + "\r\n").encode("ascii")
        with open(path, "wb") as f:
            f.write(data)
        nv = sum(len(g["vtx"]) for g in self.groups)
        nt = sum(len(g["tri"]) for g in self.groups)
        print("  %s: %d groups, %d vertices, %d triangles" % (os.path.basename(path), len(self.groups), nv, nt))


# ---- geometry helpers ------------------------------------------------------------------

def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def cross(a, b): return (a[1]*b[2] - a[2]*b[1], a[2]*b[0] - a[0]*b[2], a[0]*b[1] - a[1]*b[0])
def dot(a, b): return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]
def norm(a):
    l = math.sqrt(dot(a, a)) or 1.0
    return (a[0]/l, a[1]/l, a[2]/l)


def add_tri(g, a, b, c, n, uv=((0, 0), (1, 0), (1, 1))):
    """One flat triangle with outward normal n; winding fixed to the D3D front-face rule."""
    if dot(cross(sub(b, a), sub(c, a)), n) < 0:
        b, c = c, b
        uv = (uv[0], uv[2], uv[1])
    base = len(g["vtx"])
    for p, t in zip((a, b, c), uv):
        g["vtx"].append((p[0], p[1], p[2], n[0], n[1], n[2], t[0], t[1]))
    g["tri"].append((base, base + 1, base + 2))


def add_quad(g, p0, p1, p2, p3, n=None):
    """A flat quad p0..p3 (any consistent order round the perimeter)."""
    if n is None:
        n = norm(cross(sub(p1, p0), sub(p3, p0)))
    if dot(cross(sub(p1, p0), sub(p2, p0)), n) < 0:
        p1, p3 = p3, p1
    base = len(g["vtx"])
    for p, t in zip((p0, p1, p2, p3), ((0, 0), (1, 0), (1, 1), (0, 1))):
        g["vtx"].append((p[0], p[1], p[2], n[0], n[1], n[2], t[0], t[1]))
    g["tri"].append((base, base + 1, base + 2))
    g["tri"].append((base, base + 2, base + 3))


def prism(g, y0, y1, r, nseg=8, cap_top=True, cap_bottom=False):
    """A vertical n-gon prism about the y axis, flat-shaded sides, optional caps."""
    for i in range(nseg):
        a0 = 2 * math.pi * i / nseg
        a1 = 2 * math.pi * (i + 1) / nseg
        am = 0.5 * (a0 + a1)
        p00 = (r * math.cos(a0), y0, r * math.sin(a0))
        p01 = (r * math.cos(a1), y0, r * math.sin(a1))
        p10 = (r * math.cos(a0), y1, r * math.sin(a0))
        p11 = (r * math.cos(a1), y1, r * math.sin(a1))
        add_quad(g, p00, p01, p11, p10, (math.cos(am), 0.0, math.sin(am)))
    if cap_top:
        c = (0.0, y1, 0.0)
        for i in range(nseg):
            a0 = 2 * math.pi * i / nseg
            a1 = 2 * math.pi * (i + 1) / nseg
            add_tri(g, c, (r * math.cos(a0), y1, r * math.sin(a0)), (r * math.cos(a1), y1, r * math.sin(a1)), (0, 1, 0))
    if cap_bottom:
        c = (0.0, y0, 0.0)
        for i in range(nseg):
            a0 = 2 * math.pi * i / nseg
            a1 = 2 * math.pi * (i + 1) / nseg
            add_tri(g, c, (r * math.cos(a0), y0, r * math.sin(a0)), (r * math.cos(a1), y0, r * math.sin(a1)), (0, -1, 0))


def rot_x(p, t):
    """Rotate about the x axis: positive t tilts +z DOWN (the floodlight's pitch)."""
    y = p[1] * math.cos(t) - p[2] * math.sin(t)
    z = p[1] * math.sin(t) + p[2] * math.cos(t)
    return (p[0], y, z)


def box(g, centre, size, tilt=0.0, faces=("+x", "-x", "+y", "-y", "+z", "-z")):
    """An axis-aligned box, pitched by tilt about x and moved to centre; faces by name."""
    hx, hy, hz = size[0] * 0.5, size[1] * 0.5, size[2] * 0.5
    def P(x, y, z):
        q = rot_x((x, y, z), tilt)
        return (q[0] + centre[0], q[1] + centre[1], q[2] + centre[2])
    defs = {
        "+x": ((hx, -hy, -hz), (hx, hy, -hz), (hx, hy, hz), (hx, -hy, hz), (1, 0, 0)),
        "-x": ((-hx, -hy, -hz), (-hx, -hy, hz), (-hx, hy, hz), (-hx, hy, -hz), (-1, 0, 0)),
        "+y": ((-hx, hy, -hz), (-hx, hy, hz), (hx, hy, hz), (hx, hy, -hz), (0, 1, 0)),
        "-y": ((-hx, -hy, -hz), (hx, -hy, -hz), (hx, -hy, hz), (-hx, -hy, hz), (0, -1, 0)),
        "+z": ((-hx, -hy, hz), (hx, -hy, hz), (hx, hy, hz), (-hx, hy, hz), (0, 0, 1)),
        "-z": ((-hx, -hy, -hz), (-hx, hy, -hz), (hx, hy, -hz), (hx, -hy, -hz), (0, 0, -1)),
    }
    for f in faces:
        a, b, c, d, n = defs[f]
        n = rot_x(n, tilt)
        add_quad(g, P(*a), P(*b), P(*c), P(*d), n)


def sphere(g, centre, r, nlat=8, nlon=16):
    """A sphere with smooth normals (the globe): one vertex per lat/lon node."""
    base = len(g["vtx"])
    for i in range(nlat + 1):
        th = math.pi * i / nlat                          # 0 = top
        for j in range(nlon + 1):
            ph = 2 * math.pi * j / nlon
            n = (math.sin(th) * math.cos(ph), math.cos(th), math.sin(th) * math.sin(ph))
            g["vtx"].append((centre[0] + r * n[0], centre[1] + r * n[1], centre[2] + r * n[2],
                             n[0], n[1], n[2], float(j) / nlon, float(i) / nlat))
    for i in range(nlat):
        for j in range(nlon):
            a = base + i * (nlon + 1) + j
            b = a + 1
            c = a + (nlon + 1)
            d = c + 1
            # outward = the vertex normal at a; pick the winding that faces it
            pa = g["vtx"][a][:3]; pb = g["vtx"][b][:3]; pc = g["vtx"][c][:3]
            na = g["vtx"][a][3:6]
            if dot(cross(sub(pb, pa), sub(pc, pa)), na) >= 0:
                g["tri"].append((a, b, c)); g["tri"].append((b, d, c))
            else:
                g["tri"].append((a, c, b)); g["tri"].append((b, c, d))


# ---- the two lamps ---------------------------------------------------------------------

BASE_Y = -0.5            # the base plate; the touchdown points sit here
SPOT_HEAD_Y = 12.0       # floodlight housing centre - a 12 m post: from 38 m the light meets a
                         # DG at 15 deg and its shadow lands compact and dark (at 5.3 m it grazed
                         # the hull at 8 deg and the upper hull's shadow never reached the ground)
SPOT_HEAD_Z = 0.25
SPOT_TILT = 30.0 * math.pi / 180.0
GLOBE_Y = 9.85           # point lamp's globe centre = the emitter position

def materials(m):
    m.material("lamp_base", (0.22, 0.22, 0.24, 1.0), (0.22, 0.22, 0.24, 1.0), (0.15, 0.15, 0.15, 1.0), 8.0, (0, 0, 0, 1.0))
    m.material("lamp_steel", (0.58, 0.60, 0.63, 1.0), (0.58, 0.60, 0.63, 1.0), (0.45, 0.45, 0.45, 1.0), 24.0, (0, 0, 0, 1.0))
    # the lit glass: emissive at 1.0 = full brightness through the clamp (patch (r) passes
    # anything above 1.0 to the bloom, which a night rig does not need)
    m.material("lamp_lit", (1.0, 1.0, 1.0, 1.0), (0.2, 0.2, 0.2, 1.0), (0, 0, 0, 1.0), 0.0, (1.0, 0.96, 0.86, 1.0))


def lamp_spot():
    m = Mesh(); materials(m)
    g = m.group("BASE", 1)
    prism(g, BASE_Y, BASE_Y + 0.15, 0.60, 8, cap_top=True, cap_bottom=True)
    g = m.group("POLE", 2)
    prism(g, BASE_Y + 0.15, SPOT_HEAD_Y - 0.14, 0.10, 8, cap_top=True)
    # the head: a housing pitched 30 deg down, its front face left open for the glass group
    g = m.group("HEAD", 2)
    box(g, (0.0, SPOT_HEAD_Y, SPOT_HEAD_Z), (0.50, 0.28, 0.45), SPOT_TILT, faces=("+x", "-x", "+y", "-y", "-z"))
    # a short bracket from the pole top to the housing
    box(g, (0.0, SPOT_HEAD_Y - 0.10, 0.06), (0.12, 0.12, 0.24), 0.0)
    # the glass: the housing's front face, 1 cm proud, emissive
    g = m.group("GLASS", 3)
    box(g, (0.0, SPOT_HEAD_Y, SPOT_HEAD_Z), (0.46, 0.24, 0.47), SPOT_TILT, faces=("+z",))
    return m


def lamp_post():
    m = Mesh(); materials(m)
    g = m.group("BASE", 1)
    prism(g, BASE_Y, BASE_Y + 0.15, 0.70, 8, cap_top=True, cap_bottom=True)
    g = m.group("MAST", 2)
    prism(g, BASE_Y + 0.15, GLOBE_Y - 0.30, 0.12, 8, cap_top=True)
    g = m.group("GLOBE", 3)
    sphere(g, (0.0, GLOBE_Y, 0.0), 0.38, 8, 16)
    return m


CFG_SPOT = """; === ORO lights test rig - floodlight post (a SPOT light lamp) ===
; Config-only vessel, no module. GENERATED by tools/lampgen.py in the ORO repo -
; edit the generator, not this file. Spawned and placed by Script\\testlights.lua,
; which adds the light emitter at the head (0, 11.89, 0.45) at run time.
Meshname = ORO\\lamp_spot
Size = 12.5
Mass = 700
Inertia = 12.0 0.03 12.0
EnableFocus = FALSE
TouchdownPoints = 0 -0.5 0.55  -0.48 -0.5 -0.28  0.48 -0.5 -0.28
"""

CFG_POST = """; === ORO lights test rig - lamp mast with a globe (a POINT light lamp) ===
; Config-only vessel, no module. GENERATED by tools/lampgen.py in the ORO repo -
; edit the generator, not this file. Spawned and placed by Script\\testlights.lua,
; which adds the light emitter at the globe (0, 9.85, 0) at run time.
Meshname = ORO\\lamp_post
Size = 10.5
Mass = 600
Inertia = 8.5 0.03 8.5
EnableFocus = FALSE
TouchdownPoints = 0 -0.5 0.62  -0.54 -0.5 -0.31  0.54 -0.5 -0.31
"""


def write_text(path, text):
    with open(path, "wb") as f:
        f.write(text.replace("\n", "\r\n").encode("ascii"))
    print("  %s" % os.path.relpath(path, ROOT))


if __name__ == "__main__":
    meshdir = os.path.join(ROOT, "Meshes", "ORO")
    cfgdir = os.path.join(ROOT, "Config", "Vessels")
    for d in (meshdir, cfgdir):
        if not os.path.isdir(d):
            raise SystemExit("missing folder: " + d)
    print("lampgen -> " + ROOT)
    lamp_spot().write(os.path.join(meshdir, "lamp_spot.msh"))
    lamp_post().write(os.path.join(meshdir, "lamp_post.msh"))
    write_text(os.path.join(cfgdir, "ORO_LampSpot.cfg"), CFG_SPOT)
    write_text(os.path.join(cfgdir, "ORO_LampPost.cfg"), CFG_POST)
    print("done")
