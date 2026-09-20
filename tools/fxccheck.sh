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
# differs, and the Earth terrain shader with six maps + cube + dev tools sat at c223 of 224 until the
# 2026-09-19 register round took it to c147 (its constant structs packed and reordered). d3dxps
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

dxrunf() { # label, file, entry, profile, defines... (2026-09-19: for the shaders that are not NewPlanet.hlsl)
  local label="$1" file="$2" entry="$3" prof="$4"; shift 4
  out=$(MSYS_NO_PATHCONV=1 python "$D3DXPS" "$file" "$entry" "$prof" "$@" 2>&1)
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
  # 2026-09-20: pinned at 3950, NOT the card's declared 4096. MetalnessPS at 4080 compiled here and
  # the GTX 970's driver refused it on the device (a flat cyan hull, nothing logged); 3949 had flown.
  # The real cliff lies between the two, and the flown count is the only number worth trusting.
  out=$(MSYS_NO_PATHCONV=1 "$FXEFF" D3D9Client.fx "$@" -slots 3950 -q 2>&1); rc=$?
  # 2026-09-19: a WARNING is a failure too - the client MessageBoxes the compiler's buffer whatever
  # it holds (D3D9Effect.cpp:428), and X4121 (a gradient inside flow control) reached his sim
  # through a 27/27 matrix because fxeff prints the buffer and returns 0. X4717 stays ignored.
  if [ $rc -ne 0 ]; then echo "FAIL  $label"; echo "$out" | head -6; fail=1
  elif echo "$out" | grep -v X4717 | grep -qi 'warning X4'; then
    echo "FAIL  $label  (a warning the client boxes)"; echo "$out" | grep -v X4717 | grep -i 'warning X4' | head -4; fail=1
  else echo "ok    $label  $(echo "$out" | tail -1)"; fi
}
effrun "D3D9Client.fx 4x Partial"               -D ANISOTROPY_MACRO=4 -D LMODE=1 -D MAX_LIGHTS=4 -D SHDMAP=3 -D KERNEL_SIZE=27 -D KERNEL_WEIGHT=0.037037
effrun "D3D9Client.fx HIS 4x Full + cascade"    -D ANISOTROPY_MACRO=4 -D LMODE=2 -D MAX_LIGHTS=4 -D SHDMAP=3 -D KERNEL_SIZE=27 -D KERNEL_WEIGHT=0.037037 -D _ENVMAP -D _LIGHTGLOW -D _DEBUG -D _IRRADIANCE -D _CASCADE -D LCLMAPS=4
effrun "D3D9Client.fx MARG 8x Full + cascade + glass" -D ANISOTROPY_MACRO=4 -D LMODE=4 -D MAX_LIGHTS=8 -D SHDMAP=3 -D KERNEL_SIZE=27 -D KERNEL_WEIGHT=0.037037 -D _ENVMAP -D _LIGHTGLOW -D _DEBUG -D _IRRADIANCE -D _GLASS -D _CASCADE -D LCLMAPS=6 -D _LCLCUBE
effrun "D3D9Client.fx 8x Full + cascade + kernel 35" -D ANISOTROPY_MACRO=4 -D LMODE=4 -D MAX_LIGHTS=8 -D SHDMAP=4 -D KERNEL_SIZE=35 -D KERNEL_WEIGHT=0.0285 -D _ENVMAP -D _LIGHTGLOW -D _DEBUG -D _IRRADIANCE -D _GLASS -D _CASCADE -D LCLMAPS=6 -D _LCLCUBE
effrun "D3D9Client.fx NOSHADOW 4x Full + cascade" -D ANISOTROPY_MACRO=4 -D LMODE=2 -D MAX_LIGHTS=4 -D SHDMAP=0 -D KERNEL_SIZE=27 -D KERNEL_WEIGHT=0.037037 -D _ENVMAP -D _LIGHTGLOW -D _DEBUG -D _IRRADIANCE -D _CASCADE -D LCLMAPS=1
effrun "D3D9Client.fx 12x Full + cascade + glass"  -D ANISOTROPY_MACRO=4 -D LMODE=6 -D MAX_LIGHTS=12 -D SHDMAP=3 -D KERNEL_SIZE=27 -D KERNEL_WEIGHT=0.037037 -D _ENVMAP -D _LIGHTGLOW -D _DEBUG -D _IRRADIANCE -D _GLASS -D _CASCADE -D LCLMAPS=6 -D _LCLCUBE
# 2026-09-20: HIS EXACT ROW, built from his D3D9Client.cfg by D3D9Effect::D3D9TechInit's own rules
# (Anisotrophy 16, LightConfiguration 8 = LMODE 8 / 16 lights, ShadowMapFilter 2 = SHDMAP 3, the
# kernel is 27 always, six spot maps, the cube, glass, the mesh debugger, env maps, light glow,
# irradiance, cascades). It REPLACES the old "worst" row, whose SHDMAP 4 / kernel 35 are no
# longer reachable (the config clamps the filter to 0-2 and the kernel is packed to 27) - and
# which read 4051 where his real set reads 4080: THE ROW THAT LET THE TEAL DELTAGLIDER THROUGH.
effrun "D3D9Client.fx HIS EXACT (cfg 2026-09-20: 16x Full, cascade, glass, 6 maps, cube, debugger) - THE WORST ROW" -D ANISOTROPY_MACRO=16 -D LMODE=8 -D MAX_LIGHTS=16 -D SHDMAP=3 -D KERNEL_SIZE=27 -D KERNEL_WEIGHT=0.037037 -D LCLMAPS=6 -D _GLASS -D _DEBUG -D _ENVMAP -D _LIGHTGLOW -D _IRRADIANCE -D _CASCADE -D _LCLCUBE
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
# 2026-09-19: Scatter.hlsl is the THIRTEENTH deployed shader (AtmoParams' forty scalars packed into
# ten float4 for the terrain register round) and is also compiled on its own as the sun-colour
# image-processing entry point (VPlanetAtmo.cpp) - both of its variants belong in the matrix.
dxrunf "Scatter.hlsl SunColor"        Scatter.hlsl SunColor ps_3_0
dxrunf "Scatter.hlsl SunColor+PERF"   Scatter.hlsl SunColor ps_3_0 -D_PERFORMANCE
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
# 2026-09-20: the snow track map's stamp + fade passes (step D), the client's own compiler
dxrun "TrackStampPS"                  TrackStampPS ps_3_0
dxrun "TrackFadePS"                   TrackFadePS ps_3_0
dxrun "TrackVS"                       TrackVS vs_3_0
exit $fail
