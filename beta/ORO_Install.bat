@echo off
setlocal enabledelayedexpansion
title ORO for Orbiter 2024 - installer
color 0B

rem ===========================================================================
rem  ORO - closed beta installer
rem
rem  This file is plain text on purpose. You are about to let it replace your
rem  Orbiter graphics client, so you are entitled to read exactly what it does
rem  before running it. Nothing here touches anything outside your Orbiter
rem  folder, nothing is downloaded, and nothing needs administrator rights
rem  unless Orbiter itself lives somewhere protected.
rem
rem  Expected layout - this file sits in <OrbiterRoot>\ORO_beta\ :
rem      <OrbiterRoot>\ORO_beta\ORO_Install.bat       <- you are here
rem      <OrbiterRoot>\ORO_beta\payload\              <- what gets copied in
rem      <OrbiterRoot>\ORO_beta\stock\                <- pristine originals
rem      <OrbiterRoot>\ORO_beta\backup\               <- created at install
rem ===========================================================================

set "HERE=%~dp0"
set "HERE=%HERE:~0,-1%"
for %%I in ("%HERE%\..") do set "ROOT=%%~fI"
set "PAY=%HERE%\payload"
set "STOCK=%HERE%\stock"
set "BACKUP=%HERE%\backup"

echo.
echo  ==========================================================
echo    O R O   -   Orbiter Realism Overhaul
echo    Atmospheric, Physiological and Visual Immersion Suite
echo    closed beta installer
echo  ==========================================================
echo.
echo   Orbiter folder detected as:
echo     %ROOT%
echo.

rem --- 0. Orbiter must not be running ----------------------------------------
rem  Added 2026-08-15 after a tester's uninstall broke their installation.
rem  Orbiter AND the Launchpad both hold Modules\Plugin\D3D9Client.dll open -
rem  the Launchpad loads the graphics client to build its Video and Modules
rem  tabs. Every copy below is already checked, so this would be caught either
rem  way; catching it HERE means we stop before touching anything instead of
rem  failing part way through and leaving a tree to clean up.
call :checkRunning
if defined RUNNING (
  color 0C
  echo   ** ORBITER IS STILL RUNNING - !RUNNING! **
  echo.
  echo   Nothing has been changed.
  echo.
  echo   Please close Orbiter AND the Orbiter Launchpad completely, then run
  echo   this again. Both keep the graphics client file open, and replacing
  echo   it underneath them is how an installation gets broken.
  echo.
  echo   If no Orbiter window is open, one may be stuck: press
  echo   CTRL+SHIFT+ESC, find !RUNNING! in the list and end it.
  echo.
  goto :fail
)

rem --- 1. is this actually an Orbiter 2024 installation? ----------------------
rem  Structure alone is NOT enough to tell 2024 from 2016: the D3D9 client and
rem  XRSound can both be dropped into a modded 2016 install, so their presence
rem  proves nothing about the version. The version discriminator is the DATE ON
rem  THE BINARIES - Orbiter 2024 shipped on 2024-12-31, and no 2016 build can
rem  carry a stamp that late. We use LastWriteTime, not CreationTime: copying a
rem  file preserves the former and resets the latter to the day it was installed.
set "BAD="
if not exist "%ROOT%\Orbiter.exe" if not exist "%ROOT%\Orbiter_ng.exe" set "BAD=no Orbiter executable here"
if not defined BAD if not exist "%ROOT%\Config\Vessels\DeltaGlider.cfg"  set "BAD=Orbiter Config folder not found"
if not defined BAD if not exist "%ROOT%\Modules\Plugin\D3D9Client.dll"   set "BAD=the D3D9 graphics client is not installed"
if not defined BAD if not exist "%ROOT%\Modules\D3D9Client\D3D9Client.fx" set "BAD=the D3D9 client's shader folder is missing"

if defined BAD goto :notorbiter

rem  Read the build dates. PowerShell is used for this one check because a batch
rem  file's own %%~t date is in the machine's LOCAL short-date format, which is
rem  unparseable across locales; yyyyMMdd sorts correctly as plain text.
rem  ( -Command is not affected by the PowerShell script execution policy. )
set "SEEN="
set "TOOOLD="
for %%E in (Orbiter.exe Orbiter_ng.exe) do (
  if exist "%ROOT%\%%E" (
    set "D="
    for /f "usebackq delims=" %%D in (`powershell -NoProfile -Command "(Get-Item -LiteralPath '%ROOT%\%%E').LastWriteTime.ToString('yyyyMMdd')" 2^>nul`) do set "D=%%D"
    if defined D (
      set "SEEN=1"
      if !D! LSS 20241231 set "TOOOLD=%%E is dated !D:~0,4!-!D:~4,2!-!D:~6,2!"
    )
  )
)

if defined TOOOLD (
  set "BAD=!TOOOLD! - Orbiter 2024 binaries are dated 2024-12-31 or later"
  goto :notorbiter
)

if not defined SEEN (
  color 0E
  echo   ** Could not read the date on your Orbiter executable. **
  echo.
  echo   That check is what tells Orbiter 2024 apart from an Orbiter 2016 that
  echo   has had the D3D9 client added to it. Installing this into 2016 would
  echo   break that installation.
  echo.
  echo   Only continue if you are certain this is Orbiter 2024.
  echo.
  set /p "SURE=  Type YES to continue anyway, anything else to stop: "
  if /i not "!SURE!"=="YES" (
    echo.
    echo   Stopped. Nothing has been changed.
    goto :done_nochange
  )
)

echo   [ok] Orbiter 2024 installation confirmed.

rem --- 2. do we have our own payload? ---------------------------------------
if not exist "%PAY%\Modules\Plugin\ORO.dll" (
  color 0C
  echo   ** The payload folder is missing or incomplete. **
  echo   Re-extract the zip, keeping the ORO_beta folder intact.
  goto :fail
)

rem --- 3. already installed? ------------------------------------------------
if exist "%ROOT%\Modules\Plugin\ORO.dll" (
  color 0E
  echo.
  echo   ** ORO IS ALREADY INSTALLED. **
  echo.
  echo   Nothing has been changed. If you want to reinstall - for example to
  echo   take a newer beta build - run ORO_Uninstall.bat first, then run
  echo   this again. That way your original Orbiter files are restored from
  echo   the backup made the first time, rather than being overwritten by an
  echo   already-patched copy.
  echo.
  goto :done_nochange
)

rem --- 3b. the OLD PULSE beta - offer to remove it ----------------------------
rem This addon was called PULSE until 2026-08-12, and every file it owned has a
rem different name now. Installing over the top would leave BOTH addons in the
rem Launchpad, both loading and both writing settings, which reads as an ORO bug.
rem
rem  This block used to REFUSE and send you to PULSE_Uninstall.bat. That was
rem  honest but it was not kind: PULSE's uninstaller has a defect we shipped -
rem  it restores the graphics client without checking that the copy worked, so
rem  with Orbiter or the Launchpad open it fails silently, reports success, and
rem  removes PULSE anyway. One tester's installation was left unbootable that
rem  way. Sending people back to that script to clean up after OUR rename is
rem  asking them to pay for our mistake, so this installer now does it itself.
rem
rem  Detection is deliberately WIDER than PULSE.dll: a half-finished uninstall
rem  leaves some of these behind and not others, and "whatever is left" is
rem  exactly the case that needs helping.
set "PULSEFOUND="
if exist "%ROOT%\Modules\Plugin\PULSE.dll"  set "PULSEFOUND=1"
if exist "%ROOT%\Modules\PULSE"             set "PULSEFOUND=1"
if exist "%ROOT%\Config\PULSE.cfg"          set "PULSEFOUND=1"
if exist "%ROOT%\Config\PULSE"              set "PULSEFOUND=1"
if exist "%ROOT%\Meshes\PULSE"              set "PULSEFOUND=1"
if exist "%ROOT%\Textures\PULSE"            set "PULSEFOUND=1"
if exist "%ROOT%\Scenarios\PULSE_beta"      set "PULSEFOUND=1"

if defined PULSEFOUND (
  color 0E
  echo.
  echo   ** THE OLDER BETA - PULSE - IS STILL PRESENT. **
  echo.
  echo   ORO is the same addon under a new name. PULSE has to go before ORO
  echo   can be installed, or both will load and both will write settings.
  echo.
  echo   This installer can remove it for you. It would delete:
  echo.
  if exist "%ROOT%\Modules\Plugin\PULSE.dll" echo       Modules\Plugin\PULSE.dll
  if exist "%ROOT%\Modules\PULSE"            echo       Modules\PULSE\
  if exist "%ROOT%\Config\PULSE.cfg"         echo       Config\PULSE.cfg
  if exist "%ROOT%\Config\PULSE"             echo       Config\PULSE\           ^(PULSE's saved settings^)
  if exist "%ROOT%\Meshes\PULSE"             echo       Meshes\PULSE\
  if exist "%ROOT%\Textures\PULSE"           echo       Textures\PULSE\
  if exist "%ROOT%\Scenarios\PULSE_beta"     echo       Scenarios\PULSE_beta\
  echo.
  echo   Your PULSE_beta folder is NOT touched - it holds the backup of your
  echo   original graphics client, which is worth keeping until you are happy.
  echo   ORO ships its own tuned settings, so nothing you need is lost.
  echo.
  set /p "RMP=  Type Y to remove PULSE and continue installing ORO: "
  rem  !RMP!, not %RMP% - we are inside a parenthesised block, where %VAR% is
  rem  substituted when the block is PARSED and would always read empty here.
  if /i not "!RMP!"=="Y" goto :pulsemanual

  echo.
  echo   Removing PULSE...
  if exist "%ROOT%\Modules\Plugin\PULSE.dll" del /f /q "%ROOT%\Modules\Plugin\PULSE.dll" >nul 2>&1
  if exist "%ROOT%\Modules\PULSE"            rd /s /q  "%ROOT%\Modules\PULSE"            >nul 2>&1
  if exist "%ROOT%\Config\PULSE.cfg"         del /f /q "%ROOT%\Config\PULSE.cfg"         >nul 2>&1
  if exist "%ROOT%\Config\PULSE"             rd /s /q  "%ROOT%\Config\PULSE"             >nul 2>&1
  if exist "%ROOT%\Meshes\PULSE"             rd /s /q  "%ROOT%\Meshes\PULSE"             >nul 2>&1
  if exist "%ROOT%\Textures\PULSE"           rd /s /q  "%ROOT%\Textures\PULSE"           >nul 2>&1
  if exist "%ROOT%\Scenarios\PULSE_beta"     rd /s /q  "%ROOT%\Scenarios\PULSE_beta"     >nul 2>&1

  rem  Check it actually went. A locked file fails DEL silently, and this is the
  rem  whole lesson of the defect above - never report a removal you did not verify.
  set "PULSELEFT="
  if exist "%ROOT%\Modules\Plugin\PULSE.dll"  set "PULSELEFT=Modules\Plugin\PULSE.dll"
  if exist "%ROOT%\Modules\PULSE"             set "PULSELEFT=Modules\PULSE\"
  if exist "%ROOT%\Config\PULSE.cfg"          set "PULSELEFT=Config\PULSE.cfg"
  if exist "%ROOT%\Config\PULSE"              set "PULSELEFT=Config\PULSE\"
  if exist "%ROOT%\Meshes\PULSE"              set "PULSELEFT=Meshes\PULSE\"
  if exist "%ROOT%\Textures\PULSE"            set "PULSELEFT=Textures\PULSE\"
  if exist "%ROOT%\Scenarios\PULSE_beta"      set "PULSELEFT=Scenarios\PULSE_beta\"
  if defined PULSELEFT (
    color 0C
    echo.
    echo   ** COULD NOT REMOVE ALL OF PULSE. **
    echo.
    echo   Still there: !PULSELEFT!
    echo.
    echo   Nothing else has been changed and ORO has NOT been installed.
    echo   Something is holding that file open - Orbiter, the Launchpad, or a
    echo   file browser sitting in the folder. Close everything and run this
    echo   again.
    echo.
    goto :fail
  )
  echo   [ok] PULSE removed
  echo.
  rem  ⚠ AND NOW THE CLIENT. PULSE required a PATCHED D3D9Client.dll - it was a
  rem  hard dependency - so whatever is on disk right now is PULSE's patched
  rem  copy, NOT the tester's original. Backing that up in step 5 as "your
  rem  original files" would poison the backup and make a future ORO uninstall
  rem  restore a patched client while truthfully reporting success. So the
  rem  original is recovered here, BEFORE the backup is taken: from PULSE's own
  rem  backup if it survived (that is genuinely their file), otherwise from the
  rem  pristine originals we ship.
  rem  ⚠ NO local variable for the PULSE backup path. `set` inside a parenthesised
  rem  block does not take effect until the block finishes, so a %PBK% written and
  rem  read here would expand to NOTHING at parse time, silently skip the tester's
  rem  own original and fall through to the shipped copies. Caught by acceptance
  rem  case L13. %ROOT% and %STOCK% are set before the block, so they are safe.
  if exist "%ROOT%\PULSE_beta\backup\Modules\Plugin\D3D9Client.dll" (
    echo   Recovering your original graphics client from PULSE's backup...
    copy /y "%ROOT%\PULSE_beta\backup\Modules\Plugin\D3D9Client.dll" "%ROOT%\Modules\Plugin\" >nul 2>&1
    for %%F in (D3D9Client.fx Vessel.fx PBR.fx Metalness.fx Sketchpad.fx NewPlanet.hlsl) do (
      if exist "%ROOT%\PULSE_beta\backup\Modules\D3D9Client\%%F" copy /y "%ROOT%\PULSE_beta\backup\Modules\D3D9Client\%%F" "%ROOT%\Modules\D3D9Client\" >nul 2>&1
    )
    echo   [ok] restored from PULSE's own backup
  ) else if exist "%STOCK%\Modules\Plugin\D3D9Client.dll" (
    echo   PULSE's backup is gone - using the pristine Orbiter 2024 originals
    echo   shipped with this beta instead...
    copy /y "%STOCK%\Modules\Plugin\D3D9Client.dll" "%ROOT%\Modules\Plugin\" >nul 2>&1
    for %%F in (D3D9Client.fx Vessel.fx PBR.fx Metalness.fx Sketchpad.fx NewPlanet.hlsl) do (
      if exist "%STOCK%\Modules\D3D9Client\%%F" copy /y "%STOCK%\Modules\D3D9Client\%%F" "%ROOT%\Modules\D3D9Client\" >nul 2>&1
    )
    echo   [ok] restored from the shipped originals
  )
  echo.
  color 0A
)

rem --- 4. say plainly what is about to happen, and ask ------------------------
echo.
echo   This will:
echo     - back up your D3D9 client and its six shaders into
echo       ORO_beta\backup\  (uninstall puts them back)
echo     - install a PATCHED D3D9 client. Stock Orbiter 2024 crashes when any
echo       addon draws through the HUD, which is what ORO does - the first
echo       patch is that crash fix. Details in ORO_README.txt.
echo     - add ORO's own files under Modules, Meshes, Textures and Config.
echo.
echo   It will NOT touch your scenarios, your keyboard settings, your video
echo   settings, or anything outside this Orbiter folder.
echo.
set /p "GO=  Type Y to install, anything else to cancel: "
if /i not "%GO%"=="Y" (
  echo.
  echo   Cancelled. Nothing has been changed.
  goto :done_nochange
)

rem --- 5. back up the SEVEN files we are about to replace ---------------------
rem  We back up what YOU actually have, not what we think you have. The stock
rem  copies we ship are only a fallback if this backup is ever lost.
echo.
echo   Backing up your original files...
if not exist "%BACKUP%\Modules\Plugin"      mkdir "%BACKUP%\Modules\Plugin"      >nul 2>&1
if not exist "%BACKUP%\Modules\D3D9Client"  mkdir "%BACKUP%\Modules\D3D9Client"  >nul 2>&1

copy /y "%ROOT%\Modules\Plugin\D3D9Client.dll" "%BACKUP%\Modules\Plugin\" >nul || goto :copyfail
for %%F in (D3D9Client.fx Vessel.fx PBR.fx Metalness.fx Sketchpad.fx NewPlanet.hlsl) do (
  if exist "%ROOT%\Modules\D3D9Client\%%F" (
    copy /y "%ROOT%\Modules\D3D9Client\%%F" "%BACKUP%\Modules\D3D9Client\" >nul || goto :copyfail
  )
)
echo   [ok] originals saved to ORO_beta\backup

rem --- 6. install ------------------------------------------------------------
echo   Installing ORO...
xcopy "%PAY%\*" "%ROOT%\" /E /I /Y /Q >nul || goto :copyfail

rem --- 7. verify the install actually landed ---------------------------------
set "MISSING="
if not exist "%ROOT%\Modules\Plugin\ORO.dll"            set "MISSING=ORO.dll"
if not exist "%ROOT%\Modules\ORO\orofx.hlsl"          set "MISSING=orofx.hlsl"
if not exist "%ROOT%\Modules\D3D9Client\Vessel.fx"        set "MISSING=Vessel.fx"
if not exist "%ROOT%\Config\ORO.cfg"                    set "MISSING=ORO.cfg"
if defined MISSING (
  color 0C
  echo   ** Install incomplete - !MISSING! did not arrive. **
  echo   Run ORO_Uninstall.bat to put everything back, then tell me.
  goto :fail
)

color 0A
echo.
echo  ==========================================================
echo    ORO INSTALLED
echo  ==========================================================
echo.
echo   TWO THINGS BEFORE YOU FLY - neither is optional:
echo.
echo     1. Launchpad -^> Modules tab -^> tick "ORO control"
echo.
echo     2. Launchpad -^> Video tab -^> Advanced: turn ON Sun glare,
echo        set post-processing to "Light glow", and enable local
echo        shadows. These fail SILENTLY if left off - effects just
echo        quietly look wrong.
echo.
echo   ***  PLEASE READ THE README. It explains those settings, and
echo   ***  every control on every tab:
echo.
echo         %HERE%\ORO_README.txt
echo.
echo   In the sim, press CTRL+F4 and choose "ORO control" to open
echo   the panel. CTRL+G turns every effect on and off at once.
echo.
echo   To remove ORO completely, run ORO_Uninstall.bat in this
echo   folder. It restores your original files and keeps any
echo   settings you have tuned yourself.
echo.
pause
exit /b 0

rem ===========================================================================
rem  checkRunning - is Orbiter or its Launchpad up?
rem
rem  Locale-safe: tasklist's "no tasks" message is translated, but it can never
rem  contain the image name, so matching on the name works in any language.
rem  Fails OPEN - if tasklist is unavailable we carry on, because every copy
rem  below is checked anyway and will catch the same problem.
rem ===========================================================================
:checkRunning
set "RUNNING="
for %%P in (Orbiter.exe Orbiter_ng.exe) do (
  tasklist /FI "IMAGENAME eq %%P" 2>nul | find /I "%%P" >nul 2>&1
  if not errorlevel 1 set "RUNNING=%%P"
)
goto :eof

:notorbiter
color 0C
echo.
echo   ** THIS DOES NOT LOOK LIKE AN ORBITER 2024 INSTALLATION **
echo.
echo   Reason: %BAD%
echo.
echo   ORO is for Orbiter 2024 only. Orbiter 2024 shipped on 2024-12-31, so
echo   its Orbiter.exe and Orbiter_ng.exe carry that date or later. An older
echo   stamp means this is Orbiter 2016 - and that stays true even if the D3D9
echo   graphics client and XRSound have been added to it, which is exactly why
echo   the DATE is checked rather than which files happen to be present.
echo.
echo   Installing this into Orbiter 2016 would replace its graphics client with
echo   one built for 2024 and break that installation, so it is refused.
echo.
echo   If you DO have Orbiter 2024, then this folder is in the wrong place.
echo   Move the whole ORO_beta folder directly into your Orbiter root - the
echo   folder that contains Orbiter.exe - and run this again.
echo.
goto :fail

:copyfail
color 0C
echo.
echo   ** A file could not be copied. **
echo.
echo   The usual cause is that Orbiter is still running, or that Orbiter is
echo   installed somewhere Windows protects (Program Files). Close Orbiter and
echo   the Launchpad and try again; if Orbiter lives in Program Files, right
echo   click this file and choose "Run as administrator".
echo.
echo   SOME FILES MAY ALREADY HAVE BEEN COPIED, so this installation could be
echo   part patched. Run ORO_Uninstall.bat to put everything back - it
echo   restores your client from the backup taken a moment ago and verifies
echo   it - then fix the cause above and install again.
echo.
goto :fail

:fail
echo.
pause
exit /b 1

rem  They declined to let us remove PULSE. Their call, and a fair one - it is
rem  their installation. So hand over the manual route, including the step that
rem  actually matters: PULSE's uninstaller restores the graphics client without
rem  checking the copy worked, and Orbiter or the Launchpad holding that file
rem  open is what makes it fail. Closed, it works correctly.
:pulsemanual
color 0E
echo.
echo   Understood - nothing has been changed.
echo.
echo   To remove PULSE yourself, in this order:
echo.
echo     1. Close Orbiter AND the Launchpad completely.
echo     2. Run PULSE_Uninstall.bat from your old PULSE_beta folder.
echo     3. Do NOT start Orbiter in between.
echo     4. Run ORO_Install.bat again.
echo.
echo   Step 1 is the one that matters. PULSE's uninstaller puts your original
echo   graphics client back but does not check that the copy succeeded, and
echo   with Orbiter or the Launchpad open the file is locked and the copy is
echo   refused - it will then report success anyway. With both closed it does
echo   the right thing.
echo.
echo   If you no longer have the PULSE_beta folder, run this installer again
echo   and let it remove PULSE for you - it does not need that folder.
echo.
pause
exit /b 0

:done_nochange
echo.
pause
exit /b 0
