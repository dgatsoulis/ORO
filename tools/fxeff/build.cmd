@echo off
REM Build fxeff.exe (x86) against the June 2010 DXSDK the client itself builds with.
REM Output: tools\fxeff\fxeff.exe (ignored by git). Needs d3dx9_43.dll at runtime (Orbiter has it).
setlocal
call "Z:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
cl /nologo /EHsc /MD /O2 /W3 /I"C:\OrbiterDev\DXSDK\Include" fxeff.cpp /link /LIBPATH:"C:\OrbiterDev\DXSDK\Lib\x86" d3d9.lib d3dx9.lib user32.lib /OUT:fxeff.exe
if errorlevel 1 exit /b 1
del /q fxeff.obj 2>nul
echo built fxeff.exe
