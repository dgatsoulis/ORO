@echo off
REM Build ringprofile_test.exe (x86, plain Win32 - no DXSDK, no Orbiter SDK: the units it
REM compiles make no oapi calls, which is the point). Output ignored by git.
setlocal
call "Z:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
cl /nologo /EHsc /MD /O2 /W3 /I"..\.." ringprofile_test.cpp ..\..\OroDDS.cpp ..\..\OroRingProfile.cpp /link /OUT:ringprofile_test.exe
if errorlevel 1 exit /b 1
del /q *.obj 2>nul
echo built ringprofile_test.exe
