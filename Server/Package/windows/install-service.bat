@echo off
rem Installs the OpenUT2k4 dedicated server as a Windows service (run as administrator). Options: see install-service.ps1
rem   install-service.bat [-Name default] [-Config <ini>] [-ContentCache <dir>] [-UT2004Dir <install>] ...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-service.ps1" %*
