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

.PARAMETER NoRestart
    Skip restarting Explorer. The thumbnail cache cannot be cleared while
    Explorer is running, so already-seen PSDs will keep their old icons until
    you restart it yourself.

.NOTES
    No Windows reboot is required - only Explorer needs restarting.
#>
[CmdletBinding()]
param(
    [string] $Dll = "$PSScriptRoot\PsdThumbnailProvider\x64\Release\PsdThumbnailProvider.dll",
    [string] $InstallDir = "$env:ProgramFiles\PsdPreview",
    [switch] $InPlace,
    [switch] $NoRestart
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

# Explorer keeps thumbcache_*.db open, and the files cannot be deleted while it
# runs - so stop it first, clear the cache, then bring it back. Without this,
# PSDs that already have a cached generic icon keep showing it.
$cache = "$env:LOCALAPPDATA\Microsoft\Windows\Explorer"

if ($NoRestart) {
    Write-Warning "Skipping the Explorer restart, so the thumbnail cache was not cleared."
    Write-Warning "PSDs you have already browsed will keep their old icons."
} else {
    Write-Host "Restarting Explorer and clearing the thumbnail cache ..."

    Get-Process -Name explorer -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 800

    $removed = 0
    $failed = 0
    Get-ChildItem -Path $cache -Filter 'thumbcache_*.db' -ErrorAction SilentlyContinue | ForEach-Object {
        try {
            Remove-Item $_.FullName -Force -ErrorAction Stop
            $removed++
        } catch {
            $failed++
        }
    }

    # Explorer normally respawns on its own; start it if it did not.
    if (-not (Get-Process -Name explorer -ErrorAction SilentlyContinue)) {
        Start-Process explorer.exe
    }

    Write-Host "  cache files removed: $removed"
    if ($failed -gt 0) {
        Write-Warning "  $failed cache file(s) were still locked and could not be removed."
        Write-Warning "  Some PSDs may keep their old icons until the next sign-in."
    }
}

Write-Host ""
Write-Host "Installed." -ForegroundColor Green
Write-Host "  Thumbnails:    .psd and .psb in Explorer"
Write-Host "  Preview pane:  enable with Alt+P in Explorer"
Write-Host ""
Write-Host "No Windows restart is needed."
