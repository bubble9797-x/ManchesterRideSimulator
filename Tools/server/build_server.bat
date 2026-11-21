@echo off
rem Builds the dedicated server for Debian/Linux (x64 + ARM64) and Windows, and packs the Linux one.
rem Output: Saved\Server\manchesterride-server-linux.tar.gz  (copy that to the Debian PC)
rem         Saved\Server\windows\manchesterride-server.exe   (same server on Windows, handy for testing)
setlocal
set ROOT=%~dp0..\..
set OUT=%ROOT%\Saved\Server
set DIST=%OUT%\manchesterride-server
if exist "%DIST%" rmdir /s /q "%DIST%"
mkdir "%DIST%" "%OUT%\windows" 2>nul
pushd "%ROOT%\Server"
set CGO_ENABLED=0
set GOOS=linux& set GOARCH=amd64& go build -trimpath -ldflags "-s -w" -o "%DIST%\manchesterride-server" . || goto :fail
set GOOS=linux& set GOARCH=arm64& go build -trimpath -ldflags "-s -w" -o "%DIST%\manchesterride-server-arm64" . || goto :fail
set GOOS=windows& set GOARCH=amd64& go build -trimpath -ldflags "-s -w" -o "%OUT%\windows\manchesterride-server.exe" . || goto :fail
popd
rem Text files with Linux line endings (a CRLF start.sh would not run).
powershell -NoProfile -Command "foreach ($f in 'start.sh','manchesterride.service','README.txt') { $t = [IO.File]::ReadAllText('%~dp0dist\' + $f) -replace \"`r`n\", \"`n\"; [IO.File]::WriteAllText('%DIST%\' + $f, $t) }" || goto :fail
copy /y "%~dp0dist\README.txt" "%OUT%\windows\README.txt" >nul
pushd "%~dp0pack"
set GOOS=& set GOARCH=
go run . "%DIST%" "%OUT%\manchesterride-server-linux.tar.gz" manchesterride-server || goto :fail
popd
echo Built %OUT%\manchesterride-server-linux.tar.gz
exit /b 0
:fail
popd
echo SERVER BUILD FAILED
exit /b 1
