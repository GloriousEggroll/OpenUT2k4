<#
.SYNOPSIS
  Installs (or removes) the OpenUT2k4 dedicated server as a native Windows service (Server/Docs/DedicatedServer.md
  "Windows service"). UNVERIFIED: written without a Windows machine to test on.

.DESCRIPTION
  The server binary is the service itself (-Service=<name>: ServiceMain and the stop / shutdown controls are handled in
  Source/OpenUT2k4/Server/ArenaServerService.cpp). Stop and shutdown end the engine normally: players are told, the log
  is flushed. A failure exit or a crash restarts the service (recovery actions: restart after 10 s, 10 s, 60 s).
  Run from an elevated PowerShell in the server package:

    powershell -ExecutionPolicy Bypass -File Server\windows\install-service.ps1 [-Name default] [-Config <ini>]
        [-ServerProfile <dir>] [-StateDir <dir>] [-ContentCache <dir>] [-UT2004Dir <install>]
        [-Account "NT AUTHORITY\LocalService" | -Account <user> -Password <pw>] [-NoStart]
    powershell -ExecutionPolicy Bypass -File Server\windows\install-service.ps1 -Uninstall [-Name default] [-Purge]

  The default account is NT AUTHORITY\LocalService (unprivileged). It needs read access to the content cache and the
  UT2004 install: point -ContentCache / -UT2004Dir at folders it can read (the script grants it read access) and
  convert once with: openut2k4-server.bat -ConvertOnly -UT2004Dir=<install> -ContentCache=<dir>
#>
param(
    [string]$Name = "default",
    [string]$Config = "",
    [string]$ServerProfile = "",
    [string]$StateDir = "",
    [string]$ContentCache = "",
    [string]$UT2004Dir = "",
    [string]$Account = "NT AUTHORITY\LocalService",
    [string]$Password = "",
    [switch]$NoStart,
    [switch]$Uninstall,
    [switch]$Purge
)
$ErrorActionPreference = "Stop"

if ($Name -notmatch '^[A-Za-z0-9_.-]+$') { throw "-Name may only contain letters, digits, '.', '_' and '-'" }
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw "Run this from an elevated (administrator) PowerShell." }

$ServiceName = "OpenUT2k4Server-$Name"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$DataRoot = Join-Path $env:ProgramData "OpenUT2k4Server"
if (-not $StateDir) { $StateDir = Join-Path $DataRoot $Name }

if ($Uninstall) {
    $svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if ($svc) {
        if ($svc.Status -ne "Stopped") { Stop-Service -Name $ServiceName -Force; $svc.WaitForStatus("Stopped", "00:00:40") }
        & sc.exe delete $ServiceName | Out-Null
        Write-Host "==> removed $ServiceName"
    } else {
        Write-Host "$ServiceName is not installed"
    }
    if ($Purge) {
        if (Test-Path $StateDir) { Remove-Item -Recurse -Force -LiteralPath $StateDir }
        $cfg = Join-Path $DataRoot "$Name.ini"
        if (Test-Path $cfg) { Remove-Item -Force -LiteralPath $cfg }
        Write-Host "==> deleted $StateDir"
    }
    exit 0
}

# the real binary (not the bootstrap exe at the package root): Development or Shipping
$Exe = @("OpenUT2k4\Binaries\Win64\OpenUT2k4Server.exe", "OpenUT2k4\Binaries\Win64\OpenUT2k4Server-Win64-Shipping.exe") |
    ForEach-Object { Join-Path $Root $_ } | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $Exe) { throw "OpenUT2k4Server.exe not found under $Root\OpenUT2k4\Binaries\Win64" }

New-Item -ItemType Directory -Force -Path (Join-Path $StateDir "Logs") | Out-Null
if (-not $Config) {
    $Config = Join-Path $DataRoot "$Name.ini"
    if (-not (Test-Path $Config)) {
        Copy-Item (Join-Path $Root "Server\OpenUT2k4Server.ini") $Config
        Write-Host "==> new server config $Config (edit it, then: Restart-Service $ServiceName)"
    }
}
$Config = (Resolve-Path $Config).Path

# the service account: write access to its state, read access to the config, content and install
function Grant($Path, $Rights) {
    if ($Path -and (Test-Path $Path)) {
        $acl = Get-Acl $Path
        $inherit = if ((Get-Item $Path).PSIsContainer) { "ContainerInherit,ObjectInherit" } else { "None" }
        $rule = New-Object Security.AccessControl.FileSystemAccessRule($Account, $Rights, $inherit, "None", "Allow")
        $acl.AddAccessRule($rule)
        Set-Acl $Path $acl
    }
}
Grant $StateDir "Modify"
Grant $Config "Read"
Grant $ContentCache "ReadAndExecute"
Grant $UT2004Dir "ReadAndExecute"
Grant $Root "ReadAndExecute"

$SvcArgs = @("OpenUT2k4", "-Service=$ServiceName", "-nullrhi", "-nosound", "-nosplash", "-unattended",
    "`"-ServerConfig=$Config`"", "`"-ServerStateDir=$StateDir`"", "`"-userdir=$StateDir`"",
    "`"-abslog=$(Join-Path $StateDir 'Logs\OpenUT2k4Server.log')`"")
if ($ServerProfile) { $SvcArgs += "`"-ServerProfile=$((Resolve-Path $ServerProfile).Path)`"" }
if ($ContentCache) { $SvcArgs += "`"-ContentCache=$ContentCache`"" }
if ($UT2004Dir) { $SvcArgs += "`"-UT2004Dir=$UT2004Dir`"" }
$BinPath = "`"$Exe`" " + ($SvcArgs -join " ")

if (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) {
    Stop-Service -Name $ServiceName -Force -ErrorAction SilentlyContinue
    & sc.exe delete $ServiceName | Out-Null
    Start-Sleep -Seconds 2
}
if ($Password) {
    $cred = New-Object Management.Automation.PSCredential($Account, (ConvertTo-SecureString $Password -AsPlainText -Force))
    New-Service -Name $ServiceName -BinaryPathName $BinPath -DisplayName "OpenUT2k4 dedicated server ($Name)" -StartupType Automatic -Credential $cred | Out-Null
} else {
    New-Service -Name $ServiceName -BinaryPathName $BinPath -DisplayName "OpenUT2k4 dedicated server ($Name)" -StartupType Automatic | Out-Null
    & sc.exe config $ServiceName obj= "$Account" password= "" | Out-Null
}
& sc.exe description $ServiceName "OpenUT2k4 dedicated server for UT2004 and OpenUT2k4 players. Config: $Config" | Out-Null
# restart on failure (non-zero exit or crash), not after a normal stop; give it time to shut down cleanly
& sc.exe failure $ServiceName reset= 86400 actions= restart/10000/restart/10000/restart/60000 | Out-Null
& sc.exe failureflag $ServiceName 1 | Out-Null
& sc.exe config $ServiceName start= delayed-auto | Out-Null
Write-Host "==> installed $ServiceName ($Account): $BinPath"
if (-not $NoStart) {
    Start-Service -Name $ServiceName
    Write-Host "==> started. Status: Get-Service $ServiceName; stop: Stop-Service $ServiceName; log: $(Join-Path $StateDir 'Logs\OpenUT2k4Server.log')"
}
