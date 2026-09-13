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
    i = 3
    while i < len(argv):
        a = argv[i]
        if a in ("ps_3_0", "vs_3_0", "ps_2_0", "vs_2_0"): profile = a
        elif a == "-D" and i + 1 < len(argv): i += 1; defines.append(argv[i])
        elif a.startswith("-D") and len(a) > 2: defines.append(a[2:])
        elif a.startswith("/D") and len(a) > 2: defines.append(a[2:])
        elif a == "-flags" and i + 1 < len(argv): i += 1; flags = int(argv[i], 0)
        elif a == "-asm" and i + 1 < len(argv): i += 1; asm_out = argv[i]
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
    if hr != 0 or not code:
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
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
