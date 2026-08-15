# ORO beta acceptance test - 11 cases against a MOCK Orbiter 2024 tree.
#
# Two traps this test exists to avoid, both on record from the PULSE run:
#  (1) drive the .bat by FULL PATH - a bare name after cd /d silently fails to resolve;
#  (2) READ THE OUTPUT. A run that never invoked the installer leaves the tree
#      untouched and passes every "nothing changed" assertion TRIVIALLY. So every
#      case asserts on a SIGNATURE STRING proving the installer actually ran.
#
# CASES I, J, K WERE ADDED 2026-08-15, after a beta tester's uninstall left their
# Orbiter unbootable. This test passed 44/44 on the build that did it - because
# every case ran against a tree where the restore could always SUCCEED. The
# failure mode was never exercised, so "the client was restored" was never a
# claim under test. A test suite that only walks happy paths through a
# destructive script is not testing the thing that can hurt someone.
#
# NOT 'Stop': the scripts under test can legitimately produce native stderr, and
# PowerShell 5.1 turns a native command's stderr into a terminating error.
$ErrorActionPreference = "Continue"
$SP   = "$PSScriptRoot\_testtmp"
# newest zip, rather than a hardcoded date that silently goes stale each release
$ZIP  = (Get-ChildItem "$PSScriptRoot\dist\ORO-beta-*.zip" | Sort-Object Name -Descending | Select-Object -First 1).FullName
$MOCK = "$SP\mock"
$pass = 0; $fail = 0
"using zip: $(Split-Path $ZIP -Leaf)"

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
  # MEASURED 2026-08-15: `fc` CONSUMES PIPED STDIN - all of it. The uninstaller's
  # repair path runs `fc /b` to decide whether the client needs restoring BEFORE
  # it prompts, so a piped answer is already gone when `set /p` runs. That is an
  # artifact of piping into cmd; a user typing at a console is unaffected, and a
  # genuinely redirected run just auto-cancels, which changes nothing.
  # So seed the answer in the environment too: `set /p` overwrites it when stdin
  # has data and leaves it alone at EOF, so both routes give the same answer.
  $env:GO = $stdin
  try     { return ($stdin | & cmd.exe /c "`"$full`"" 2>&1 | Out-String) }
  finally { Remove-Item Env:GO -ErrorAction SilentlyContinue }
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
# H2 is deliberately narrow. Since 2026-08-15 there are TWO "ORO is not here"
# branches - the clean one and the repair offer - and the loose old pattern
# matched both, so it could not tell them apart. This asserts the CLEAN one.
"[H] refuses a second uninstall"
$o = Run-Bat "ORO_Uninstall.bat" "Y"
Check "H1 uninstaller actually ran"    ($o -match 'ORO') "output len $($o.Length)"
Check "H2 said nothing to uninstall"   ($o -match 'ORO IS NOT PRESENT') $o
Check "H3 offered no bogus repair"     ($o -notmatch 'REPAIRED|NOT THE ORIGINAL') $o

# --- I: THE ONE THAT WAS MISSING - a restore that cannot succeed --------------
# A tester's uninstall failed to write the client back and the script carried on
# regardless, deleting ORO and printing a green success screen. Read-only stands
# in for the real cause (Orbiter holding the file open).
"[I] a failed client restore aborts and removes NOTHING"
Reset-Mock "20241231" $false
Run-Bat "ORO_Install.bat" "Y" | Out-Null
attrib +R "$MOCK\Modules\Plugin\D3D9Client.dll"
$o = Run-Bat "ORO_Uninstall.bat" "Y"
attrib -R "$MOCK\Modules\Plugin\D3D9Client.dll"
Check "I1 uninstaller actually ran"    ($o -match 'ORO') "output len $($o.Length)"
Check "I2 stopped loudly"              ($o -match 'STOPPED|could not be restored') $o
Check "I3 did NOT claim success"       ($o -notmatch 'ORO REMOVED') $o
Check "I4 ORO.dll NOT deleted"         (Test-Path "$MOCK\Modules\Plugin\ORO.dll") "deleted after a failed restore!"
Check "I5 ORO shader NOT deleted"      (Test-Path "$MOCK\Modules\ORO\orofx.hlsl") "deleted!"
Check "I6 settings NOT deleted"        (Test-Path "$MOCK\Config\ORO.cfg") "deleted!"

# --- J: repairing an install that is already broken ---------------------------
# The state the tester was left in: ORO's files gone, client missing, Orbiter
# dying a line or two into startup. The old script said "nothing to uninstall".
"[J] repairs a half-uninstalled tree whose client is missing"
Reset-Mock "20241231" $false
Run-Bat "ORO_Install.bat" "Y" | Out-Null
Remove-Item "$MOCK\Modules\Plugin\ORO.dll","$MOCK\Modules\ORO\orofx.hlsl" -Force
Remove-Item "$MOCK\Modules\Plugin\D3D9Client.dll" -Force
$o = Run-Bat "ORO_Uninstall.bat" "Y"
Check "J1 uninstaller actually ran"    ($o -match 'ORO') "output len $($o.Length)"
Check "J2 spotted the broken client"   ($o -match 'NOT THE ORIGINAL|MISSING altogether') $o
Check "J3 repaired it"                 ($o -match 'REPAIRED') $o
Check "J4 client is back"              ((Get-Content "$MOCK\Modules\Plugin\D3D9Client.dll" -Raw) -match 'STOCK CLIENT') "not restored"

# --- K: neither script runs while Orbiter has the client open -----------------
"[K] both scripts refuse while Orbiter is running"
Reset-Mock "20241231" $false
Run-Bat "ORO_Install.bat" "Y" | Out-Null
$fake = "$SP\Orbiter.exe"
Copy-Item "$env:SystemRoot\System32\cmd.exe" $fake -Force
$proc = Start-Process $fake -ArgumentList '/c','ping -n 40 127.0.0.1 >nul' -WindowStyle Hidden -PassThru
try {
  Start-Sleep -Milliseconds 700
  $up = ((tasklist /FI "IMAGENAME eq Orbiter.exe" | Out-String) -match 'Orbiter\.exe')
  Check "K1 setup: Orbiter.exe is running" $up "tasklist cannot see it - K is inconclusive"
  $o = Run-Bat "ORO_Uninstall.bat" "Y"
  Check "K2 uninstaller refused"       ($o -match 'STILL RUNNING') $o
  Check "K3 removed nothing"           (Test-Path "$MOCK\Modules\Plugin\ORO.dll") "removed while Orbiter ran!"
  $o = Run-Bat "ORO_Install.bat" "Y"
  Check "K4 installer refused"         ($o -match 'STILL RUNNING') $o
} finally {
  if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
  Start-Sleep -Milliseconds 300
  Remove-Item $fake -Force -ErrorAction SilentlyContinue
}

""
"================ $pass passed, $fail failed ================"
# Exit on OUR result, not on $LASTEXITCODE - the last thing run is a .bat that
# correctly returns 1 when it refuses, which would otherwise report a clean
# suite as a failure.
if ($fail -gt 0) { "SOME CASES FAILED - read the FAIL lines above"; exit 1 }
exit 0
