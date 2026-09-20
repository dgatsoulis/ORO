# d3dxps.py - compile a standalone ps_3_0 / vs_3_0 entry point with the CLIENT'S OWN compiler.
#
# The patched D3D9Client compiles NewPlanet.hlsl (and the other ShaderClass shaders) at run
# time through D3DXCompileShaderFromFile in d3dx9_43.dll, the June 2010 SDK's library. That
# compiler allocates literal constants into the same 224 ps_3_0 registers as the uniforms and
# packs them differently from the Windows-Kit fxc: on 2026-09-10 fxc passed a terrain shader
# that the client rejected with X4507 in his sim. This drives the very same DLL through
# ctypes, so what passes here passes there. tools/fxeff does the same for the EFFECT files.
#
#   python tools\d3dxps.py <file.hlsl> <entry> [ps_3_0|vs_3_0] [-D NAME[=VALUE]]... [-flags N] [-asm out.txt]
#
# Prints OK/FAIL, the compiler's message on failure, and on success the highest constant
# register the code touches (uniforms and literals together) and the instruction count.
# Exit code: 0 compiled, 1 compile error, 2 usage/setup error.
import ctypes, os, re, sys
from ctypes import wintypes

D3DXSHADER_PARTIALPRECISION = (1 << 5)
D3DXSHADER_NO_PRESHADER     = (1 << 8)
D3DXSHADER_PREFER_FLOW_CONTROL = (1 << 10)

class D3DXMACRO(ctypes.Structure):
    _fields_ = [("Name", ctypes.c_char_p), ("Definition", ctypes.c_char_p)]

def buffer_text(pbuf):
    """ID3DXBuffer -> bytes (vtable: QueryInterface, AddRef, Release, GetBufferPointer, GetBufferSize)."""
    if not pbuf:
        return b""
    vtbl = ctypes.cast(pbuf, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    GetBufferPointer = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vtbl[3])
    GetBufferSize = ctypes.WINFUNCTYPE(ctypes.c_uint32, ctypes.c_void_p)(vtbl[4])
    p = GetBufferPointer(pbuf); n = GetBufferSize(pbuf)
    return ctypes.string_at(p, n)

def main(argv):
    if len(argv) < 3:
        print(__doc__); return 2
    src, entry = argv[1], argv[2]
    profile = "ps_3_0"
    defines, flags, asm_out = [], 0, None
    members = False
    i = 3
    while i < len(argv):
        a = argv[i]
        if a in ("ps_3_0", "vs_3_0", "ps_2_0", "vs_2_0"): profile = a
        elif a == "-D" and i + 1 < len(argv): i += 1; defines.append(argv[i])
        elif a.startswith("-D") and len(a) > 2: defines.append(a[2:])
        elif a.startswith("/D") and len(a) > 2: defines.append(a[2:])
        elif a == "-flags" and i + 1 < len(argv): i += 1; flags = int(argv[i], 0)
        elif a == "-asm" and i + 1 < len(argv): i += 1; asm_out = argv[i]
        elif a == "-members": members = True
        else: print("unknown argument", a); return 2
        i += 1
    if not os.path.isfile(src):
        print("no such file", src); return 2
    try:
        d3dx = ctypes.WinDLL("d3dx9_43.dll")
    except OSError as e:
        print("cannot load d3dx9_43.dll:", e); return 2

    macros = (D3DXMACRO * (len(defines) + 1))()
    keep = []
    for k, d in enumerate(defines):
        name, _, val = d.partition("=")
        nb, vb = name.encode(), (val if val else "1").encode()
        keep += [nb, vb]
        macros[k].Name, macros[k].Definition = nb, vb
    macros[len(defines)].Name = None; macros[len(defines)].Definition = None

    compile_fn = d3dx.D3DXCompileShaderFromFileA
    compile_fn.restype = ctypes.c_long
    compile_fn.argtypes = [ctypes.c_char_p, ctypes.POINTER(D3DXMACRO), ctypes.c_void_p, ctypes.c_char_p,
                           ctypes.c_char_p, wintypes.DWORD, ctypes.POINTER(ctypes.c_void_p),
                           ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p)]
    code, errs, ctab = ctypes.c_void_p(), ctypes.c_void_p(), ctypes.c_void_p()
    hr = compile_fn(src.encode(), macros, None, entry.encode(), profile.encode(), flags,
                    ctypes.byref(code), ctypes.byref(errs), ctypes.byref(ctab))
    msg = buffer_text(errs).decode(errors="replace").strip()
    label = "%s %s [%s]" % (os.path.basename(src), entry, " ".join(defines))
    # 2026-09-19: a WARNING fails the row too - the client boxes the buffer whatever it holds
    # (D3D9Util.cpp: "Failed to compile a shader"); X4717 stays ignored as in fxccheck.sh.
    warn = [l for l in msg.splitlines() if "warning X4" in l and "X4717" not in l]
    if hr != 0 or not code or warn:
        print("FAIL  %s  hr 0x%08X" % (label, hr & 0xFFFFFFFF))
        print("      " + msg.replace("\n", "\n      "))
        return 1

    dis = ctypes.c_void_p()
    d3dx.D3DXDisassembleShader.restype = ctypes.c_long
    d3dx.D3DXDisassembleShader.argtypes = [ctypes.c_void_p, wintypes.BOOL, ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
    vt = ctypes.cast(code, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    GetBufferPointer = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])
    d3dx.D3DXDisassembleShader(GetBufferPointer(code), False, None, ctypes.byref(dis))
    text = buffer_text(dis).decode(errors="replace")
    if asm_out:
        open(asm_out, "w").write(text)
    regs = [int(m) for m in re.findall(r"\bc(\d+)\b", text)]
    cmax = max(regs) if regs else -1
    defs = len(re.findall(r"^\s*def c", text, re.M))
    m = re.search(r"approximately (\d+) instruction slot", text)
    slots = m.group(1) if m else "?"
    samp = len(set(re.findall(r"\bs(\d+)\b", text)))
    print("OK    %s  max c%d (%d literal defs)  %s slots  %d samplers  %s" % (label, cmax, defs, slots, samp, msg))
    if members:
        dump_members(ctab, text)
    return 0


# ---------------------------------------------------------------------------------------------
# -members (2026-09-19, the terrain register audit): walk the constant table and print, for every
# uniform and every member of a struct uniform, the registers D3DX allocated to it and how many
# of those the compiled CODE actually reads. D3DX allocates a referenced struct WHOLE, one
# register per SCALAR member (no packing inside a struct), so this is where a 224-register
# budget goes: a struct the pixel shader touches once costs all of it.
# ---------------------------------------------------------------------------------------------
class D3DXCONSTANT_DESC(ctypes.Structure):
    _fields_ = [("Name", ctypes.c_char_p), ("RegisterSet", ctypes.c_int), ("RegisterIndex", ctypes.c_uint),
                ("RegisterCount", ctypes.c_uint), ("Class", ctypes.c_int), ("Type", ctypes.c_int),
                ("Rows", ctypes.c_uint), ("Columns", ctypes.c_uint), ("Elements", ctypes.c_uint),
                ("StructMembers", ctypes.c_uint), ("Bytes", ctypes.c_uint), ("DefaultValue", ctypes.c_void_p)]

class D3DXCONSTANTTABLE_DESC(ctypes.Structure):
    _fields_ = [("Creator", ctypes.c_char_p), ("Version", ctypes.c_uint32), ("Constants", ctypes.c_uint)]

REGSET = {0: "b", 1: "i", 2: "c", 3: "s"}

def dump_members(ctab, asm_text):
    # ID3DXConstantTable vtable: IUnknown (0-2), ID3DXBuffer (3-4), GetDesc 5, GetConstantDesc 6,
    # GetSamplerIndex 7, GetConstant 8, GetConstantByName 9, GetConstantElement 10, ...
    vt = ctypes.cast(ctab, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    GetDesc = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.POINTER(D3DXCONSTANTTABLE_DESC))(vt[5])
    GetConstantDesc = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.c_void_p,
                                         ctypes.POINTER(D3DXCONSTANT_DESC), ctypes.POINTER(ctypes.c_uint))(vt[6])
    GetConstant = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint)(vt[8])
    # the registers the CODE reads (def lines and declarations excluded)
    used = {"c": set(), "b": set(), "i": set(), "s": set()}
    for ln in asm_text.splitlines():
        s = ln.strip()
        if not s or s.startswith("//") or s.startswith(("def ", "defb", "defi", "dcl", "ps_", "vs_")):
            continue
        for m in re.finditer(r"\b([cbis])(\d+)", s):
            used[m.group(1)].add(int(m.group(2)))
    td = D3DXCONSTANTTABLE_DESC()
    GetDesc(ctab, ctypes.byref(td))
    print("constant table: %d constants" % td.Constants)
    print("  %-34s %-6s %5s %5s  %s" % ("name", "set", "regs", "read", "unread registers"))
    def line(name, cd, indent, off=None):
        rs = REGSET.get(cd.RegisterSet, "?")
        regs = list(range(cd.RegisterIndex, cd.RegisterIndex + cd.RegisterCount))
        u = used.get(rs, set())
        unread = [r for r in regs if r not in u]
        # off: the member's byte offset in the SOURCE buffer as SetValue walks it (members' Bytes
        # accumulated in declaration order) - what the C++ mirror must match
        print("  %-34s %-6s %5d %5d  %s%s" % (indent + name, rs + str(cd.RegisterIndex), cd.RegisterCount,
              cd.RegisterCount - len(unread), ("" if off is None else "bytes %4d at %4d  " % (cd.Bytes, off)),
              "-" if not unread else ",".join(rs + str(r) for r in unread)))
    for i in range(td.Constants):
        h = GetConstant(ctab, None, i)
        cd = D3DXCONSTANT_DESC(); n = ctypes.c_uint(1)
        if GetConstantDesc(ctab, h, ctypes.byref(cd), ctypes.byref(n)) != 0:
            continue
        line(cd.Name.decode(), cd, "")
        off = 0
        for j in range(cd.StructMembers):
            hm = GetConstant(ctab, h, j)
            md = D3DXCONSTANT_DESC(); nm = ctypes.c_uint(1)
            if GetConstantDesc(ctab, hm, ctypes.byref(md), ctypes.byref(nm)) != 0:
                continue
            line(md.Name.decode(), md, "  .", off)
            off += md.Bytes

if __name__ == "__main__":
    sys.exit(main(sys.argv))
