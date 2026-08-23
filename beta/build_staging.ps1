# Build the ORO closed-beta staging tree from the LIVE renamed install.
# Follows beta/MANIFEST.md exactly, including its DO-NOT-SHIP list. Built
# explicitly rather than by copying the PULSE staging tree, so nothing stale
# or old-named can survive by accident.
$ErrorActionPreference = "Stop"
$R    = "Z:\Orbiter-2024"
# Derived, not hardcoded: the source folder was renamed samples\PULSE -> samples\ORO
# on 2026-08-12, and a literal path here would have silently pointed at nothing.
$REPO = Split-Path $PSScriptRoot -Parent
$OUT  = "$PSScriptRoot\dist\ORO_beta"
$PAY  = "$OUT\payload"

# (source under $R, destination under $PAY)
$payload = @(
  # 1. the two DLLs
  @("Modules\Plugin\ORO.dll",                  "Modules\Plugin\ORO.dll"),
  @("Modules\Plugin\D3D9Client.dll",           "Modules\Plugin\D3D9Client.dll"),
  # 2. the SEVEN deployed shaders (patched; must match the DLL). Mesh.fx joined
  #    with patch (s)'s base-tile ground work - the 260823 audit caught it
  #    missing from this list while already being load-bearing in the sim.
  @("Modules\D3D9Client\D3D9Client.fx",        "Modules\D3D9Client\D3D9Client.fx"),
  @("Modules\D3D9Client\Vessel.fx",            "Modules\D3D9Client\Vessel.fx"),
  @("Modules\D3D9Client\PBR.fx",               "Modules\D3D9Client\PBR.fx"),
  @("Modules\D3D9Client\Metalness.fx",         "Modules\D3D9Client\Metalness.fx"),
  @("Modules\D3D9Client\Sketchpad.fx",         "Modules\D3D9Client\Sketchpad.fx"),
  @("Modules\D3D9Client\NewPlanet.hlsl",       "Modules\D3D9Client\NewPlanet.hlsl"),
  @("Modules\D3D9Client\Mesh.fx",              "Modules\D3D9Client\Mesh.fx"),
  # 3. ORO's own runtime assets. Sounds live under XRSound\ORO since 2026-08-23
  #    (the Orbiter convention); README.txt is the freesound CREDIT LEDGER and
  #    ships wherever the thunder wavs do - CC-BY requires it.
  @("Modules\ORO\banner.bmp",                  "Modules\ORO\banner.bmp"),
  @("Modules\ORO\orofx.hlsl",                  "Modules\ORO\orofx.hlsl"),
  @("XRSound\ORO\heartbeat.wav",               "XRSound\ORO\heartbeat.wav"),
  @("XRSound\ORO\Induce_gloc.wav",             "XRSound\ORO\Induce_gloc.wav"),
  @("XRSound\ORO\Rain_light.wav",              "XRSound\ORO\Rain_light.wav"),
  @("XRSound\ORO\Rain_medium.wav",             "XRSound\ORO\Rain_medium.wav"),
  @("XRSound\ORO\Rain_heavy.wav",              "XRSound\ORO\Rain_heavy.wav"),
  @("XRSound\ORO\Rain_hull.wav",               "XRSound\ORO\Rain_hull.wav"),
  @("XRSound\ORO\Thunder_close_1.wav",         "XRSound\ORO\Thunder_close_1.wav"),
  @("XRSound\ORO\Thunder_close_2.wav",         "XRSound\ORO\Thunder_close_2.wav"),
  @("XRSound\ORO\Thunder_close_3.wav",         "XRSound\ORO\Thunder_close_3.wav"),
  @("XRSound\ORO\Thunder_mid_1.wav",           "XRSound\ORO\Thunder_mid_1.wav"),
  @("XRSound\ORO\Thunder_mid_2.wav",           "XRSound\ORO\Thunder_mid_2.wav"),
  @("XRSound\ORO\Thunder_mid_3.wav",           "XRSound\ORO\Thunder_mid_3.wav"),
  @("XRSound\ORO\Thunder_far_1.wav",           "XRSound\ORO\Thunder_far_1.wav"),
  @("XRSound\ORO\Thunder_far_2.wav",           "XRSound\ORO\Thunder_far_2.wav"),
  @("XRSound\ORO\Thunder_far_3.wav",           "XRSound\ORO\Thunder_far_3.wav"),
  @("XRSound\ORO\README.txt",                  "XRSound\ORO\README.txt"),
  # 4. meshes and textures. The rain shields are HIS authored roofs (stock
  #    classes only - the XR2's stays local, the XR2Ravenstar.cfg rule);
  #    bolt_atlas.dds is the baked lightning-bolt atlas (derived work - the
  #    raw Resource Boy pack NEVER ships, only this derivative).
  @("Meshes\ORO\DG-S.msh",                     "Meshes\ORO\DG-S.msh"),
  @("Meshes\ORO\DeltaGlider.msh",              "Meshes\ORO\DeltaGlider.msh"),
  @("Meshes\ORO\DG-S_bell.msh",                "Meshes\ORO\DG-S_bell.msh"),
  @("Meshes\ORO\DeltaGlider_bell.msh",         "Meshes\ORO\DeltaGlider_bell.msh"),
  @("Meshes\ORO\DG-S_rainshield.msh",          "Meshes\ORO\DG-S_rainshield.msh"),
  @("Meshes\ORO\DeltaGlider_rainshield.msh",   "Meshes\ORO\DeltaGlider_rainshield.msh"),
  @("Textures\ORO\bell_glow.dds",              "Textures\ORO\bell_glow.dds"),
  @("Textures\ORO\bolt_atlas.dds",             "Textures\ORO\bolt_atlas.dds"),
  # 5. settings - his tuned look
  @("Config\ORO.cfg",                          "Config\ORO.cfg"),
  @("Config\ORO\Atlantis.cfg",                 "Config\ORO\Atlantis.cfg"),
  @("Config\ORO\DeltaGlider.cfg",              "Config\ORO\DeltaGlider.cfg"),
  @("Config\ORO\DG-S.cfg",                     "Config\ORO\DG-S.cfg"),
  @("Config\ORO\ProjectAlpha_ISS.cfg",         "Config\ORO\ProjectAlpha_ISS.cfg"),
  # 6. scenarios
  @("Scenarios\ORO_beta\Atlantis reentry.scn", "Scenarios\ORO_beta\Atlantis reentry.scn"),
  @("Scenarios\ORO_beta\DG reentry.scn",       "Scenarios\ORO_beta\DG reentry.scn"),
  @("Scenarios\ORO_beta\Habana Spaceport.scn", "Scenarios\ORO_beta\Habana Spaceport.scn"),
  @("Scenarios\ORO_beta\Thruster effects.scn", "Scenarios\ORO_beta\Thruster effects.scn")
)

function Put($srcFull, $dstFull) {
  if (-not (Test-Path $srcFull)) { throw "MISSING SOURCE: $srcFull" }
  $d = Split-Path $dstFull -Parent
  if (-not (Test-Path $d)) { New-Item -ItemType Directory -Path $d -Force | Out-Null }
  Copy-Item $srcFull $dstFull -Force
}

if (-not (Test-Path $OUT)) { New-Item -ItemType Directory -Path $OUT -Force | Out-Null }

# Clean the payload + stock trees first: staging only ever ADDED files, so a
# path REMOVED from the list above (the 260823 sound move) would linger from a
# previous run and ship both layouts at once. Deliberate, after exactly that
# nearly happened.
if (Test-Path $PAY)         { Remove-Item $PAY -Recurse -Force }
if (Test-Path "$OUT\stock") { Remove-Item "$OUT\stock" -Recurse -Force }

$n = 0
foreach ($p in $payload) { Put "$R\$($p[0])" "$PAY\$($p[1])"; $n++ }

# 5b. the twelve per-body aurora files
Get-ChildItem "$R\Config\ORO\bodies\*.cfg" | ForEach-Object {
  Put $_.FullName "$PAY\Config\ORO\bodies\$($_.Name)"; $script:n++
}

# 7. the restore bundle (stock originals - the fallback if a tester loses the backup)
Get-ChildItem "$REPO\upstream\stock" -File | Where-Object { $_.Extension -in '.fx','.hlsl' } | ForEach-Object {
  Put $_.FullName "$OUT\stock\Modules\D3D9Client\$($_.Name)"; $script:n++
}
Put "$R\Modules\Plugin\D3D9Client.dll.orig-241231" "$OUT\stock\Modules\Plugin\D3D9Client.dll"; $n++
Put "$REPO\upstream\stock\RESTORE.txt"             "$OUT\stock\RESTORE.txt"; $n++

# 8. installer, uninstaller, readme
Put "$REPO\beta\ORO_Install.bat"   "$OUT\ORO_Install.bat";   $n++
Put "$REPO\beta\ORO_Uninstall.bat" "$OUT\ORO_Uninstall.bat"; $n++
Put "$REPO\beta\ORO_README.txt"    "$OUT\ORO_README.txt";    $n++

"staged $n files into $OUT"
