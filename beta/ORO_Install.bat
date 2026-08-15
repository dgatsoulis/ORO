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

rem --- 3b. the OLD PULSE beta is still installed? -----------------------------
rem This addon was called PULSE until 2026-08-12. Every file it owned has a
rem different name now, so this installer cannot see them, cannot back them up
rem and cannot remove them. Installing over the top leaves BOTH addons in the
rem Launchpad, both loading, both writing settings - which reads as an ORO bug.
if exist "%ROOT%\Modules\Plugin\PULSE.dll" (
  color 0E
  echo.
  echo   ** THE OLDER BETA - PULSE - IS STILL INSTALLED. **
  echo.
  echo   Nothing has been changed. ORO is the same addon under a new name, so
  echo   please remove PULSE first:
  echo.
  echo     1. Find your PULSE_beta folder and run PULSE_Uninstall.bat
  echo     2. Then run this installer again.
  echo.
  echo   That restores your original graphics client from the backup PULSE
  echo   made, and keeps anything you tuned.
  echo.
  echo   If you no longer have the PULSE_beta folder, delete this file by hand
  echo   and run this again:
  echo     %ROOT%\Modules\Plugin\PULSE.dll
  echo.
  goto :done_nochange
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

:done_nochange
echo.
pause
exit /b 0
