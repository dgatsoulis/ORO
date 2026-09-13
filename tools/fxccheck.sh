#!/bin/bash
# ORO: the MANDATORY fxc compile-check matrix for the patched client shaders (2026-09-05).
# Runs every entry point in every configuration the client compiles, including the
# _DEVTOOLS terrain build that sits at the ps_3_0 sampler ceiling. Run from Git Bash:
#   tools/fxccheck.sh [path-to-clone-shaders]
# Prints ok/FAIL per entry with the instruction-slot count; exit 1 on any failure.
FXC="/c/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x86/fxc.exe"
FXEFF="$(cd "$(dirname "$0")" && pwd)/fxeff/fxeff.exe"   # resolved BEFORE the cd below
D3DXPS=$(cygpath -w "$(cd "$(dirname "$0")" && pwd)/d3dxps.py")   # ditto (Windows form: the runner
#                                                                     below suppresses msys path conversion)
cd "${1:-/c/OrbiterDev/orbiter/OVP/D3D9Client/shaders}" || exit 1
fail=0
run() { # label, args...
  local label="$1"; shift
  out=$(MSYS_NO_PATHCONV=1 "$FXC" /nologo "$@" /Fo "$TEMP/fxcchk.fxo" /Fc "$TEMP/fxcchk.asm" 2>&1)
  rc=$?
  bad=$(echo "$out" | grep -v X4717 | grep -iE 'error|warning X4')
  if [ $rc -ne 0 ] || [ -n "$bad" ]; then
    echo "FAIL  $label"; echo "$bad" | head -6; fail=1
  else
    inst=$(grep -oE 'approximately [0-9]+ instruction slot' "$TEMP/fxcchk.asm" 2>/dev/null | head -1)
    echo "ok    $label  ${inst}"
  fi
}
# 2026-09-10: THE STANDALONE TERRAIN ENTRY POINTS RUN THROUGH tools/d3dxps.py, NOT fxc. His sim
# rejected a NewPlanet.hlsl the Windows-Kit fxc had just passed (X4507, max ps_3_0 constant register
# index exceeded): the client compiles these through d3dx9_43.dll, whose literal-constant allocation
# differs, and the Earth terrain shader with six maps + cube + dev tools sits at c223 of 224. d3dxps
# drives that very DLL, so what passes here passes in the sim; it also prints the highest register.
dxrun() { # label, entry, profile, defines...
  local label="$1" entry="$2" prof="$3"; shift 3
  out=$(MSYS_NO_PATHCONV=1 python "$D3DXPS" NewPlanet.hlsl "$entry" "$prof" "$@" 2>&1)
  if echo "$out" | grep -q '^OK'; then
    echo "ok    $label  $(echo "$out" | sed -E 's/^OK +[^ ]+ [^ ]+ \[[^]]*\] +//')"
  else
    echo "FAIL  $label"; echo "$out" | tail -3; fail=1
  fi
}

E="/D_WATER=1 /D_RIPPLES=1 /D_CLOUDSHD=1 /D_NIGHTLIGHTS=1 /D_LOCALLIGHTS=1 /D_SHDMAP=1"
DE="-D_WATER -D_RIPPLES -D_CLOUDSHD -D_NIGHTLIGHTS -D_LOCALLIGHTS -D_SHDMAP"
# 2026-09-08: THE EFFECT ROWS RUN THROUGH tools/fxeff, NOT fxc. The Windows-Kit fxc rejected a
# tester's LIVE 8x Full + Cascaded configuration (X4507 / X4505 / X4550) while her client had
# compiled it with D3DX and was running it; fxeff IS that D3DX compiler on a NULLREF device and
# reports registers per pass. fxc stays below for the standalone ps_3_0 terrain entry points.
# 2026-09-09: -slots 4096 pins the ps_3_0 instruction-slot cap NVIDIA reports (a pass over it compiles
# and then silently fails on the device - patch (ah) step 4 at 16x), whatever GPU runs this check.
effrun() { # label, args...
  label="$1"; shift
  out=$(MSYS_NO_PATHCONV=1 "$FXEFF" D3D9Client.fx "$@" -slots 4096 -q 2>&1); rc=$?
  if [ $rc -ne 0 ]; then echo "FAIL  $label"; echo "$out" | head -6; fail=1; else echo "ok    $label  $(echo "$out" | tail -1)"; fi
}
effrun "D3D9Client.fx 4x Partial"               -D ANISOTROPY_MACRO=4 -D LMODE=1 -D MAX_LIGHTS=4 -D SHDMAP=3 -D KERNEL_SIZE=27 -D KERNEL_WEIGHT=0.037037
effrun "D3D9Client.fx HIS 4x Full + cascade"    -D ANISOTROPY_MACRO=4 -D LMODE=2 -D MAX_LIGHTS=4 -D SHDMAP=3 -D KERNEL_SIZE=27 -D KERNEL_WEIGHT=0.037037 -D _ENVMAP -D _LIGHTGLOW -D _DEBUG -D _IRRADIANCE -D _CASCADE -D LCLMAPS=4
effrun "D3D9Client.fx MARG 8x Full + cascade + glass" -D ANISOTROPY_MACRO=4 -D LMODE=4 -D MAX_LIGHTS=8 -D SHDMAP=3 -D KERNEL_SIZE=27 -D KERNEL_WEIGHT=0.037037 -D _ENVMAP -D _LIGHTGLOW -D _DEBUG -D _IRRADIANCE -D _GLASS -D _CASCADE -D LCLMAPS=6 -D _LCLCUBE
effrun "D3D9Client.fx 8x Full + cascade + kernel 35" -D ANISOTROPY_MACRO=4 -D LMODE=4 -D MAX_LIGHTS=8 -D SHDMAP=4 -D KERNEL_SIZE=35 -D KERNEL_WEIGHT=0.0285 -D _ENVMAP -D _LIGHTGLOW -D _DEBUG -D _IRRADIANCE -D _GLASS -D _CASCADE -D LCLMAPS=6 -D _LCLCUBE
effrun "D3D9Client.fx NOSHADOW 4x Full + cascade" -D ANISOTROPY_MACRO=4 -D LMODE=2 -D MAX_LIGHTS=4 -D SHDMAP=0 -D KERNEL_SIZE=27 -D KERNEL_WEIGHT=0.037037 -D _ENVMAP -D _LIGHTGLOW -D _DEBUG -D _IRRADIANCE -D _CASCADE -D LCLMAPS=1
effrun "D3D9Client.fx 12x Full + cascade + glass"  -D ANISOTROPY_MACRO=4 -D LMODE=6 -D MAX_LIGHTS=12 -D SHDMAP=3 -D KERNEL_SIZE=27 -D KERNEL_WEIGHT=0.037037 -D _ENVMAP -D _LIGHTGLOW -D _DEBUG -D _IRRADIANCE -D _GLASS -D _CASCADE -D LCLMAPS=6 -D _LCLCUBE
effrun "D3D9Client.fx 16x Full + cascade + glass + kernel 35 (worst)" -D ANISOTROPY_MACRO=4 -D LMODE=8 -D MAX_LIGHTS=16 -D SHDMAP=4 -D KERNEL_SIZE=35 -D KERNEL_WEIGHT=0.0285 -D _ENVMAP -D _LIGHTGLOW -D _DEBUG -D _IRRADIANCE -D _GLASS -D _CASCADE -D LCLMAPS=6 -D _LCLCUBE
effrun "D3D9Client.fx 16x Partial + cascade"        -D ANISOTROPY_MACRO=4 -D LMODE=7 -D MAX_LIGHTS=16 -D SHDMAP=3 -D KERNEL_SIZE=27 -D KERNEL_WEIGHT=0.037037 -D _ENVMAP -D _LIGHTGLOW -D _DEBUG -D _IRRADIANCE -D _CASCADE -D LCLMAPS=2
# ORO patch (ae): HIS effect - LightConfiguration 2, env maps, light glow, the mesh debugger, cascades on.
# mirrors the client's D3DXSHADER_PREFER_FLOW_CONTROL, which is what spends the boolean registers.
# ORO patch (ae) round 11: Vessel mapping NONE - the client sets ShadowFilter -1, so SHDMAP is 0 and every
# "#if SHDMAP > 0" block is gone (patch (p)'s FAST line was X3004 in this configuration since 2026-08-09).
dxrun "TerrainPS Earth+DEVTOOLS"      TerrainPS ps_3_0 $DE -D_DEVTOOLS -D_LCL6 -D_LCLCUBE -D_MED
dxrun "TerrainPS Earth+DEVTOOLS+PERF" TerrainPS ps_3_0 $DE -D_DEVTOOLS -D_PERFORMANCE -D_LCL6 -D_LCLCUBE -D_MED
# (no Earth+_MICROTEX row: VPlanetAtmo's EARTH block never sets that flag - it is Mars/Moon's -
#  and the combination exceeds the 16-sampler ceiling, so it is a latent limit if it ever moves)
dxrun "TerrainPS Earth"               TerrainPS ps_3_0 $DE
dxrun "TerrainPS Mars"                TerrainPS ps_3_0 -D_LOCALLIGHTS -D_MICROTEX -D_SHDMAP -D_DEVTOOLS -D_MED -D_LCL6 -D_LCLCUBE
dxrun "TerrainPS Moon"                TerrainPS ps_3_0 -D_LOCALLIGHTS -D_MICROTEX -D_SHDMAP -D_DEVTOOLS -D_MED -D_NO_ATMOSPHERE -D_LCL6 -D_LCLCUBE
dxrun "TerrainVS Earth"               TerrainVS vs_3_0 $DE -D_DEVTOOLS
dxrun "CloudPS"                       CloudPS ps_3_0 -D_CLOUDMICRO -D_CLOUDNORMALS
dxrun "CloudPS plain"                 CloudPS ps_3_0
dxrun "CloudVS"                       CloudVS vs_3_0 -D_CLOUDMICRO -D_CLOUDNORMALS
dxrun "GiantPS"                       GiantPS ps_3_0
dxrun "GiantCloudPS"                  GiantCloudPS ps_3_0
dxrun "HorizonPS"                     HorizonPS ps_3_0
run "HorizonRingPS"               /T ps_3_0 /E HorizonRingPS NewPlanet.hlsl
run "HorizonVS"                   /T vs_3_0 /E HorizonVS NewPlanet.hlsl
run "TileShdPS"                   /T ps_3_0 /E TileShdPS $E /D_DEVTOOLS=1 NewPlanet.hlsl
run "TileDepthPS"                 /T ps_3_0 /E TileDepthPS NewPlanet.hlsl
run "TileDepthVS"                 /T vs_3_0 /E TileDepthVS NewPlanet.hlsl
exit $fail
