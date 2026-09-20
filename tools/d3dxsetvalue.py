"""d3dxsetvalue.py - what does ID3DXConstantTable::SetValue actually put in the registers?

The client uploads its mirrored C++ structs (ConstParams -> AtmoParams, ShaderParams ->
PerObjectParams, LightF -> _Light, ...) through SetValue, which walks the shader struct
member by member and reads the source bytes at the PACKED offsets (the members' Bytes
summed in declaration order). A C++ struct that puts a member anywhere else - a padding
hole, an alignment the shader does not have - is misread SILENTLY: no error, wrong
numbers. On 2026-09-19 FMATRIX4's 16-byte alignment left a 4-byte hole before two
matrices moved to the end of ShaderParams, every matrix arrived one float early, and the
terrain vanished.

This makes the walk visible. It creates a NULLREF Direct3D 9 device, compiles the entry
point with the client's own compiler (d3dx9_43.dll), uploads a buffer whose every float
is its own index (0, 1, 2, ...) through SetValue exactly as ShaderClass::SetVSConstants /
SetPSConstants do, reads the constant registers back, and prints per member which SOURCE
FLOATS landed in its registers beside the byte range the packed walk predicts. Compare
the "floats a..b" column against tools/structlayout.py's C++ offsets: they must agree.

  python tools\d3dxsetvalue.py <file.hlsl> <entry> <vs_3_0|ps_3_0> [-D NAME[=VALUE]]...
                               -s <UniformName>:<bytes> [-s ...]

  e.g. python tools\d3dxsetvalue.py NewPlanet.hlsl TerrainVS vs_3_0 -D_WATER ... -s Const:432 -s Prm:352
  (run from the shaders directory, or give the file's path; bytes = sizeof the C++ mirror)
"""
import ctypes, os, sys
from ctypes import wintypes

class D3DXMACRO(ctypes.Structure):
    _fields_ = [("Name", ctypes.c_char_p), ("Definition", ctypes.c_char_p)]
class D3DPRESENT_PARAMETERS(ctypes.Structure):
    _fields_ = [("BackBufferWidth", ctypes.c_uint), ("BackBufferHeight", ctypes.c_uint), ("BackBufferFormat", ctypes.c_int),
                ("BackBufferCount", ctypes.c_uint), ("MultiSampleType", ctypes.c_int), ("MultiSampleQuality", ctypes.c_uint32),
                ("SwapEffect", ctypes.c_int), ("hDeviceWindow", ctypes.c_void_p), ("Windowed", ctypes.c_int),
                ("EnableAutoDepthStencil", ctypes.c_int), ("AutoDepthStencilFormat", ctypes.c_int), ("Flags", ctypes.c_uint32),
                ("FullScreen_RefreshRateInHz", ctypes.c_uint), ("PresentationInterval", ctypes.c_uint)]
class D3DXCONSTANT_DESC(ctypes.Structure):
    _fields_ = [("Name", ctypes.c_char_p), ("RegisterSet", ctypes.c_int), ("RegisterIndex", ctypes.c_uint),
                ("RegisterCount", ctypes.c_uint), ("Class", ctypes.c_int), ("Type", ctypes.c_int),
                ("Rows", ctypes.c_uint), ("Columns", ctypes.c_uint), ("Elements", ctypes.c_uint),
                ("StructMembers", ctypes.c_uint), ("Bytes", ctypes.c_uint), ("DefaultValue", ctypes.c_void_p)]

def vfn(obj, idx, restype, *argtypes):
    vt = ctypes.cast(obj, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    return ctypes.WINFUNCTYPE(restype, ctypes.c_void_p, *argtypes)(vt[idx])

def main(argv):
    if len(argv) < 4:
        print(__doc__); return 2
    src, entry, profile = argv[1], argv[2], argv[3]
    defines, structs = [], []
    i = 4
    while i < len(argv):
        a = argv[i]
        if a == "-D" and i + 1 < len(argv): i += 1; defines.append(argv[i])
        elif a.startswith("-D") and len(a) > 2: defines.append(a[2:])
        elif a == "-s" and i + 1 < len(argv):
            i += 1; name, _, nb = argv[i].partition(":"); structs.append((name, int(nb)))
        else: print("unknown argument", a); return 2
        i += 1
    if not structs:
        print("give at least one -s <Uniform>:<bytes>"); return 2
    if not os.path.isfile(src):
        print("no such file", src); return 2

    d3d9 = ctypes.WinDLL("d3d9.dll"); d3dx = ctypes.WinDLL("d3dx9_43.dll")
    d3d9.Direct3DCreate9.restype = ctypes.c_void_p
    d3d9.Direct3DCreate9.argtypes = [ctypes.c_uint]
    p3d = d3d9.Direct3DCreate9(32)
    if not p3d: print("Direct3DCreate9 failed"); return 2
    pp = D3DPRESENT_PARAMETERS()
    pp.Windowed = 1; pp.SwapEffect = 1; pp.BackBufferFormat = 0; pp.BackBufferCount = 1
    pp.hDeviceWindow = ctypes.windll.user32.GetDesktopWindow()
    dev = ctypes.c_void_p()
    # IDirect3D9::CreateDevice = vtable 16; D3DDEVTYPE_NULLREF = 4; SOFTWARE_VERTEXPROCESSING 0x20 | FPU_PRESERVE 0x2
    CreateDevice = vfn(p3d, 16, ctypes.c_long, ctypes.c_uint, ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32,
                       ctypes.POINTER(D3DPRESENT_PARAMETERS), ctypes.POINTER(ctypes.c_void_p))
    hr = CreateDevice(p3d, 0, 4, pp.hDeviceWindow, 0x22, ctypes.byref(pp), ctypes.byref(dev))
    if hr != 0 or not dev: print("CreateDevice failed 0x%08X" % (hr & 0xFFFFFFFF)); return 2

    macros = (D3DXMACRO * (len(defines) + 1))()
    keep = []
    for k, d in enumerate(defines):
        name, _, val = d.partition("=")
        nb, vb = name.encode(), (val if val else "1").encode(); keep += [nb, vb]
        macros[k].Name, macros[k].Definition = nb, vb
    macros[len(defines)].Name = None; macros[len(defines)].Definition = None
    comp = d3dx.D3DXCompileShaderFromFileA
    comp.restype = ctypes.c_long
    comp.argtypes = [ctypes.c_char_p, ctypes.POINTER(D3DXMACRO), ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p,
                     wintypes.DWORD, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p)]
    code, errs, ctab = ctypes.c_void_p(), ctypes.c_void_p(), ctypes.c_void_p()
    srcdir = os.path.dirname(os.path.abspath(src))
    os.chdir(srcdir)   # the file's #includes resolve beside it
    hr = comp(os.path.basename(src).encode(), macros, None, entry.encode(), profile.encode(), 0,
              ctypes.byref(code), ctypes.byref(errs), ctypes.byref(ctab))
    if hr != 0: print("compile failed 0x%08X" % (hr & 0xFFFFFFFF)); return 1
    print("compiled %s %s [%s]" % (os.path.basename(src), entry, " ".join(defines)))

    GetConstantByName = vfn(ctab, 9, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p)
    GetConstantDesc = vfn(ctab, 6, ctypes.c_long, ctypes.c_void_p, ctypes.POINTER(D3DXCONSTANT_DESC), ctypes.POINTER(ctypes.c_uint))
    GetConstant = vfn(ctab, 8, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint)
    SetValue = vfn(ctab, 12, ctypes.c_long, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint)
    # IDirect3DDevice9::GetVertexShaderConstantF = vtable 95, GetPixelShaderConstantF = 106
    isVS = profile.startswith("vs")
    GetConstF = vfn(dev, 95 if isVS else 106, ctypes.c_long, ctypes.c_uint, ctypes.POINTER(ctypes.c_float), ctypes.c_uint)

    for name, nbytes in structs:
        h = GetConstantByName(ctab, None, name.encode())
        if not h: print("%s: not in this entry point's constant table" % name); continue
        n = nbytes // 4
        src_buf = (ctypes.c_float * n)(*[float(i) for i in range(n)])
        hr = SetValue(ctab, dev, h, ctypes.cast(src_buf, ctypes.c_void_p), nbytes)
        print("%s (%d bytes): SetValue hr 0x%08X" % (name, nbytes, hr & 0xFFFFFFFF))
        cd = D3DXCONSTANT_DESC(); cnt = ctypes.c_uint(1)
        GetConstantDesc(ctab, h, ctypes.byref(cd), ctypes.byref(cnt))
        if cd.StructMembers == 0:
            print("  (not a struct)"); continue
        off = 0
        print("  %-14s %-6s %-18s %s" % ("member", "reg", "packed walk", "registers hold (source float indices)"))
        for j in range(cd.StructMembers):
            hm = GetConstant(ctab, h, j)
            md = D3DXCONSTANT_DESC(); nm = ctypes.c_uint(1)
            GetConstantDesc(ctab, hm, ctypes.byref(md), ctypes.byref(nm))
            got = []
            for r in range(md.RegisterIndex, md.RegisterIndex + md.RegisterCount):
                buf = (ctypes.c_float * 4)()
                GetConstF(dev, r, buf, 1)
                got.append("[" + " ".join("%g" % v for v in buf) + "]")
            print("  %-14s c%-5d floats %4d..%-4d  %s" % (md.Name.decode(), md.RegisterIndex, off // 4, (off + md.Bytes) // 4 - 1,
                  " ".join(got) if got else "(no registers: unreferenced tail)"))
            off += md.Bytes
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
