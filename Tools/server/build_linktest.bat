@echo off
rem Builds the protocol test bot (the game's link code outside Unreal).
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 -vcvars_ver=14.44 >nul || exit /b 1
if not exist "%~dp0..\..\Saved\Server\test" mkdir "%~dp0..\..\Saved\Server\test"
cl /nologo /std:c++20 /O2 /EHsc /W3 /I"%~dp0..\..\Source\ManchesterRide" "%~dp0linktest.cpp" "%~dp0..\..\Source\ManchesterRide\Net\MRServerLink.cpp" /DMR_LINK_STANDALONE /Fo"%~dp0..\..\Saved\Server\test\\" /Fe"%~dp0..\..\Saved\Server\test\linktest.exe" ws2_32.lib bcrypt.lib
