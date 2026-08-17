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

function Reset-Mock([string]$exeDate, [bool]$withPulse, [bool]$withPulseBackup) {
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
  # A REALISTIC PULSE FOOTPRINT, not just the DLL. The installer detects seven
  # separate things because a half-finished uninstall leaves some and not others,
  # and "whatever is left" is exactly the case worth testing.
  # ⚠ The on-disk client is PULSE's PATCHED one, because PULSE required a patched
  # client - it was a hard dependency. That is what makes the poisoned-backup case
  # real: if ORO backed THAT up as "your original", a later ORO uninstall would
  # restore a patched client and truthfully report success.
  if ($withPulse) {
    New-Item -ItemType Directory -Force -Path `
      "$MOCK\Modules\PULSE","$MOCK\Config\PULSE","$MOCK\Meshes\PULSE",`
      "$MOCK\Textures\PULSE","$MOCK\Scenarios\PULSE_beta" | Out-Null
    Set-Content "$MOCK\Modules\Plugin\PULSE.dll"          "the old beta's dll"
    Set-Content "$MOCK\Modules\PULSE\pulsefx.hlsl"        "old shader"
    Set-Content "$MOCK\Config\PULSE.cfg"                  "old global settings"
    Set-Content "$MOCK\Config\PULSE\DeltaGlider.cfg"      "old class settings"
    Set-Content "$MOCK\Meshes\PULSE\DG-S_bell.msh"        "old mesh"
    Set-Content "$MOCK\Textures\PULSE\bell_glow.dds"      "old texture"
    Set-Content "$MOCK\Scenarios\PULSE_beta\demo.scn"     "old scenario"
    Set-Content "$MOCK\Modules\Plugin\D3D9Client.dll"     "PULSE PATCHED CLIENT"
  }
  if ($withPulseBackup) {
    New-Item -ItemType Directory -Force -Path `
      "$MOCK\PULSE_beta\backup\Modules\Plugin","$MOCK\PULSE_beta\backup\Modules\D3D9Client" | Out-Null
    Set-Content "$MOCK\PULSE_beta\backup\Modules\Plugin\D3D9Client.dll" "STOCK CLIENT DLL"
    foreach ($s in 'D3D9Client.fx','Vessel.fx','PBR.fx','Metalness.fx','Sketchpad.fx','NewPlanet.hlsl') {
      Set-Content "$MOCK\PULSE_beta\backup\Modules\D3D9Client\$s" "STOCK SHADER $s"
    }
  }
  Expand-Archive -Path $ZIP -DestinationPath $MOCK -Force
}

function Run-Bat([string]$bat, [string]$stdin, [string]$rmp) {
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
  # A PULSE-present install asks TWO questions - remove PULSE? then install?
  # ⚠ MEASURED 2026-08-17: PIPING TWO ANSWERS DOES NOT WORK. A `set /p` inside a
  # parenthesised block consumes the whole remaining stream rather than one line,
  # so the first prompt swallows both answers and the second reads EMPTY - which
  # silently sends the run down the DECLINE path and made every removal assertion
  # fail. Seed the environment and feed EOF instead: `set /p` overwrites the
  # variable when stdin has data and leaves it alone at EOF, so at EOF both
  # prompts read their seeded answer. Verified with a standalone probe before
  # being used here.
  $env:GO = $stdin
  $feed   = $stdin
  if ($PSBoundParameters.ContainsKey('rmp')) { $env:RMP = $rmp; $feed = "" }
  try     { return ($feed | & cmd.exe /c "`"$full`"" 2>&1 | Out-String) }
  finally { Remove-Item Env:GO,Env:RMP -ErrorAction SilentlyContinue }
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

# --- C: PULSE present, and the tester DECLINES removal -----------------------
# Their installation, their call. Declining must change NOTHING and must hand
# over the manual route - including the step that actually matters, which is
# closing Orbiter before running PULSE's own uninstaller.
"[C] PULSE present, removal declined - nothing changes"
Reset-Mock "20241231" $true $true
$o = Run-Bat "ORO_Install.bat" "Y" "N"
Check "C1 installer actually ran"      ($o -match 'Orbiter 2024 installation confirmed') "no confirm line"
Check "C2 named PULSE and offered"     ($o -match 'PULSE' -and $o -match 'Type Y to remove PULSE') $o
Check "C3 gave the manual steps"       ($o -match 'PULSE_Uninstall.bat' -and $o -match 'Close Orbiter') $o
Check "C4 warned about the lock"       ($o -match 'does not check|report success anyway') $o
Check "C5 no ORO.dll installed"        (-not (Test-Path "$MOCK\Modules\Plugin\ORO.dll")) "ORO.dll present!"
Check "C6 PULSE.dll still there"       (Test-Path "$MOCK\Modules\Plugin\PULSE.dll") "PULSE deleted without consent!"
Check "C7 PULSE settings still there"  (Test-Path "$MOCK\Config\PULSE\DeltaGlider.cfg") "settings deleted without consent!"
Check "C8 client untouched"            ((Get-Content "$MOCK\Modules\Plugin\D3D9Client.dll" -Raw) -match 'PULSE PATCHED') "client changed!"

# --- L: PULSE present, tester ACCEPTS removal, PULSE's backup survives --------
# The whole point of the 2026-08-17 rework: we renamed the addon, so cleaning up
# after that rename is OUR job, not a chore handed back to the tester along with
# a script we know can fail.
"[L] PULSE present, removal accepted - PULSE gone, ORO installed"
Reset-Mock "20241231" $true $true
$o = Run-Bat "ORO_Install.bat" "Y" "Y"
Check "L1 installer actually ran"      ($o -match 'Orbiter 2024 installation confirmed') "no confirm line"
Check "L2 said it removed PULSE"       ($o -match '\[ok\] PULSE removed') $o
Check "L3 PULSE.dll gone"              (-not (Test-Path "$MOCK\Modules\Plugin\PULSE.dll")) "PULSE.dll left behind"
Check "L4 Modules\PULSE gone"          (-not (Test-Path "$MOCK\Modules\PULSE")) "Modules\PULSE left behind"
Check "L5 Config\PULSE.cfg gone"       (-not (Test-Path "$MOCK\Config\PULSE.cfg")) "Config\PULSE.cfg left behind"
Check "L6 Config\PULSE gone"           (-not (Test-Path "$MOCK\Config\PULSE")) "Config\PULSE left behind"
Check "L7 Meshes\PULSE gone"           (-not (Test-Path "$MOCK\Meshes\PULSE")) "Meshes\PULSE left behind"
Check "L8 Textures\PULSE gone"         (-not (Test-Path "$MOCK\Textures\PULSE")) "Textures\PULSE left behind"
Check "L9 Scenarios\PULSE_beta gone"   (-not (Test-Path "$MOCK\Scenarios\PULSE_beta")) "scenarios left behind"
Check "L10 PULSE_beta folder KEPT"     (Test-Path "$MOCK\PULSE_beta\backup\Modules\Plugin\D3D9Client.dll") "we deleted their backup!"
Check "L11 ORO installed"              (Test-Path "$MOCK\Modules\Plugin\ORO.dll") "ORO.dll missing"
# ⚠ THE ONE THAT MATTERS. The client on disk was PULSE's PATCHED copy. If that
# got backed up as "your original files", a later ORO uninstall would restore a
# patched client and report success - the silent-wrongness hole found on
# 2026-08-17. The original must be recovered BEFORE the backup is taken.
Check "L12 backup is NOT the patched client" `
      ((Get-Content "$MOCK\ORO_beta\backup\Modules\Plugin\D3D9Client.dll" -Raw) -notmatch 'PULSE PATCHED') "POISONED BACKUP"
Check "L13 backup is their real original" `
      ((Get-Content "$MOCK\ORO_beta\backup\Modules\Plugin\D3D9Client.dll" -Raw) -match 'STOCK CLIENT') "backup is not stock"

# --- M: PULSE present, accepted, but PULSE's own backup is GONE --------------
# The half-uninstalled case. There is no original to recover from PULSE, so the
# pristine copies we ship have to stand in - otherwise the patched client would
# be backed up as theirs and L12's hole reopens by another route.
"[M] PULSE present, its backup gone - shipped originals stand in"
Reset-Mock "20241231" $true $false
$o = Run-Bat "ORO_Install.bat" "Y" "Y"
Check "M1 installer actually ran"      ($o -match 'Orbiter 2024 installation confirmed') "no confirm line"
Check "M2 said the backup was gone"    ($o -match "PULSE's backup is gone|shipped originals") $o
Check "M3 PULSE.dll gone"              (-not (Test-Path "$MOCK\Modules\Plugin\PULSE.dll")) "PULSE.dll left behind"
Check "M4 ORO installed"               (Test-Path "$MOCK\Modules\Plugin\ORO.dll") "ORO.dll missing"
Check "M5 backup is NOT the patched client" `
      ((Get-Content "$MOCK\ORO_beta\backup\Modules\Plugin\D3D9Client.dll" -Raw) -notmatch 'PULSE PATCHED') "POISONED BACKUP"

# --- N: only PULSE RESIDUE, no DLL - still detected --------------------------
# A previous uninstall removed the marker file and left the rest. The old guard
# keyed on PULSE.dll alone and would have sailed straight past this.
"[N] PULSE residue with no DLL is still detected"
Reset-Mock "20241231" $true $true
Remove-Item "$MOCK\Modules\Plugin\PULSE.dll" -Force
Set-Content "$MOCK\Modules\Plugin\D3D9Client.dll" "STOCK CLIENT DLL"
$o = Run-Bat "ORO_Install.bat" "Y" "Y"
Check "N1 installer actually ran"      ($o -match 'Orbiter 2024 installation confirmed') "no confirm line"
Check "N2 spotted the residue"         ($o -match 'PULSE' -and $o -match 'Type Y to remove PULSE') $o
Check "N3 residue removed"             (-not (Test-Path "$MOCK\Config\PULSE")) "Config\PULSE left behind"
Check "N4 ORO installed"               (Test-Path "$MOCK\Modules\Plugin\ORO.dll") "ORO.dll missing"

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
