r"""structlayout.py - the 32-bit byte layout of the client's shader-mirrored C++ structs.

The client's ConstParams / ShaderParams / FlowControl* (VPlanet.h, the pack(4) block) and
Surfmgr2.cpp's LightF are uploaded byte for byte into shader structs through D3DX's
SetValue, which walks the PACKED shader layout. The C++ side must match it exactly, and
the compiler does not promise that: FMATRIX4 carries a 16-byte alignment of its own, and
#pragma pack(4) lowers only natural alignment, never a type's declared one - so a matrix
after a stray scalar gets a 4-byte hole in front of it and every later member is read one
float early (2026-09-19, the terrain vanished). VPlanet.h now carries static_asserts on the
two big structs; this prints the real offsets so a new member can be checked BEFORE it is
built into a DLL and flown.

It extracts the pack(4) block of VPlanet.h and the LightF block of Surfmgr2.cpp verbatim,
compiles a tiny 32-bit probe with the client's own compiler (cl via vcvars32) against the
Orbiter SDK and DirectX headers, and runs it. Then compare against tools/d3dxsetvalue.py's
"packed walk" column, which is what the shader side expects.

  python tools\structlayout.py [clone-D3D9Client-dir]      (default C:\OrbiterDev\orbiter\OVP\D3D9Client)
"""
import os, re, subprocess, sys, tempfile

VCVARS = r"Z:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
SDKINC = r"C:\OrbiterDev\orbiter\Orbitersdk\include"
DXINC  = r"C:\OrbiterDev\DXSDK\Include"

PROBE = r'''
#include <windows.h>
#include <stdio.h>
#include <stddef.h>
#include <d3dx9.h>
#include "OrbiterAPI.h"
#include "DrawAPI.h"
using namespace oapi;
#define float2 FVECTOR2
#define float3 FVECTOR3
#define float4 FVECTOR4
#define float4x4 FMATRIX4
#include "structs.h"
#include "lightf.h"
#define OFF(S, m) printf("  %-16s at %4u\n", #m, (unsigned)offsetof(S, m))
int main()
{
    printf("sizeof FVECTOR2 %u FVECTOR3 %u FVECTOR4 %u FMATRIX4 %u (alignof FMATRIX4 %u)\n",
           (unsigned)sizeof(FVECTOR2), (unsigned)sizeof(FVECTOR3), (unsigned)sizeof(FVECTOR4),
           (unsigned)sizeof(FMATRIX4), (unsigned)__alignof(FMATRIX4));
    printf("ConstParams %u bytes\n", (unsigned)sizeof(ConstParams));
    OFF(ConstParams, CamPos); OFF(ConstParams, HG); OFF(ConstParams, cAmbient); OFF(ConstParams, PlanetRad);
    OFF(ConstParams, Up); OFF(ConstParams, rmO); OFF(ConstParams, PlanetRad2); OFF(ConstParams, wBoost); OFF(ConstParams, mVP);
    printf("ShaderParams %u bytes\n", (unsigned)sizeof(ShaderParams));
    OFF(ShaderParams, vSHD); OFF(ShaderParams, vOverlayCtrl); OFF(ShaderParams, vEclipse); OFF(ShaderParams, fEclipse);
    OFF(ShaderParams, fAlpha); OFF(ShaderParams, fTgtScale); OFF(ShaderParams, fPad0); OFF(ShaderParams, mWorld); OFF(ShaderParams, mLVP);
    printf("LightF %u bytes\n", (unsigned)sizeof(LightF));
    OFF(LightF, position); OFF(LightF, direction); OFF(LightF, diffuse); OFF(LightF, attenuation);
    printf("FlowControlPS %u bytes, FlowControlVS %u bytes\n", (unsigned)sizeof(FlowControlPS), (unsigned)sizeof(FlowControlVS));
    return 0;
}
'''

def block(text, start_pat, end_pat, drop=()):
    lines = text.replace("\r\n", "\n").split("\n")   # VPlanet.h is CRLF; a text-mode write would double the CR
    i0 = next(i for i, l in enumerate(lines) if re.search(start_pat, l))
    i1 = next(i for i in range(i0 + 1, len(lines)) if re.search(end_pat, lines[i]))
    out = [l for l in lines[i0:i1 + 1] if not any(re.search(d, l) for d in drop)]
    return "\n".join(out) + "\n"

def main(argv):
    src = argv[1] if len(argv) > 1 else r"C:\OrbiterDev\orbiter\OVP\D3D9Client"
    vp = open(os.path.join(src, "VPlanet.h"), "rb").read().decode("latin-1")
    sm = open(os.path.join(src, "Surfmgr2.cpp"), "rb").read().decode("latin-1")
    structs = block(vp, r"^#pragma pack\(push, 4\)", r"^#pragma pack\(pop\)", drop=(r"static_assert", r"#include <cstddef>"))
    lightf = block(sm, r"^#pragma pack\(push, 4\)", r"^#pragma pack\(pop\)")
    work = os.path.join(tempfile.gettempdir(), "oro_structlayout")
    os.makedirs(work, exist_ok=True)
    open(os.path.join(work, "structs.h"), "w").write(structs)
    open(os.path.join(work, "lightf.h"), "w").write(lightf)
    open(os.path.join(work, "layout.cpp"), "w").write(PROBE)
    cmd = ('call "%s" >nul 2>&1 & cd /d "%s" & cl /nologo /EHsc /W1 /I"%s" /I"%s" layout.cpp /Fe:layout.exe > build.log 2>&1 & "%s\\layout.exe"'
           % (VCVARS, work, SDKINC, DXINC, work))   # full path: this shell does not search the cwd for executables
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True)   # one string: cmd.exe parses it whole
    out = r.stdout.strip()
    if "sizeof" not in out:
        print("the probe did not build; build.log:")
        lg = os.path.join(work, "build.log")
        print(open(lg, errors="replace").read()[-2000:] if os.path.isfile(lg) else "(no build.log - vcvars32 or cl not found) " + r.stdout + r.stderr)
        return 1
    print(out)
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
