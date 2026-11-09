@echo off
rem sloperinaGZ installer: double-click this. It runs install.ps1 (options: see the top of that file).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
echo.
pause
