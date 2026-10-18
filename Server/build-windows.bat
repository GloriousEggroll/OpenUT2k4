@echo off
rem Build, cook and package the OpenUT2k4 dedicated server for Windows (Server/Docs/DedicatedServer.md "Building").
rem UNVERIFIED: written on Linux without a Windows machine to run it on.
rem
rem Usage: Server\build-windows.bat [--shipping] [--server-target] [UE_ROOT]
rem   UE_ROOT defaults to the UE_ROOT environment variable, then C:\Program Files\Epic Games\UE_5.8
rem   Output: Client\Dist\Server-Windows\ (no UT2004 data)
rem   Default: the OpenUT2k4Server target as a Game target (an installed engine), Development configuration
rem            (a Game target has no log in Shipping on an installed engine).
rem   --shipping       Shipping configuration
rem   --server-target  a source-built engine: a real Server target (UE_SERVER), cooked for WindowsServer
rem   Check the package before distributing it: Client/Scripts/check-release.sh Client/Dist/Server-Windows (Git Bash / WSL).
setlocal EnableExtensions

set "CONFIG=Development"
set "SERVER_TARGET=0"
:args
if /i "%~1"=="--shipping" (
    set "CONFIG=Shipping"
    shift
    goto args
)
if /i "%~1"=="--server-target" (
    set "SERVER_TARGET=1"
    shift
    goto args
)

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..\Client") do set "PROJECT_DIR=%%~fI"
set "PROJECT=%PROJECT_DIR%\OpenUT2k4.uproject"
if not "%~1"=="" set "UE_ROOT=%~1"
if "%UE_ROOT%"=="" set "UE_ROOT=C:\Program Files\Epic Games\UE_5.8"
set "RUNUAT=%UE_ROOT%\Engine\Build\BatchFiles\RunUAT.bat"
set "ARCHIVE_DIR=%PROJECT_DIR%\Dist\Server-Windows"
if "%CONFIG%"=="Shipping" set "ARCHIVE_DIR=%PROJECT_DIR%\Dist\Server-Windows-Shipping"

if not exist "%RUNUAT%" (
    echo error: RunUAT.bat not found at "%RUNUAT%" ^(set UE_ROOT or pass the engine path^)
    exit /b 1
)
if not exist "%PROJECT_DIR%\Content\Materials" (
    echo error: Content\Materials missing: run Client\Scripts\build-windows.bat once ^(it generates the content^)
    exit /b 1
)

if "%SERVER_TARGET%"=="1" (
    set "OPENUT2K4_SERVER_TARGET_TYPE=Server"
    set "TARGET_ARGS=-server -noclient -serverplatform=Win64 -serverconfig=%CONFIG% -target=OpenUT2k4Server"
) else (
    set "OPENUT2K4_SERVER_TARGET_TYPE=Game"
    set "TARGET_ARGS=-platform=Win64 -clientconfig=%CONFIG% -target=OpenUT2k4Server"
)

if exist "%ARCHIVE_DIR%" rmdir /s /q "%ARCHIVE_DIR%"
echo ==^> BuildCookRun OpenUT2k4Server (Win64, %CONFIG%)
call "%RUNUAT%" BuildCookRun "-project=%PROJECT%" %TARGET_ARGS% "-map=/Game/Maps/MainMenu+/Game/Maps/Arena" -build -cook -stage -pak -iostore -nodebuginfo -archive "-archivedirectory=%ARCHIVE_DIR%" "-stagingdirectory=%PROJECT_DIR%\Saved\StagedBuilds\Server" -nop4 -utf8output -unattended
if errorlevel 1 exit /b 1

rem UAT archives into a platform subfolder: flatten it
for %%S in (WindowsServer Windows) do (
    if exist "%ARCHIVE_DIR%\%%S\OpenUT2k4" (
        robocopy "%ARCHIVE_DIR%\%%S" "%ARCHIVE_DIR%" /E /MOVE /NFL /NDL /NJH /NJS >nul
    )
)
del /s /q "%ARCHIVE_DIR%\*.pdb" >nul 2>&1
rem headless: no Vulkan loader / layers needed by a -nullrhi server
if exist "%ARCHIVE_DIR%\Engine\Binaries\ThirdParty\Vulkan" rmdir /s /q "%ARCHIVE_DIR%\Engine\Binaries\ThirdParty\Vulkan"

mkdir "%ARCHIVE_DIR%\Server\windows" 2>nul
copy /y "%SCRIPT_DIR%Package\openut2k4-server.bat" "%ARCHIVE_DIR%\" >nul
copy /y "%SCRIPT_DIR%Package\ucc.bat" "%ARCHIVE_DIR%\" >nul
copy /y "%SCRIPT_DIR%Package\OpenUT2k4Server.ini" "%ARCHIVE_DIR%\Server\" >nul
copy /y "%SCRIPT_DIR%Package\windows\*" "%ARCHIVE_DIR%\Server\windows\" >nul
copy /y "%SCRIPT_DIR%..\LICENSE" "%ARCHIVE_DIR%\" >nul
copy /y "%SCRIPT_DIR%..\NOTICE.md" "%ARCHIVE_DIR%\" >nul

echo ==^> Done: %ARCHIVE_DIR%
echo     Service: Server\windows\install-service.bat ^(administrator^)
endlocal
