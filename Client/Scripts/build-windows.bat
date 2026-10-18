@echo off
rem Build, cook and package OpenUT2k4 for Windows (Win64, Shipping, Vulkan default RHI).
rem
rem Usage: Scripts\build-windows.bat [--with-imports] [UE_ROOT]
rem   UE_ROOT defaults to the UE_ROOT environment variable, then C:\Program Files\Epic Games\UE_5.8
rem   Default: a release package with no UT2004 data. Output: Dist\Windows\
rem   --with-imports: developer package that also cooks the local editor imports (never distribute it).
rem            Output: Dist\Windows-WithImports\
rem   Check a package before distributing it: Scripts/check-release.sh (Git Bash / WSL) or look for
rem   Content\Imported paths with UnrealPak -List.
setlocal EnableExtensions

set "WITH_IMPORTS=0"
if /i "%~1"=="--with-imports" (
    set "WITH_IMPORTS=1"
    shift
)

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..") do set "PROJECT_DIR=%%~fI"
set "PROJECT=%PROJECT_DIR%\OpenUT2k4.uproject"

if not "%~1"=="" set "UE_ROOT=%~1"
if "%UE_ROOT%"=="" set "UE_ROOT=C:\Program Files\Epic Games\UE_5.8"

set "RUNUAT=%UE_ROOT%\Engine\Build\BatchFiles\RunUAT.bat"
set "BUILD_BAT=%UE_ROOT%\Engine\Build\BatchFiles\Build.bat"
set "EDITOR=%UE_ROOT%\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
set "ARCHIVE_DIR=%PROJECT_DIR%\Dist\Windows"
if "%WITH_IMPORTS%"=="1" set "ARCHIVE_DIR=%PROJECT_DIR%\Dist\Windows-WithImports"

if not exist "%RUNUAT%" (
    echo error: RunUAT.bat not found at "%RUNUAT%" ^(set UE_ROOT or pass the engine path^)
    exit /b 1
)

if exist "%PROJECT_DIR%\Content\Materials" goto cook

echo ==^> Content\Materials missing: building OpenUT2k4Editor and generating content
call "%BUILD_BAT%" OpenUT2k4Editor Win64 Development "-Project=%PROJECT%" -WaitMutex
if errorlevel 1 exit /b 1
"%EDITOR%" "%PROJECT%" -run=pythonscript "-script=%PROJECT_DIR%\Tools\GenerateContent.py" -unattended -nop4 -nosplash -nullrhi -stdout -FullStdOutLogOutput
if errorlevel 1 exit /b 1
if not exist "%PROJECT_DIR%\Content\Materials\M_ArenaSurface.uasset" (
    echo error: content generation failed
    exit /b 1
)

:cook
set "MAPS=/Game/Maps/MainMenu+/Game/Maps/Arena"
set "EXTRA="
if not "%WITH_IMPORTS%"=="1" goto run
rem Local UT2004 editor imports (git-ignored, differ per machine): add their maps and the WithImports config.
set "EXTRA=-CustomConfig=WithImports"
if exist "%PROJECT_DIR%\Content\Imported\Maps\*.umap" (
    for %%F in ("%PROJECT_DIR%\Content\Imported\Maps\*.umap") do call set "MAPS=%%MAPS%%+/Game/Imported/Maps/%%~nF"
)

:run
rem Start from an empty output folder so files of an older package never linger.
if exist "%ARCHIVE_DIR%" rmdir /s /q "%ARCHIVE_DIR%"
echo ==^> BuildCookRun (Win64, Shipping)
call "%RUNUAT%" BuildCookRun "-project=%PROJECT%" -platform=Win64 -clientconfig=Shipping "-map=%MAPS%" -build -cook -stage -pak -iostore -archive "-archivedirectory=%ARCHIVE_DIR%" %EXTRA% -nop4 -utf8output -unattended
if errorlevel 1 exit /b 1

rem Licence and notices next to the executable
copy /y "%PROJECT_DIR%\..\LICENSE" "%ARCHIVE_DIR%\" >nul
copy /y "%PROJECT_DIR%\..\NOTICE.md" "%ARCHIVE_DIR%\" >nul

rem Release archive + detached signature, only with OPENUT2K4_SIGNING_KEY (Server/Docs/Validator.md "Release signing")
if defined OPENUT2K4_SIGNING_KEY (
    echo ==^> Packing and signing %ARCHIVE_DIR%.zip
    powershell -NoProfile -Command "Compress-Archive -Path '%ARCHIVE_DIR%' -DestinationPath '%ARCHIVE_DIR%.zip' -Force"
    if errorlevel 1 exit /b 1
    python "%PROJECT_DIR%\Tools\openut2k4_signing.py" sign --key "%OPENUT2K4_SIGNING_KEY%" "%ARCHIVE_DIR%.zip" >nul
    if errorlevel 1 exit /b 1
)

echo ==^> Done: %ARCHIVE_DIR%
endlocal
