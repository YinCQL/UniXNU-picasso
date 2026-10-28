@echo off
setlocal
title Picasso USB Log Capture
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\capture-picasso-usb-console.ps1" %*
if errorlevel 1 (
  echo.
  echo Capture stopped with an error. Received log data is already saved.
  pause
)
endlocal
