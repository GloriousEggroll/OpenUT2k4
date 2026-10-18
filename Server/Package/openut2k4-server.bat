@echo off
rem OpenUT2k4 dedicated server launcher (Windows package; Server/Docs/DedicatedServer.md).
rem   openut2k4-server.bat [server] [DM-Rankin?game=XGame.xDeathMatch?MaxPlayers=8] [-ServerConfig=<ini>] [-ServerProfile=<dir>] ...
rem   openut2k4-server.bat -ConvertOnly -UT2004Dir=<UT2004 install> [-ContentCache=<dir>]
rem -log opens a console window with the log. Admin commands: the TCP admin console ([OpenUT2k4.AdminConsole]).
setlocal
set "BIN=%~dp0OpenUT2k4\Binaries\Win64\OpenUT2k4Server.exe"
if not exist "%BIN%" set "BIN=%~dp0OpenUT2k4\Binaries\Win64\OpenUT2k4Server-Win64-Shipping.exe"
if not exist "%BIN%" (
  echo error: OpenUT2k4Server.exe not found next to %~f0
  exit /b 2
)
"%BIN%" OpenUT2k4 -nullrhi -nosound -nosplash -unattended -log %*
exit /b %ERRORLEVEL%
