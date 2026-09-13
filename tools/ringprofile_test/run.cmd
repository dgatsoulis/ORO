@echo off
REM Dump the C++ derivation for both stock ringed bodies and diff it against the Python
REM spec. Radii are the stock cfg values (Size / RingMinRadius / RingMaxRadius); the
REM compare step re-reads the cfg itself and refuses if these do not match it.
REM   tools\ringprofile_test\run.cmd [OrbiterRoot]
setlocal
set ROOT=%~1
if "%ROOT%"=="" set ROOT=Z:\Orbiter-2024
cd /d "%~dp0"
REM Test for the exe AFTER the build rather than trusting its errorlevel: vcvars32.bat's
REM own vswhere probe fails noisily on this machine and can leave errorlevel set even
REM though cl ran and linked (seen 2026-09-12 - the first run reported failure with the
REM exe sitting right there).
if not exist ringprofile_test.exe call build.cmd
if not exist ringprofile_test.exe ( echo ringprofile_test.exe did not build & exit /b 1 )

ringprofile_test.exe "%ROOT%\Textures" Saturn 58232 1.278 2.407 0 0 saturn.txt || exit /b 1
ringprofile_test.exe "%ROOT%\Textures" Uranus 25362 1.335 2.005 0 0 uranus.txt || exit /b 1

python ..\ringprofile.py Saturn --orbiter "%ROOT%" --compare saturn.txt || exit /b 1
python ..\ringprofile.py Uranus --orbiter "%ROOT%" --compare uranus.txt || exit /b 1
echo.
echo ringprofile_test: C++ and Python agree on both bodies.
