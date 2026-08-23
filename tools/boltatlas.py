# boltatlas.py - bakes Textures\OROolt_atlas.dds for the rain lightning
# (the bellgen.cpp pattern: the SHIPPED asset is derived; this is its source).
#
# Input: the "Resource Boy - Lightning Bolt Textures" pack (100 RGBA PNGs),
# downloaded by the author into Textures\ORO\. Its licence permits modified
# inclusion in applications; it PROHIBITS redistributing the raw files - so the
# pack itself must NEVER enter the repo or a release zip. Only the derived
# atlas ships. https://resourceboy.com/
#
# What it does: measures all 100 bolts (aspect + fill), picks 16 for variety
# (thin singles -> branched -> wide forked), crops each to its alpha bbox,
# BOTTOM-aligns it in a 256x1024 slot (the channel's lowest lit pixel = the
# quad's ground edge, which is what guarantees ground contact), premultiplies
# alpha into RGB (transparent -> BLACK: free under additive draw), bakes a
# two-radius glow (the per-filament halo), and writes an uncompressed
# A8R8G8B8 DDS, 2048x2048, 8x2 slots. Requires PIL.
import os, struct
from PIL import Image, ImageFilter, ImageChops

SRC = r"Z:\Orbiter-2024\Textures\ORO\Resource Boy - Lightning Bolt Textures"
OUT = r"Z:\Orbiter-2024\Textures\ORO\bolt_atlas.dds"

# --- measure every bolt: alpha bounding box, aspect, fill density ---
stats = []
for i in range(1, 101):
    p = os.path.join(SRC, "%02d.png" % i if i < 100 else "100.png")
    if not os.path.exists(p):
        continue
    im = Image.open(p)
    im.draft(None, (im.width // 4, im.height // 4))   # fast quarter-res decode ok for metrics
    im = im.convert("RGBA")
    a = im.getchannel("A")
    bbox = a.getbbox()
    if not bbox:
        continue
    w = bbox[2] - bbox[0]; h = bbox[3] - bbox[1]
    crop = a.crop(bbox)
    hist = crop.histogram()
    lit = sum(hist[32:])                 # pixels with meaningful alpha
    fill = lit / float(w * h)
    stats.append((i, h / float(w), fill))

tall_thin  = sorted([s for s in stats if s[1] > 3.0 and s[2] < 0.10], key=lambda s: s[2])
branched   = sorted([s for s in stats if 1.6 < s[1] <= 3.0 or (s[1] > 3.0 and s[2] >= 0.10)], key=lambda s: -s[2])
wide       = sorted([s for s in stats if s[1] <= 1.6], key=lambda s: -s[2])

def spread(lst, n):
    if len(lst) <= n:
        return lst
    step = len(lst) / float(n)
    return [lst[int(k * step)] for k in range(n)]

pick = spread(tall_thin, 10) + spread(branched, 4) + spread(wide, 2)
pick = pick[:16]
while len(pick) < 16:                    # top up from whatever category has spares
    pool = [s for s in stats if s not in pick]
    pick.append(pool[0])
print("picked:", [p[0] for p in pick])
for p in pick:
    print("  %3d.png  aspect %.2f  fill %.3f" % p)

# --- bake: 2048x2048, 8x2 slots of 256x1024, premultiplied onto black ---
SW, SH = 256, 1024
atlas = Image.new("RGBA", (2048, 2048), (0, 0, 0, 0))
for idx, (num, _, _) in enumerate(pick):
    p = os.path.join(SRC, "%02d.png" % num if num < 100 else "100.png")
    im = Image.open(p).convert("RGBA")
    bbox = im.getchannel("A").getbbox()
    im = im.crop(bbox)
    # fit into the slot preserving aspect
    sc = min(SW / float(im.width), SH / float(im.height))
    im = im.resize((max(1, int(im.width * sc)), max(1, int(im.height * sc))), Image.LANCZOS)
    # premultiply: bolt light stays, transparency becomes BLACK (additive-free)
    px = im.load()
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, a = px[x, y]
            px[x, y] = (r * a // 255, g * a // 255, b * a // 255, a)
    # BLOOM (his call: "perhaps a bit bloom?"): a blurred copy added under the
    # sharp channel - the halo follows every filament per pixel, and the additive
    # draw turns it into radiance. Two radii: a tight hot sheath + a wide soft glow.
    tight = im.filter(ImageFilter.GaussianBlur(3))
    wide  = im.filter(ImageFilter.GaussianBlur(10))
    im = ImageChops.add(im, ImageChops.add(
        tight.point(lambda v: v * 55 // 100),
        wide.point(lambda v: v * 40 // 100)))
    ox = (idx % 8) * SW + (SW - im.width) // 2
    oy = (idx // 8) * SH + (SH - im.height)      # BOTTOM-aligned: the channel's lowest lit pixel sits at the quad's ground edge
    atlas.paste(im, (ox, oy))

# --- write uncompressed A8R8G8B8 DDS ---
W, H = atlas.size
hdr = struct.pack("<4s7I44x", b"DDS ", 124,
                  0x1 | 0x2 | 0x4 | 0x8 | 0x1000,   # CAPS|HEIGHT|WIDTH|PITCH|PIXELFORMAT
                  H, W, W * 4, 0, 0)
pf = struct.pack("<2I4s5I", 32, 0x41, b"\0\0\0\0", 32,
                 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)   # DDPF_RGB|ALPHAPIXELS
caps = struct.pack("<4I4x", 0x1000, 0, 0, 0)
data = atlas.tobytes("raw", "BGRA")
with open(OUT, "wb") as f:
    f.write(hdr + pf + caps + data)
print("atlas written:", OUT, os.path.getsize(OUT) // 1024, "KB")
