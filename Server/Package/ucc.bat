@echo off
rem UT2004 "ucc server" compatibility: ucc server DM-Rankin?game=XGame.xDeathMatch ini=MyServer.ini log=server.log -port=7777
if /I "%~1"=="server" (
  shift
  call "%~dp0openut2k4-server.bat" %1 %2 %3 %4 %5 %6 %7 %8 %9
  exit /b %ERRORLEVEL%
)
echo usage: ucc server ^<Map^>?game=^<GameClass^>?^<options^> [ini=^<file^>] [log=^<file^>] [-port=^<n^>]
exit /b 2
