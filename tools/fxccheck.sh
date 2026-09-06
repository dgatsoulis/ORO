#!/bin/bash
# ORO: the MANDATORY fxc compile-check matrix for the patched client shaders (2026-09-05).
# Runs every entry point in every configuration the client compiles, including the
# _DEVTOOLS terrain build that sits at the ps_3_0 sampler ceiling. Run from Git Bash:
#   tools/fxccheck.sh [path-to-clone-shaders]
# Prints ok/FAIL per entry with the instruction-slot count; exit 1 on any failure.
FXC="/c/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x86/fxc.exe"
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
E="/D_WATER=1 /D_RIPPLES=1 /D_CLOUDSHD=1 /D_NIGHTLIGHTS=1 /D_LOCALLIGHTS=1 /D_SHDMAP=1"
run "D3D9Client.fx fx_2_0"        /T fx_2_0 /DANISOTROPY_MACRO=4 /DLMODE=1 /DMAX_LIGHTS=12 /DSHDMAP=3 /DKERNEL_SIZE=27 /DKERNEL_WEIGHT=0.0285 D3D9Client.fx
# ORO patch (ae): HIS effect - LightConfiguration 2, env maps, light glow, the mesh debugger, cascades on.
# mirrors the client's D3DXSHADER_PREFER_FLOW_CONTROL, which is what spends the boolean registers.
run "D3D9Client.fx fx_2_0 HIS+CASCADE" /T fx_2_0 /DANISOTROPY_MACRO=4 /DLMODE=2 /DMAX_LIGHTS=4 /DSHDMAP=3 /DKERNEL_SIZE=27 /DKERNEL_WEIGHT=0.0285 /D_ENVMAP /D_LIGHTGLOW /D_DEBUG /D_IRRADIANCE /D_CASCADE D3D9Client.fx
# ORO patch (ae) round 11: Vessel mapping NONE - the client sets ShadowFilter -1, so SHDMAP is 0 and every
# "#if SHDMAP > 0" block is gone (patch (p)'s FAST line was X3004 in this configuration since 2026-08-09).
run "D3D9Client.fx fx_2_0 NOSHADOW" /T fx_2_0 /DANISOTROPY_MACRO=4 /DLMODE=1 /DMAX_LIGHTS=12 /DSHDMAP=0 /DKERNEL_SIZE=27 /DKERNEL_WEIGHT=0.037037 D3D9Client.fx
run "D3D9Client.fx fx_2_0 HIS+NOSHADOW+CASCADE" /T fx_2_0 /DANISOTROPY_MACRO=4 /DLMODE=2 /DMAX_LIGHTS=4 /DSHDMAP=0 /DKERNEL_SIZE=27 /DKERNEL_WEIGHT=0.037037 /D_ENVMAP /D_LIGHTGLOW /D_DEBUG /D_IRRADIANCE /D_CASCADE D3D9Client.fx
run "TerrainPS Earth+DEVTOOLS"    /T ps_3_0 /E TerrainPS $E /D_DEVTOOLS=1 NewPlanet.hlsl
run "TerrainPS Earth+DEVTOOLS+PERF" /T ps_3_0 /E TerrainPS $E /D_DEVTOOLS=1 /D_PERFORMANCE=1 NewPlanet.hlsl
run "TerrainPS Earth"             /T ps_3_0 /E TerrainPS $E NewPlanet.hlsl
run "TerrainPS Mars"              /T ps_3_0 /E TerrainPS /D_LOCALLIGHTS=1 /D_MICROTEX=1 /D_SHDMAP=1 /D_DEVTOOLS=1 /D_MED=1 NewPlanet.hlsl
run "TerrainPS Moon"              /T ps_3_0 /E TerrainPS /D_LOCALLIGHTS=1 /D_MICROTEX=1 /D_SHDMAP=1 /D_DEVTOOLS=1 /D_MED=1 /D_NO_ATMOSPHERE=1 NewPlanet.hlsl
run "TerrainVS Earth"             /T vs_3_0 /E TerrainVS $E /D_DEVTOOLS=1 NewPlanet.hlsl
run "CloudPS"                     /T ps_3_0 /E CloudPS /D_CLOUDMICRO=1 /D_CLOUDNORMALS=1 NewPlanet.hlsl
run "CloudPS plain"               /T ps_3_0 /E CloudPS NewPlanet.hlsl
run "CloudVS"                     /T vs_3_0 /E CloudVS /D_CLOUDMICRO=1 /D_CLOUDNORMALS=1 NewPlanet.hlsl
run "GiantPS"                     /T ps_3_0 /E GiantPS NewPlanet.hlsl
run "GiantCloudPS"                /T ps_3_0 /E GiantCloudPS NewPlanet.hlsl
run "HorizonPS"                   /T ps_3_0 /E HorizonPS NewPlanet.hlsl
run "HorizonRingPS"               /T ps_3_0 /E HorizonRingPS NewPlanet.hlsl
run "HorizonVS"                   /T vs_3_0 /E HorizonVS NewPlanet.hlsl
run "TileShdPS"                   /T ps_3_0 /E TileShdPS $E /D_DEVTOOLS=1 NewPlanet.hlsl
run "TileDepthPS"                 /T ps_3_0 /E TileDepthPS NewPlanet.hlsl
run "TileDepthVS"                 /T vs_3_0 /E TileDepthVS NewPlanet.hlsl
exit $fail
