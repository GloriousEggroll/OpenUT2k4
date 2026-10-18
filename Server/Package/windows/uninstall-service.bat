@echo off
rem Removes the OpenUT2k4 dedicated server Windows service (run as administrator).
rem   uninstall-service.bat [-Name default] [-Purge]
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-service.ps1" -Uninstall %*
