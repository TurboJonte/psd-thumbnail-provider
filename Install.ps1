<#
.SYNOPSIS
    Installs the PSD thumbnail provider and preview handler.

.DESCRIPTION
    Copies the DLL to a stable location and self-registers it (regsvr32), which
    writes both the IThumbnailProvider and IPreviewHandler registrations for
    .psd and .psb. Must be run elevated.

.PARAMETER Dll
    Path to PsdThumbnailProvider.dll. Defaults to the Release build.

.PARAMETER InstallDir
    Where to copy the DLL. Defaults to %ProgramFiles%\PsdPreview.

.PARAMETER InPlace
    Register the DLL where it is instead of copying it (useful while developing).
#>
[CmdletBinding()]
param(
    [string] $Dll = "$PSScriptRoot\PsdThumbnailProvider\x64\Release\PsdThumbnailProvider.dll",
    [string] $InstallDir = "$env:ProgramFiles\PsdPreview",
    [switch] $InPlace
)

$ErrorActionPreference = 'Stop'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "This script must be run from an elevated PowerShell (Run as Administrator)."
}

if (-not (Test-Path $Dll)) {
    throw "DLL not found: $Dll`nBuild it first with .\Build.ps1"
}

$target = (Resolve-Path $Dll).Path

if (-not $InPlace) {
    if (-not (Test-Path $InstallDir)) {
        New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
    }
    $dest = Join-Path $InstallDir 'PsdThumbnailProvider.dll'

    # An already-registered copy may be loaded by these hosts.
    foreach ($proc in 'prevhost', 'dllhost') {
        Get-Process -Name $proc -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Milliseconds 300

    Copy-Item -Path $target -Destination $dest -Force
    $target = $dest
}

Write-Host "Registering $target ..."
$p = Start-Process -FilePath 'regsvr32.exe' -ArgumentList '/s', "`"$target`"" -Wait -PassThru -NoNewWindow
if ($p.ExitCode -ne 0) {
    throw "regsvr32 failed with exit code $($p.ExitCode)"
}

# Drop cached thumbnails so existing PSDs are re-rendered.
Write-Host "Clearing the thumbnail cache ..."
Get-Process -Name explorer -ErrorAction SilentlyContinue | Out-Null
$cache = "$env:LOCALAPPDATA\Microsoft\Windows\Explorer"
Get-ChildItem -Path $cache -Filter 'thumbcache_*.db' -ErrorAction SilentlyContinue |
    ForEach-Object { Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue }

Write-Host ""
Write-Host "Installed." -ForegroundColor Green
Write-Host "  Thumbnails:    .psd and .psb in Explorer"
Write-Host "  Preview pane:  enable with Alt+P in Explorer"
Write-Host ""
Write-Host "Restart Explorer to pick up the change:  Stop-Process -Name explorer -Force"
