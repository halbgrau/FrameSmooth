@echo off
copy /y "%~dp0uninstall.ps1" "%TEMP%\framesmooth_uninstall.ps1" >nul
powershell -NoProfile -ExecutionPolicy Bypass -File "%TEMP%\framesmooth_uninstall.ps1"
del "%TEMP%\framesmooth_uninstall.ps1" >nul 2>&1
pause
