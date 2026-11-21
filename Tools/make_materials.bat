@echo off
rem Imports the CC0 photo textures (Tools/import_textures.py), then recreates Content/MR/Materials from Tools/make_materials.py.
rem Close the editor first. Uses the same M: mapping as build_editor.bat.
setlocal
set "UE=C:\Program Files\Epic Games\UE_5.5"
for %%I in ("%~dp0..") do set "PROJ=%%~fI"
if not exist M:\ManchesterRide.uproject (
  subst M: "%PROJ%"
  if errorlevel 1 (echo Drive M: is in use. Free it or edit this script. & exit /b 1)
)
"%UE%\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" M:\ManchesterRide.uproject -run=pythonscript -script="M:\Tools\import_textures.py" -unattended -nosplash -nullrhi -stdout
"%UE%\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" M:\ManchesterRide.uproject -run=pythonscript -script="M:\Tools\make_materials.py" -unattended -nosplash -nullrhi -stdout
exit /b %ERRORLEVEL%
