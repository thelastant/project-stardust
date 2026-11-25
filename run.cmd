@echo off
setlocal
cd /d "%~dp0"

where python.exe >nul 2>nul || (
  echo Python 3.10 or newer was not found on PATH.
  echo Install Python and try again.
  pause
  exit /b 2
)

where pwsh.exe >nul 2>nul || (
  echo PowerShell 7 is required to build the PC version.
  echo Install PowerShell 7 and try again.
  pause
  exit /b 2
)

python.exe "tools\build_game.py" %*
set "RESULT=%ERRORLEVEL%"
if not "%RESULT%"=="0" echo.
if not "%RESULT%"=="0" echo The build did not complete. Review the message above.
pause
exit /b %RESULT%
