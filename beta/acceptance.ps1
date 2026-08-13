# ORO beta acceptance test - 8 cases against a MOCK Orbiter 2024 tree.
#
# Two traps this test exists to avoid, both on record from the PULSE run:
#  (1) drive the .bat by FULL PATH - a bare name after cd /d silently fails to resolve;
#  (2) READ THE OUTPUT. A run that never invoked the installer leaves the tree
#      untouched and passes every "nothing changed" assertion TRIVIALLY. So every
#      case asserts on a SIGNATURE STRING proving the installer actually ran.
$ErrorActionPreference = "Stop"
$SP   = "$PSScriptRoot\_testtmp"
$ZIP  = "$PSScriptRoot\dist\ORO-beta-260813.zip"
$MOCK = "$SP\mock"
$pass = 0; $fail = 0

function Reset-Mock([string]$exeDate, [bool]$withPulse) {
  if (Test-Path $MOCK) { Remove-Item $MOCK -Recurse -Force }
  New-Item -ItemType Directory -Path "$MOCK\Config\Vessels","$MOCK\Modules\Plugin","$MOCK\Modules\D3D9Client" -Force | Out-Null
  Set-Content "$MOCK\Orbiter.exe"    "mock orbiter binary"       -NoNewline
  Set-Content "$MOCK\Orbiter_ng.exe" "mock orbiter ng binary"    -NoNewline
  Set-Content "$MOCK\Config\Vessels\DeltaGlider.cfg" "mock dg cfg"
  Set-Content "$MOCK\Modules\Plugin\D3D9Client.dll"  "STOCK CLIENT DLL"
  foreach ($s in 'D3D9Client.fx','Vessel.fx','PBR.fx','Metalness.fx','Sketchpad.fx','NewPlanet.hlsl') {
    Set-Content "$MOCK\Modules\D3D9Client\$s" "STOCK SHADER $s"
  }
  if ($exeDate) {
    (Get-Item "$MOCK\Orbiter.exe").LastWriteTime    = [datetime]::ParseExact($exeDate,'yyyyMMdd',$null)
    (Get-Item "$MOCK\Orbiter_ng.exe").LastWriteTime = [datetime]::ParseExact($exeDate,'yyyyMMdd',$null)
  }
  if ($withPulse) { Set-Content "$MOCK\Modules\Plugin\PULSE.dll" "the old beta's dll" }
  Expand-Archive -Path $ZIP -DestinationPath $MOCK -Force
}

function Run-Bat([string]$bat, [string]$stdin) {
  # FULL PATH, always. Capture everything.
  $full = "$MOCK\ORO_beta\$bat"
  if (-not (Test-Path $full)) { return "!!! BAT NOT FOUND: $full" }
  return ($stdin | & cmd.exe /c "`"$full`"" 2>&1 | Out-String)
}

function Check($name, $cond, $evidence) {
  if ($cond) { $script:pass++; "  PASS  $name"; }
  else       { $script:fail++; "  FAIL  $name  <<< $evidence" }
}

"================ ORO BETA ACCEPTANCE ================"

# --- A: not an Orbiter folder ------------------------------------------------
"[A] refuses a folder that is not Orbiter"
Reset-Mock "20241231" $false
Remove-Item "$MOCK\Orbiter.exe","$MOCK\Orbiter_ng.exe" -Force
$o = Run-Bat "ORO_Install.bat" "Y"
Check "A1 installer actually ran"      ($o -match 'ORO' -and $o.Length -gt 40) "output len $($o.Length)"
Check "A2 refused"                     ($o -match 'does not look like|no Orbiter executable|Nothing has been changed') $o
Check "A3 no ORO.dll installed"        (-not (Test-Path "$MOCK\Modules\Plugin\ORO.dll")) "ORO.dll present!"
Check "A4 client untouched"            ((Get-Content "$MOCK\Modules\Plugin\D3D9Client.dll" -Raw) -match 'STOCK') "client replaced!"

# --- B: Orbiter 2016-shaped (old exe date) -----------------------------------
"[B] refuses an Orbiter 2016-dated install"
Reset-Mock "20160815" $false
$o = Run-Bat "ORO_Install.bat" "Y"
Check "B1 installer actually ran"      ($o -match 'ORO') "output len $($o.Length)"
Check "B2 refused on date"             ($o -match '2016|dated|2024-12-31') $o
Check "B3 no ORO.dll installed"        (-not (Test-Path "$MOCK\Modules\Plugin\ORO.dll")) "ORO.dll present!"
Check "B4 client untouched"            ((Get-Content "$MOCK\Modules\Plugin\D3D9Client.dll" -Raw) -match 'STOCK') "client replaced!"

# --- C: the OLD PULSE beta still installed (NEW guard) -----------------------
"[C] refuses when the old PULSE beta is still installed"
Reset-Mock "20241231" $true
$o = Run-Bat "ORO_Install.bat" "Y"
Check "C1 installer actually ran"      ($o -match 'Orbiter 2024 installation confirmed') "no confirm line"
Check "C2 named PULSE + uninstaller"   ($o -match 'PULSE' -and $o -match 'PULSE_Uninstall.bat') $o
Check "C3 no ORO.dll installed"        (-not (Test-Path "$MOCK\Modules\Plugin\ORO.dll")) "ORO.dll present!"
Check "C4 client untouched"            ((Get-Content "$MOCK\Modules\Plugin\D3D9Client.dll" -Raw) -match 'STOCK') "client replaced!"

# --- D: cancel at the prompt -------------------------------------------------
"[D] cancel leaves the tree untouched"
Reset-Mock "20241231" $false
$o = Run-Bat "ORO_Install.bat" "N"
Check "D1 installer actually ran"      ($o -match 'Orbiter 2024 installation confirmed') "no confirm line"
Check "D2 said cancelled"              ($o -match 'Cancelled') $o
Check "D3 no ORO.dll installed"        (-not (Test-Path "$MOCK\Modules\Plugin\ORO.dll")) "ORO.dll present!"
Check "D4 client untouched"            ((Get-Content "$MOCK\Modules\Plugin\D3D9Client.dll" -Raw) -match 'STOCK') "client replaced!"
Check "D5 no backup made"              (-not (Test-Path "$MOCK\ORO_beta\backup")) "backup exists!"

# --- E: the real install -----------------------------------------------------
"[E] installs"
Reset-Mock "20241231" $false
$o = Run-Bat "ORO_Install.bat" "Y"
Check "E1 installer actually ran"      ($o -match 'Orbiter 2024 installation confirmed') "no confirm line"
# NB: do not test for "**" here - the success screen legitimately prints
# "***  PLEASE READ THE README". Assert the success banner instead.
Check "E2 reported success"            ($o -match 'ORO INSTALLED' -and $o -notmatch 'FAILED|could not') $o
Check "E3 ORO.dll installed"           (Test-Path "$MOCK\Modules\Plugin\ORO.dll") "missing"
Check "E4 orofx.hlsl installed"        (Test-Path "$MOCK\Modules\ORO\orofx.hlsl") "missing"
Check "E5 banner installed"            (Test-Path "$MOCK\Modules\ORO\banner.bmp") "missing"
Check "E6 Config\ORO.cfg installed"    (Test-Path "$MOCK\Config\ORO.cfg") "missing"
Check "E7 12 body cfgs installed"      ((Get-ChildItem "$MOCK\Config\ORO\bodies\*.cfg" -EA SilentlyContinue).Count -eq 12) "count wrong"
Check "E8 meshes installed"            (Test-Path "$MOCK\Meshes\ORO\DG-S_bell.msh") "missing"
Check "E9 texture installed"           (Test-Path "$MOCK\Textures\ORO\bell_glow.dds") "missing"
Check "E10 scenarios installed"        ((Get-ChildItem "$MOCK\Scenarios\ORO_beta\*.scn" -EA SilentlyContinue).Count -eq 4) "count wrong"
Check "E11 client REPLACED by patched" ((Get-Content "$MOCK\Modules\Plugin\D3D9Client.dll" -Raw) -notmatch 'STOCK CLIENT') "still stock"
Check "E12 backup of THEIR client made" ((Get-Content "$MOCK\ORO_beta\backup\Modules\Plugin\D3D9Client.dll" -Raw) -match 'STOCK CLIENT') "backup wrong"
Check "E13 backup of their 6 shaders"  ((Get-ChildItem "$MOCK\ORO_beta\backup\Modules\D3D9Client\*" -EA SilentlyContinue).Count -eq 6) "count wrong"
Check "E14 no PULSE-named file landed" ((Get-ChildItem $MOCK -Recurse -File | Where-Object { $_.Name -match 'PULSE' }).Count -eq 0) "PULSE file present"

# --- F: double install -------------------------------------------------------
"[F] refuses a second install"
$o = Run-Bat "ORO_Install.bat" "Y"
Check "F1 installer actually ran"      ($o -match 'ORO') "output len $($o.Length)"
Check "F2 said already installed"      ($o -match 'ALREADY INSTALLED') $o
Check "F3 backup NOT overwritten"      ((Get-Content "$MOCK\ORO_beta\backup\Modules\Plugin\D3D9Client.dll" -Raw) -match 'STOCK CLIENT') "backup clobbered!"

# --- G: tuned-file preservation + uninstall ----------------------------------
"[G] uninstall restores the client and KEEPS tuned files"
Set-Content "$MOCK\Config\ORO\DeltaGlider.cfg" "PlasSat = 1.7   ; the tester tuned this"
Set-Content "$MOCK\Config\ORO\MyOwnShip.cfg"   "; a class the tester added"
$o = Run-Bat "ORO_Uninstall.bat" "Y"
Check "G1 uninstaller actually ran"    ($o -match 'ORO') "output len $($o.Length)"
Check "G2 client restored to THEIRS"   ((Get-Content "$MOCK\Modules\Plugin\D3D9Client.dll" -Raw) -match 'STOCK CLIENT') "not restored"
Check "G3 shaders restored"            ((Get-Content "$MOCK\Modules\D3D9Client\Sketchpad.fx" -Raw) -match 'STOCK SHADER') "not restored"
Check "G4 ORO.dll removed"             (-not (Test-Path "$MOCK\Modules\Plugin\ORO.dll")) "still there"
Check "G5 TUNED cfg KEPT"              (Test-Path "$MOCK\Config\ORO\DeltaGlider.cfg") "tuned file deleted!"
Check "G6 tuned content intact"        ((Get-Content "$MOCK\Config\ORO\DeltaGlider.cfg" -Raw) -match 'PlasSat = 1.7') "content changed"
Check "G7 tester's own cfg KEPT"       (Test-Path "$MOCK\Config\ORO\MyOwnShip.cfg") "deleted!"
Check "G8 untouched shipped cfg gone"  (-not (Test-Path "$MOCK\Config\ORO\Atlantis.cfg")) "left behind"

# --- H: double uninstall -----------------------------------------------------
"[H] refuses a second uninstall"
$o = Run-Bat "ORO_Uninstall.bat" "Y"
Check "H1 uninstaller actually ran"    ($o -match 'ORO') "output len $($o.Length)"
Check "H2 said not installed"          ($o -match 'not installed|NOT INSTALLED|nothing to') $o

""
"================ $pass passed, $fail failed ================"
if ($fail -gt 0) { "SOME CASES FAILED - read the FAIL lines above" }
