@echo off
rem Double-click launcher for the Braille+ emulator.
rem
rem PowerShell refuses to run unsigned scripts under the default execution
rem policy, so invoke it with a policy scoped to this one process rather than
rem asking the user to change a machine-wide setting.
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Run-BraillePlus.ps1" %*
if errorlevel 1 (
    echo.
    echo The emulator exited with an error. The messages above say why.
    pause
)
endlocal
