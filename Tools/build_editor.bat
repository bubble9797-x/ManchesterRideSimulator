@echo off
rem Builds ManchesterRideEditor (Win64 Development) with UE 5.5 and the VS 2022 toolchain.
rem
rem MSVC fails on intermediate paths over 260 characters, so the project is mapped to drive M:
rem (subst: no admin needed, gone after sign-out) and built from there. UE 5.5 does not
rem support the VS 2026 (14.5x) compiler, so the VS 2022 Build Tools version is pinned.
setlocal
set "UE=C:\Program Files\Epic Games\UE_5.5"
set "MSVC=14.44.35207"
for %%I in ("%~dp0..") do set "PROJ=%%~fI"

if not exist M:\ManchesterRide.uproject (
  subst M: "%PROJ%"
  if errorlevel 1 (echo Drive M: is in use. Free it or edit this script. & exit /b 1)
)

call "%UE%\Engine\Build\BatchFiles\Build.bat" ManchesterRideEditor Win64 Development -Project=M:\ManchesterRide.uproject -WaitMutex -NoHotReloadFromIDE -CompilerVersion=%MSVC%
exit /b %ERRORLEVEL%
