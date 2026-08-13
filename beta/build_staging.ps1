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
  # 2. the six deployed shaders (patched; must match the DLL)
  @("Modules\D3D9Client\D3D9Client.fx",        "Modules\D3D9Client\D3D9Client.fx"),
  @("Modules\D3D9Client\Vessel.fx",            "Modules\D3D9Client\Vessel.fx"),
  @("Modules\D3D9Client\PBR.fx",               "Modules\D3D9Client\PBR.fx"),
  @("Modules\D3D9Client\Metalness.fx",         "Modules\D3D9Client\Metalness.fx"),
  @("Modules\D3D9Client\Sketchpad.fx",         "Modules\D3D9Client\Sketchpad.fx"),
  @("Modules\D3D9Client\NewPlanet.hlsl",       "Modules\D3D9Client\NewPlanet.hlsl"),
  # 3. ORO's own runtime assets
  @("Modules\ORO\banner.bmp",                  "Modules\ORO\banner.bmp"),
  @("Modules\ORO\orofx.hlsl",                  "Modules\ORO\orofx.hlsl"),
  @("Modules\ORO\sounds\heartbeat.wav",        "Modules\ORO\sounds\heartbeat.wav"),
  @("Modules\ORO\sounds\Induce_gloc.wav",      "Modules\ORO\sounds\Induce_gloc.wav"),
  # 4. meshes and textures
  @("Meshes\ORO\DG-S.msh",                     "Meshes\ORO\DG-S.msh"),
  @("Meshes\ORO\DeltaGlider.msh",              "Meshes\ORO\DeltaGlider.msh"),
  @("Meshes\ORO\DG-S_bell.msh",                "Meshes\ORO\DG-S_bell.msh"),
  @("Meshes\ORO\DeltaGlider_bell.msh",         "Meshes\ORO\DeltaGlider_bell.msh"),
  @("Textures\ORO\bell_glow.dds",              "Textures\ORO\bell_glow.dds"),
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
