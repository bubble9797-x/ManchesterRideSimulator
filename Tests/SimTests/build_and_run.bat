@echo off
rem Builds and runs the engine-independent riding simulation tests with MSVC.
rem Usage: Tests\SimTests\build_and_run.bat   (no Unreal Engine needed)
setlocal
set HERE=%~dp0
set SIM=%HERE%..\..\Source\ManchesterRide\Sim
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (
  for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSDIR=%%i
)
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" (
  echo Visual Studio with C++ tools not found.
  exit /b 2
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if not exist "%HERE%out" mkdir "%HERE%out"
pushd "%HERE%out"
cl /nologo /std:c++17 /O2 /EHsc /W4 /wd4100 /I "%SIM%" "%HERE%SimTests.cpp" "%SIM%\RideSim.cpp" "%SIM%\TrickTracker.cpp" "%SIM%\VehicleCatalog.cpp" "%SIM%\GeoProjection.cpp" /Fe:SimTests.exe
if errorlevel 1 ( popd & exit /b 1 )
SimTests.exe
set RC=%ERRORLEVEL%
popd
exit /b %RC%
