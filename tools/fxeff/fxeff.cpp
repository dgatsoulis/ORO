// fxeff - compile a D3D9 effect (.fx) with the SAME compiler and flags the D3D9Client uses
// (d3dx9_43, D3DXCreateEffectFromFile, NO_PRESHADER | PREFER_FLOW_CONTROL) and report, per
// technique pass, the shader model and the constant / sampler registers each compiled
// vertex and pixel shader actually occupies, plus the instruction count.
//
// WHY THIS EXISTS (2026-09-08): the Windows-Kit fxc.exe in tools/fxccheck.sh is a different
// compiler. It rejected a tester's live configuration (LightConfiguration 4 = "8x Full" with
// Cascaded shadows) with X4507/X4505 while that tester's client had compiled it and was
// running. So fxc cannot answer "how many local lights fit". This tool asks the compiler
// that decides.
//
// usage: fxeff <file.fx> [-D NAME[=VALUE]]... [-nopre] [-flow] [-pass <substr>] [-q]
//   -nopre  D3DXSHADER_NO_PRESHADER        (the client sets both; default on)
//   -flow   D3DXSHADER_PREFER_FLOW_CONTROL (the client sets both; default on)
//   -nofl   clear both flags (a plain compile)
//   -pass   only report passes whose "technique/pass" name contains <substr>
//   -q      errors and the summary line only
// exit code: 0 compiled, 1 compile error, 2 usage/device error.
//
// Build: tools\fxeff\build.cmd (x86, DXSDK June 2010 - the client's own SDK).
// Run from anywhere; #include lines resolve relative to the .fx file (default D3DX handler).

#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <vector>
#include <string>

struct RegUse { int f4, i4, b, s; int f4max, samax; };

static RegUse Measure(const DWORD* func)
{
    RegUse r = { 0, 0, 0, 0, 0, 0 };
    if (!func) return r;
    LPD3DXCONSTANTTABLE ct = NULL;
    if (FAILED(D3DXGetShaderConstantTable(func, &ct)) || !ct) return r;
    D3DXCONSTANTTABLE_DESC cd;
    ct->GetDesc(&cd);
    for (UINT i = 0; i < cd.Constants; i++) {
        D3DXHANDLE h = ct->GetConstant(NULL, i);
        D3DXCONSTANT_DESC d[16]; UINT n = 16;
        if (FAILED(ct->GetConstantDesc(h, d, &n))) continue;
        for (UINT k = 0; k < n; k++) {
            int top = (int)(d[k].RegisterIndex + d[k].RegisterCount);
            switch (d[k].RegisterSet) {
            case D3DXRS_FLOAT4:  r.f4 += d[k].RegisterCount; if (top > r.f4max) r.f4max = top; break;
            case D3DXRS_INT4:    r.i4 += d[k].RegisterCount; break;
            case D3DXRS_BOOL:    r.b  += d[k].RegisterCount; break;
            case D3DXRS_SAMPLER: r.s  += d[k].RegisterCount; if (top > r.samax) r.samax = top; break;
            }
        }
    }
    ct->Release();
    return r;
}

static int Instructions(const DWORD* func)
{
    if (!func) return -1;
    LPD3DXBUFFER buf = NULL;
    if (FAILED(D3DXDisassembleShader(func, FALSE, NULL, &buf)) || !buf) return -1;
    const char* s = (const char*)buf->GetBufferPointer();
    const char* p = strstr(s, "approximately ");
    int n = -1;
    if (p) n = atoi(p + 14);
    buf->Release();
    return n;
}

static const char* Model(const DWORD* func, char* b)   // b: caller's 16-byte buffer (two calls per printf)
{
    if (!func) return "-";
    DWORD v = *func;
    const char* kind = ((v & 0xFFFF0000) == 0xFFFF0000) ? "ps" : "vs";
    sprintf_s(b, 16, "%s_%d_%d", kind, (int)D3DSHADER_VERSION_MAJOR(v), (int)D3DSHADER_VERSION_MINOR(v));
    return b;
}

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: fxeff <file.fx> [-D NAME[=VALUE]]... [-nofl] [-pass substr] [-q]\n"); return 2; }
    const char* file = argv[1];
    std::vector<std::string> names, values;
    DWORD flags = D3DXSHADER_NO_PRESHADER | D3DXSHADER_PREFER_FLOW_CONTROL;
    const char* passFilter = NULL;
    bool quiet = false;
    int psSlots = 0;        // ps_3_0 instruction-slot cap to check against (0 = read the HAL adapter's)
    const char* asmFile = NULL;   // -asm <file>: dump the (-pass filtered) pixel shaders' disassembly there
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-D") && i + 1 < argc) {
            std::string d = argv[++i];
            size_t eq = d.find('=');
            names.push_back(eq == std::string::npos ? d : d.substr(0, eq));
            values.push_back(eq == std::string::npos ? "1" : d.substr(eq + 1));
        }
        else if (!strncmp(argv[i], "-D", 2) && strlen(argv[i]) > 2) {
            std::string d = argv[i] + 2;
            size_t eq = d.find('=');
            names.push_back(eq == std::string::npos ? d : d.substr(0, eq));
            values.push_back(eq == std::string::npos ? "1" : d.substr(eq + 1));
        }
        else if (!strcmp(argv[i], "-nofl")) flags = 0;
        else if (!strcmp(argv[i], "-nopre")) flags |= D3DXSHADER_NO_PRESHADER;
        else if (!strcmp(argv[i], "-flow")) flags |= D3DXSHADER_PREFER_FLOW_CONTROL;
        else if (!strcmp(argv[i], "-pass") && i + 1 < argc) passFilter = argv[++i];
        else if (!strcmp(argv[i], "-q")) quiet = true;
        else if (!strcmp(argv[i], "-slots") && i + 1 < argc) psSlots = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-asm") && i + 1 < argc) asmFile = argv[++i];
        else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 2; }
    }
    std::vector<D3DXMACRO> macros;
    for (size_t i = 0; i < names.size(); i++) { D3DXMACRO m = { names[i].c_str(), values[i].c_str() }; macros.push_back(m); }
    D3DXMACRO end = { NULL, NULL }; macros.push_back(end);

    // A NULLREF device: the effect compiler wants one, nothing is rendered.
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { fprintf(stderr, "Direct3DCreate9 failed\n"); return 2; }

    // 2026-09-09: THE HARDWARE CAP. A pass that compiles is not a pass the device will run:
    // the runtime validates a ps_3_0 shader against MaxPixelShader30InstructionSlots, and
    // NVIDIA reports 4096 (patch (ah) step 4, 16x Full + six maps = 4758 slots: the DG hulls
    // vanished and the frame rate fell to 20, no error logged anywhere). Read the HAL
    // adapter's cap here (no device needed) and FAIL a pass above it; -slots N overrides.
    char adapter[160] = "unknown adapter";
    {
        D3DADAPTER_IDENTIFIER9 id; ZeroMemory(&id, sizeof(id));
        if (SUCCEEDED(d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id))) strcpy_s(adapter, sizeof(adapter), id.Description);
    }
    if (psSlots <= 0) {
        D3DCAPS9 caps; ZeroMemory(&caps, sizeof(caps));
        if (SUCCEEDED(d3d->GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps))) psSlots = (int)caps.MaxPixelShader30InstructionSlots;
        else psSlots = 4096;   // no HAL here: assume the tightest cap in the field
    }
    else strcat_s(adapter, sizeof(adapter), ", cap pinned by -slots");
    D3DPRESENT_PARAMETERS pp; ZeroMemory(&pp, sizeof(pp));
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.BackBufferWidth = 64; pp.BackBufferHeight = 64; pp.hDeviceWindow = GetDesktopWindow();
    IDirect3DDevice9* dev = NULL;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, GetDesktopWindow(),
                                   D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &dev);
    if (FAILED(hr) || !dev) { fprintf(stderr, "CreateDevice(NULLREF) failed 0x%08X\n", (unsigned)hr); return 2; }

    LPD3DXEFFECT fx = NULL; LPD3DXBUFFER err = NULL;
    hr = D3DXCreateEffectFromFileA(dev, file, macros.data(), NULL, flags, NULL, &fx, &err);
    if (err) {
        printf("%s", (const char*)err->GetBufferPointer());
        err->Release();
    }
    if (FAILED(hr) || !fx) { printf("COMPILE FAILED (0x%08X)\n", (unsigned)hr); return 1; }

    D3DXEFFECT_DESC ed; fx->GetDesc(&ed);
    int worstPSf4 = 0, worstVSf4 = 0, worstSam = 0, worstInst = 0;
    std::string worstPS, worstVS, worstInstPass;
    if (!quiet) printf("%-34s %-7s %5s %5s %4s %4s %5s   %-7s %5s %5s %5s\n",
        "technique/pass", "ps", "c#", "cmax", "b#", "smp", "instr", "vs", "c#", "cmax", "instr");
    for (UINT t = 0; t < ed.Techniques; t++) {
        D3DXHANDLE ht = fx->GetTechnique(t);
        D3DXTECHNIQUE_DESC td; fx->GetTechniqueDesc(ht, &td);
        for (UINT p = 0; p < td.Passes; p++) {
            D3DXHANDLE hp = fx->GetPass(ht, p);
            D3DXPASS_DESC pd; fx->GetPassDesc(hp, &pd);
            char name[128]; sprintf_s(name, "%s/%s", td.Name ? td.Name : "?", pd.Name ? pd.Name : "?");
            if (passFilter && !strstr(name, passFilter)) continue;
            RegUse ps = Measure(pd.pPixelShaderFunction);
            RegUse vs = Measure(pd.pVertexShaderFunction);
            int ipi = Instructions(pd.pPixelShaderFunction);
            int ivi = Instructions(pd.pVertexShaderFunction);
            if (ps.f4max > worstPSf4) { worstPSf4 = ps.f4max; worstPS = name; }
            if (vs.f4max > worstVSf4) { worstVSf4 = vs.f4max; worstVS = name; }
            if (ps.samax > worstSam) worstSam = ps.samax;
            if (ipi > worstInst) { worstInst = ipi; worstInstPass = name; }
            if (asmFile && pd.pPixelShaderFunction) {   // 2026-09-09: the disassembly, for the flow-control questions the counts cannot answer
                LPD3DXBUFFER dis = NULL;
                if (SUCCEEDED(D3DXDisassembleShader(pd.pPixelShaderFunction, FALSE, NULL, &dis)) && dis) {
                    FILE* fo = NULL;
                    if (fopen_s(&fo, asmFile, "ab") == 0 && fo) {
                        fprintf(fo, "\n// ===== %s (ps) =====\n%s\n", name, (const char*)dis->GetBufferPointer());
                        fclose(fo);
                    }
                    dis->Release();
                }
            }
            char mp[16], mv[16];
            if (!quiet) printf("%-34s %-7s %5d %5d %4d %4d %5d   %-7s %5d %5d %5d\n",
                name, Model(pd.pPixelShaderFunction, mp), ps.f4, ps.f4max, ps.b, ps.samax, ipi,
                Model(pd.pVertexShaderFunction, mv), vs.f4, vs.f4max, ivi);
        }
    }
    const bool over = (psSlots > 0 && worstInst > psSlots);
    printf("%s  ps c-max %d/224 (%s)  vs c-max %d/256 (%s)  samplers-max %d/16  ps instr-max %d/%d slots (%s; cap of %s)\n",
           over ? "FAIL" : "OK", worstPSf4, worstPS.c_str(), worstVSf4, worstVS.c_str(), worstSam,
           worstInst, psSlots, worstInstPass.c_str(), adapter);
    fx->Release(); dev->Release(); d3d->Release();
    return over ? 1 : 0;
}
