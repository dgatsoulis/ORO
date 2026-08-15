@echo off
setlocal enabledelayedexpansion
title ORO for Orbiter 2024 - uninstaller
color 0B

rem ===========================================================================
rem  ORO - closed beta uninstaller
rem
rem  Restores the Orbiter files ORO replaced and removes what it added.
rem
rem  THE RULE FOR YOUR SETTINGS: a file is deleted only if it is byte-for-byte
rem  what ORO shipped. Anything you added, and anything you tuned and saved,
rem  is KEPT and listed at the end. So if you spent an evening dialling in the
rem  DeltaGlider's plasma, uninstalling will not throw that away.
rem
rem  THE RULE FOR YOUR GRAPHICS CLIENT (added 2026-08-15, after a beta report):
rem  the client is put back, then READ BACK AND COMPARED byte for byte, and
rem  NOTHING IS DELETED until that comparison passes. The previous version
rem  copied the client without checking the result and carried on regardless -
rem  so a copy that failed, or was interrupted while Orbiter still had the file
rem  open, left a tester with an Orbiter that would not start, underneath a
rem  green screen saying the uninstall had worked. If the restore cannot be
rem  verified now, this script stops and changes nothing at all, which leaves
rem  ORO installed and working rather than leaving you with neither.
rem ===========================================================================

set "HERE=%~dp0"
set "HERE=%HERE:~0,-1%"
for %%I in ("%HERE%\..") do set "ROOT=%%~fI"
set "PAY=%HERE%\payload"
set "STOCK=%HERE%\stock"
set "BACKUP=%HERE%\backup"
set /a KEPT=0
set "CVFAIL="

echo.
echo  ==========================================================
echo    O R O   -   uninstaller
echo  ==========================================================
echo.
echo   Orbiter folder detected as:
echo     %ROOT%
echo.

rem --- 0. Orbiter must not be running ---------------------------------------
rem  This is the check whose absence broke a tester's installation on
rem  2026-08-15. Orbiter AND the Launchpad both hold
rem  Modules\Plugin\D3D9Client.dll open - the Launchpad loads the graphics
rem  client to build its Video and Modules tabs. Writing over a file in that
rem  state either fails outright or leaves a partly written DLL, and a
rem  truncated graphics client stops Orbiter before it can report anything:
rem  the Launchpad simply dies a line or two into startup.
call :checkRunning
if defined RUNNING (
  color 0C
  echo   ** ORBITER IS STILL RUNNING - !RUNNING! **
  echo.
  echo   Nothing has been changed.
  echo.
  echo   Please close Orbiter AND the Orbiter Launchpad completely, then run
  echo   this again. Both of them keep the graphics client file open, and
  echo   replacing it underneath them is how an installation gets broken.
  echo.
  echo   If no Orbiter window is open, one may be stuck: press
  echo   CTRL+SHIFT+ESC, find !RUNNING! in the list and end it.
  echo.
  goto :fail
)

rem --- 1. does this even look like the Orbiter folder? -----------------------
rem  The installer checks this carefully; here we only need enough to be sure
rem  we are not about to copy files into somewhere unrelated because the
rem  ORO_beta folder was moved out of the Orbiter root.
set "BAD="
if not exist "%ROOT%\Orbiter.exe" if not exist "%ROOT%\Orbiter_ng.exe" set "BAD=1"
if not exist "%ROOT%\Modules\Plugin" set "BAD=1"
if defined BAD (
  color 0C
  echo   ** This does not look like an Orbiter installation. **
  echo.
  echo   Nothing has been changed.
  echo.
  echo   This uninstaller expects to sit in a folder directly inside your
  echo   Orbiter root - the folder that contains Orbiter.exe. Move the whole
  echo   ORO_beta folder back there and run this again.
  echo.
  goto :fail
)

rem --- 2. where would we restore the client FROM? ----------------------------
rem  Decided up front, because the repair path in step 3 needs to know whether
rem  a restore is even possible before it can offer one.
rem  Prefer the backup made at install time - that is YOUR original file. The
rem  shipped stock copies are the fallback if that backup is gone.
set "SRC="
set "SRCWHAT="
if exist "%BACKUP%\Modules\Plugin\D3D9Client.dll" (
  set "SRC=%BACKUP%"
  set "SRCWHAT=the install-time backup of your own files"
) else if exist "%STOCK%\Modules\Plugin\D3D9Client.dll" (
  set "SRC=%STOCK%"
  set "SRCWHAT=the pristine Orbiter 2024 originals shipped with this beta"
) else if exist "%STOCK%\Modules\D3D9Client\D3D9Client.fx" (
  set "SRC=%STOCK%"
  set "SRCWHAT=the pristine shaders shipped with this beta - no client DLL"
)

rem --- 3. is ORO actually here? ---------------------------------------------
rem  The DLL and the shader are the definitive markers. Settings deliberately
rem  SURVIVE an uninstall if you changed them, so their presence does not mean
rem  ORO is installed - checking for those would make a second uninstall
rem  claim ORO was still here.
set "FOUND="
if exist "%ROOT%\Modules\Plugin\ORO.dll"   set "FOUND=1"
if exist "%ROOT%\Modules\ORO\orofx.hlsl"   set "FOUND=1"

if not defined FOUND goto :notinstalled

echo   ORO found. This will restore your original D3D9 client and its
echo   shaders, and remove ORO's files. Settings you have changed
echo   yourself will be kept and listed.
echo.
set /p "GO=  Type Y to uninstall, anything else to cancel: "
if /i not "%GO%"=="Y" (
  echo.
  echo   Cancelled. Nothing has been changed.
  goto :done
)

rem --- 4. restore the client + its six shaders, AND CHECK THAT IT WORKED -----
rem  Every copy is verified by reading the file back and comparing it to the
rem  source. That catches all three ways this can go wrong: the copy refused
rem  (destination locked, so the old patched file is still sitting there), the
rem  copy truncated, and the destination folder not being where we thought.
echo.
if not defined SRC (
  color 0C
  echo   ** Could not find anything to restore the D3D9 client from. **
  echo.
  echo   Nothing has been changed - ORO is still installed and still works.
  echo.
  echo   Both the install-time backup and the shipped stock copies are
  echo   missing from this ORO_beta folder. Re-extract the zip next to this
  echo   file and run this again; if you no longer have the zip, installing
  echo   Orbiter 2024 over the top restores the client and the shaders and
  echo   leaves your scenarios and settings alone.
  echo.
  goto :fail
)

echo   Restoring your graphics client from %SRCWHAT%...

if exist "%SRC%\Modules\Plugin\D3D9Client.dll" (
  call :copyVerify "%SRC%\Modules\Plugin\D3D9Client.dll" "%ROOT%\Modules\Plugin" "D3D9Client.dll"
  if defined CVFAIL goto :restorefailed
  echo   [ok] D3D9Client.dll restored and verified
)

for %%F in (D3D9Client.fx Vessel.fx PBR.fx Metalness.fx Sketchpad.fx NewPlanet.hlsl) do (
  if exist "%SRC%\Modules\D3D9Client\%%F" (
    call :copyVerify "%SRC%\Modules\D3D9Client\%%F" "%ROOT%\Modules\D3D9Client" "%%F"
    if defined CVFAIL goto :restorefailed
  )
)
echo   [ok] shaders restored and verified

rem --- 5. only NOW remove ORO's own files, sparing anything you changed ------
rem  Deliberately after the verified restore. If anything above had failed we
rem  would have stopped with ORO intact, which is a state you can fly in and
rem  retry from - unlike a half-removed one.
echo.
echo   Removing ORO files...

if not exist "%PAY%" (
  color 0E
  echo.
  echo   NOTE: the payload folder is gone, so ORO cannot tell which of your
  echo   settings files are its own and which are yours. Everything under
  echo   Config\ORO will be KEPT to be safe. Delete it by hand if you want
  echo   it gone.
  echo.
)

del /q "%ROOT%\Modules\Plugin\ORO.dll" >nul 2>&1
call :cleanTree "Modules\ORO"
call :cleanTree "Meshes\ORO"
call :cleanTree "Textures\ORO"
call :cleanTree "Scenarios\ORO_beta"
call :cleanTree "Config\ORO"
call :cleanFile "Config\ORO.cfg"

set "LEFT="
if exist "%ROOT%\Modules\Plugin\ORO.dll" set "LEFT=1"

echo.
color 0A
echo  ==========================================================
echo    ORO REMOVED
echo  ==========================================================
echo.
if defined LEFT (
  color 0E
  echo   NOTE: Modules\Plugin\ORO.dll could not be deleted. It is harmless
  echo   once unticked in the Launchpad Modules tab, but you can delete it
  echo   by hand. If it refuses, something still has it open.
  echo.
)
if %KEPT% GTR 0 (
  color 0E
  echo   %KEPT% file^(s^) were KEPT because you added or changed them - they are
  echo   listed above. They do nothing without ORO installed, and they will
  echo   be picked up again if you reinstall it.
  echo.
)
echo   Your original D3D9 graphics client is back, and was checked byte for
echo   byte after being written. Untick "ORO control" in the Launchpad
echo   Modules tab if it is still listed.
echo.
echo   This ORO_beta folder can be deleted whenever you like - but if you
echo   might reinstall, keeping it keeps your install-time backup too.
echo.
goto :done

rem ===========================================================================
rem  notinstalled - ORO's markers are absent.
rem
rem  This used to just say "nothing to uninstall" and exit, which is wrong for
rem  the one case that matters most: a PREVIOUS uninstall that removed ORO's
rem  files but failed to put the graphics client back. That tree has no ORO
rem  markers, so the old script refused to help exactly the person who needed
rem  it. Now we look at the client itself and offer to repair it.
rem ===========================================================================
:notinstalled
set "NEEDCLIENT="
if defined SRC (
  if exist "%SRC%\Modules\Plugin\D3D9Client.dll" (
    rem  fc reports a difference for a patched client, a truncated one, and a
    rem  missing one alike - all three want the same repair.
    fc /b "%SRC%\Modules\Plugin\D3D9Client.dll" "%ROOT%\Modules\Plugin\D3D9Client.dll" >nul 2>&1
    if errorlevel 1 set "NEEDCLIENT=1"
  )
)

if not defined NEEDCLIENT (
  color 0E
  echo   ** ORO IS NOT PRESENT - nothing to uninstall. **
  echo.
  echo   Either it was never installed in this Orbiter folder, or it has
  echo   already been removed. Your graphics client is the original one.
  echo   Nothing has been changed.
  echo.
  if exist "%ROOT%\Config\ORO" (
    echo   Note: settings of your own are still in Config\ORO - they were kept
    echo   on purpose by an earlier uninstall. They do nothing, and they will be
    echo   picked up again if you reinstall. Delete that folder if you want them
    echo   gone for good.
    echo.
  )
  goto :done
)

color 0E
echo   ** ORO IS NOT INSTALLED, BUT YOUR GRAPHICS CLIENT IS NOT THE ORIGINAL. **
echo.
if not exist "%ROOT%\Modules\Plugin\D3D9Client.dll" (
  echo   In fact Modules\Plugin\D3D9Client.dll is MISSING altogether. That is
  echo   why Orbiter will not start: it dies while loading its graphics client,
  echo   only a line or two into the log, before it can tell you why.
  echo.
) else (
  echo   That means an earlier uninstall removed ORO's files but did not
  echo   manage to put your client back - or the file it wrote was damaged.
  echo   A damaged client is why Orbiter would stop starting, dying only a
  echo   line or two into the log before it can tell you why.
  echo.
)
echo   This can be repaired right now from %SRCWHAT%.
echo.
set /p "GO=  Type Y to restore the graphics client, anything else to cancel: "
if /i not "%GO%"=="Y" (
  echo.
  echo   Cancelled. Nothing has been changed.
  goto :done
)

echo.
call :copyVerify "%SRC%\Modules\Plugin\D3D9Client.dll" "%ROOT%\Modules\Plugin" "D3D9Client.dll"
if defined CVFAIL goto :restorefailed
echo   [ok] D3D9Client.dll restored and verified

for %%F in (D3D9Client.fx Vessel.fx PBR.fx Metalness.fx Sketchpad.fx NewPlanet.hlsl) do (
  if exist "%SRC%\Modules\D3D9Client\%%F" (
    call :copyVerify "%SRC%\Modules\D3D9Client\%%F" "%ROOT%\Modules\D3D9Client" "%%F"
    if defined CVFAIL goto :restorefailed
  )
)
echo   [ok] shaders restored and verified

color 0A
echo.
echo  ==========================================================
echo    GRAPHICS CLIENT REPAIRED
echo  ==========================================================
echo.
echo   Orbiter should start normally again. ORO itself was already
echo   removed, so there is nothing else to undo.
echo.
echo   If "ORO control" is still listed in the Launchpad Modules tab,
echo   untick it.
echo.
goto :done

rem ===========================================================================
rem  restorefailed - the one path that must never be quiet
rem ===========================================================================
:restorefailed
color 0C
echo.
echo  ==========================================================
echo    STOPPED - THE GRAPHICS CLIENT COULD NOT BE RESTORED
echo  ==========================================================
echo.
echo   Could not write a verified copy of:  !CVFAIL!
echo.
echo   NOTHING HAS BEEN REMOVED. This was checked before anything was
echo   deleted, precisely so that a failure here leaves you with a working
echo   Orbiter rather than a broken one.
echo.
echo   The usual causes, in order of likelihood:
echo.
echo     1. Orbiter or the Launchpad is still open somewhere. Close both
echo        and run this again.
echo     2. Orbiter is installed under Program Files, so Windows is
echo        refusing the write. Right click this file and choose
echo        "Run as administrator".
echo     3. Antivirus is holding the file while it scans it. Wait a few
echo        seconds and run this again.
echo.
echo   If it keeps failing, you can do it by hand with Orbiter closed:
echo     copy    %SRC%\Modules\Plugin\D3D9Client.dll
echo     over    %ROOT%\Modules\Plugin\D3D9Client.dll
echo   and the six .fx / .hlsl files from
echo     %SRC%\Modules\D3D9Client\
echo   over the ones in
echo     %ROOT%\Modules\D3D9Client\
echo.
goto :fail

rem ===========================================================================
rem  checkRunning - is Orbiter or its Launchpad up?
rem
rem  Locale-safe: tasklist's "no tasks" message is translated, but it can never
rem  contain the image name, so matching on the name works in any language.
rem  Fails OPEN - if tasklist is unavailable we carry on, because copyVerify is
rem  an independent second line of defence against exactly the same problem.
rem ===========================================================================
:checkRunning
set "RUNNING="
for %%P in (Orbiter.exe Orbiter_ng.exe) do (
  tasklist /FI "IMAGENAME eq %%P" 2>nul | find /I "%%P" >nul 2>&1
  if not errorlevel 1 set "RUNNING=%%P"
)
goto :eof

rem ===========================================================================
rem  copyVerify - copy one file and prove it arrived intact
rem    %1 = source file   %2 = destination FOLDER   %3 = label for messages
rem  Sets CVFAIL to the label if the destination does not end up byte-identical
rem  to the source. Note this is correct even when the copy is REFUSED: the old
rem  file is still there, so the compare fails and we report it, rather than
rem  believing a copy that never happened.
rem ===========================================================================
:copyVerify
set "CVSRC=%~1"
set "CVDIR=%~2"
set "CVNAME=%~nx1"
if not exist "%CVDIR%\" (
  set "CVFAIL=%~3"
  goto :eof
)
copy /y "%CVSRC%" "%CVDIR%\" >nul 2>&1
if not exist "%CVDIR%\%CVNAME%" (
  set "CVFAIL=%~3"
  goto :eof
)
fc /b "%CVSRC%" "%CVDIR%\%CVNAME%" >nul 2>&1
if errorlevel 1 set "CVFAIL=%~3"
goto :eof

rem ===========================================================================
rem  cleanTree - delete only files identical to what we shipped
rem ===========================================================================
:cleanTree
set "SUB=%~1"
if not exist "%ROOT%\%SUB%" goto :eof
for /r "%ROOT%\%SUB%" %%F in (*) do (
  set "FULL=%%F"
  set "REL=!FULL:%ROOT%\=!"
  if not exist "%PAY%" (
    set /a KEPT+=1
    echo     kept: !REL!
  ) else if exist "%PAY%\!REL!" (
    fc /b "%%F" "%PAY%\!REL!" >nul 2>&1
    if errorlevel 1 (
      set /a KEPT+=1
      echo     kept ^(you changed this^): !REL!
    ) else (
      del /q "%%F" >nul 2>&1
    )
  ) else (
    set /a KEPT+=1
    echo     kept ^(yours^): !REL!
  )
)
rem remove directories that are now empty, deepest first
for /f "delims=" %%D in ('dir "%ROOT%\%SUB%" /ad /b /s 2^>nul ^| sort /r') do rd "%%D" >nul 2>&1
rd "%ROOT%\%SUB%" >nul 2>&1
goto :eof

rem ===========================================================================
rem  cleanFile - same rule, for a single file
rem ===========================================================================
:cleanFile
set "REL=%~1"
if not exist "%ROOT%\%REL%" goto :eof
if not exist "%PAY%\%REL%" (
  set /a KEPT+=1
  echo     kept: %REL%
  goto :eof
)
fc /b "%ROOT%\%REL%" "%PAY%\%REL%" >nul 2>&1
if errorlevel 1 (
  set /a KEPT+=1
  echo     kept ^(you changed this^): %REL%
) else (
  del /q "%ROOT%\%REL%" >nul 2>&1
)
goto :eof

:fail
echo.
pause
exit /b 1

:done
echo.
pause
exit /b 0
