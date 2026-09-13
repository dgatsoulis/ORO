@echo off
rem  build.bat - compiles luachk.exe (Lua 5.1 syntax checker) into %TEMP%\luachk from the
rem  client clone's fetched Lua sources. Run from any cmd prompt; needs VS2022 and the clone.
rem  Then:  %TEMP%\luachk\luachk.exe Z:\Orbiter-2024\Script\testlights.lua
call "Z:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set "SRC=C:\OrbiterDev\orbiter\out\build\windows-x86-release\_deps\lua-src"
set "OUT=%TEMP%\luachk"
if not exist "%OUT%" mkdir "%OUT%"
del "%OUT%\files.txt" 2>nul
for %%f in ("%SRC%\*.c") do (
  if /i not "%%~nxf"=="lua.c" if /i not "%%~nxf"=="luac.c" if /i not "%%~nxf"=="print.c" echo %%f>>"%OUT%\files.txt"
)
cl /nologo /O1 /W1 /D_CRT_SECURE_NO_WARNINGS /I"%SRC%" "%~dp0luachk.c" @"%OUT%\files.txt" /Fo"%OUT%\\" /Fe:"%OUT%\luachk.exe" > "%OUT%\build.log" 2>&1
if errorlevel 1 (
  echo BUILD FAILED - see %OUT%\build.log
  exit /b 1
)
echo BUILD OK: %OUT%\luachk.exe
