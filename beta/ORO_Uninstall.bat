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
rem ===========================================================================

set "HERE=%~dp0"
set "HERE=%HERE:~0,-1%"
for %%I in ("%HERE%\..") do set "ROOT=%%~fI"
set "PAY=%HERE%\payload"
set "STOCK=%HERE%\stock"
set "BACKUP=%HERE%\backup"
set /a KEPT=0

echo.
echo  ==========================================================
echo    O R O   -   uninstaller
echo  ==========================================================
echo.
echo   Orbiter folder detected as:
echo     %ROOT%
echo.

rem --- 1. is ORO actually here? -------------------------------------------
rem  The DLL and the shader are the definitive markers. Settings deliberately
rem  SURVIVE an uninstall if you changed them, so their presence does not mean
rem  ORO is installed - checking for those would make a second uninstall
rem  claim ORO was still here.
set "FOUND="
if exist "%ROOT%\Modules\Plugin\ORO.dll"      set "FOUND=1"
if exist "%ROOT%\Modules\ORO\orofx.hlsl"    set "FOUND=1"

if not defined FOUND (
  color 0E
  echo   ** ORO IS NOT PRESENT - nothing to uninstall. **
  echo.
  echo   Either it was never installed in this Orbiter folder, or it has
  echo   already been removed. Nothing has been changed.
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

rem --- 2. restore the client + its six shaders -------------------------------
rem  Prefer the backup made at install time - that is YOUR original file. The
rem  shipped stock copies are the fallback if that backup is gone.
echo.
set "SRC="
if exist "%BACKUP%\Modules\Plugin\D3D9Client.dll" (
  set "SRC=%BACKUP%"
  echo   Restoring your original files from the install-time backup...
) else if exist "%STOCK%\Modules\Plugin\D3D9Client.dll" (
  set "SRC=%STOCK%"
  echo   No install backup found - restoring the pristine Orbiter 2024
  echo   originals shipped with this beta instead...
) else if exist "%STOCK%\D3D9Client.fx" (
  set "SRC=%STOCK%"
  echo   No install backup found - restoring the pristine shaders shipped
  echo   with this beta ^(client DLL not included - see below^)...
)

if not defined SRC (
  color 0E
  echo   ** Could not find anything to restore the D3D9 client from. **
  echo   ORO's own files will still be removed, but the PATCHED client
  echo   will remain in place. To get the stock one back, reinstall
  echo   Orbiter 2024 over the top - that replaces the client and shaders
  echo   and leaves your scenarios and settings alone.
  echo.
) else (
  if exist "%SRC%\Modules\Plugin\D3D9Client.dll" (
    copy /y "%SRC%\Modules\Plugin\D3D9Client.dll" "%ROOT%\Modules\Plugin\" >nul && echo   [ok] D3D9Client.dll restored
  )
  for %%F in (D3D9Client.fx Vessel.fx PBR.fx Metalness.fx Sketchpad.fx NewPlanet.hlsl) do (
    if exist "%SRC%\Modules\D3D9Client\%%F" (
      copy /y "%SRC%\Modules\D3D9Client\%%F" "%ROOT%\Modules\D3D9Client\" >nul
    ) else if exist "%SRC%\%%F" (
      copy /y "%SRC%\%%F" "%ROOT%\Modules\D3D9Client\" >nul
    )
  )
  echo   [ok] shaders restored
)

rem --- 3. remove ORO's own files, sparing anything you changed -------------
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

echo.
color 0A
echo  ==========================================================
echo    ORO REMOVED
echo  ==========================================================
echo.
if %KEPT% GTR 0 (
  color 0E
  echo   %KEPT% file^(s^) were KEPT because you added or changed them - they are
  echo   listed above. They do nothing without ORO installed, and they will
  echo   be picked up again if you reinstall it.
  echo.
)
echo   Your original D3D9 graphics client is back. Untick "ORO control"
echo   in the Launchpad Modules tab if it is still listed.
echo.
echo   This ORO_beta folder can be deleted whenever you like - but if you
echo   might reinstall, keeping it keeps your install-time backup too.
echo.
goto :done

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

:done
echo.
pause
exit /b 0
