@echo off
rem Makes Saved\Installer\ManchesterRide-Setup.exe from the packaged game. Run Tools\package_game.bat first.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0make_installer.ps1" %*
exit /b %ERRORLEVEL%
