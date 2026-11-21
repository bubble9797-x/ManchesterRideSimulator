@echo off
rem Builds the standalone game (Shipping, Win64) into Saved\Package\Windows. Close the editor/game first.
rem Then run Tools\installer\make_installer.bat to wrap it in the Windows installer.
rem
rem The code is built with Build.bat rather than BuildCookRun -build: with the project at the root of M:
rem the automation tool passes -remoteini="M:\" and the trailing backslash swallows the closing quote.
setlocal
set "UE=C:\Program Files\Epic Games\UE_5.5"
set "MSVC=14.44.35207"
for %%I in ("%~dp0..") do set "PROJ=%%~fI"
if not exist M:\ManchesterRide.uproject (
  subst M: "%PROJ%"
  if errorlevel 1 (echo Drive M: is in use. Free it or edit this script. & exit /b 1)
)
call "%UE%\Engine\Build\BatchFiles\Build.bat" ManchesterRideEditor Win64 Development -Project=M:\ManchesterRide.uproject -WaitMutex -NoHotReloadFromIDE -CompilerVersion=%MSVC%
if errorlevel 1 exit /b 1
call "%UE%\Engine\Build\BatchFiles\Build.bat" ManchesterRide Win64 Shipping -Project=M:\ManchesterRide.uproject -WaitMutex -NoHotReloadFromIDE -CompilerVersion=%MSVC%
if errorlevel 1 exit /b 1
call "%UE%\Engine\Build\BatchFiles\RunUAT.bat" BuildCookRun -project=M:\ManchesterRide.uproject -noP4 -platform=Win64 -clientconfig=Shipping -nocompile -nocompileeditor -skipbuild -cook -stage -pak -iostore -compressed -prereqs -nodebuginfo -archive -archivedirectory=M:\Saved\Package -utf8output -unattended
exit /b %ERRORLEVEL%
