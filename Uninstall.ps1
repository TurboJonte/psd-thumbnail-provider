<#
.SYNOPSIS
    Removes the PSD thumbnail provider and preview handler.
#>
[CmdletBinding()]
param(
    [string] $Dll = "$env:ProgramFiles\PsdPreview\PsdThumbnailProvider.dll"
)

$ErrorActionPreference = 'Stop'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "This script must be run from an elevated PowerShell (Run as Administrator)."
}

if (Test-Path $Dll) {
    Write-Host "Unregistering $Dll ..."
    Start-Process -FilePath 'regsvr32.exe' -ArgumentList '/u', '/s', "`"$Dll`"" -Wait -NoNewWindow
} else {
    Write-Warning "DLL not found at $Dll - removing registry entries directly."
    $thumb   = '{5BB45D32-AF01-414F-B60A-5E999B986681}'
    $preview = '{C4A4EC15-9C49-40A5-BD02-269CB227A29D}'
    foreach ($ext in '.psd', '.psb') {
        foreach ($slot in '{e357fccd-a995-4576-b01f-234630154e96}', '{8895b1c6-b41f-4c1c-a562-0d564250836f}') {
            Remove-Item -Path "Registry::HKEY_CLASSES_ROOT\$ext\shellex\$slot" -Recurse -Force -ErrorAction SilentlyContinue
            Remove-Item -Path "Registry::HKEY_CLASSES_ROOT\SystemFileAssociations\$ext\shellex\$slot" -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
    foreach ($clsid in $thumb, $preview) {
        Remove-Item -Path "Registry::HKEY_CLASSES_ROOT\CLSID\$clsid" -Recurse -Force -ErrorAction SilentlyContinue
    }
    Remove-ItemProperty -Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\PreviewHandlers' -Name $preview -Force -ErrorAction SilentlyContinue
}

foreach ($proc in 'prevhost', 'dllhost') {
    Get-Process -Name $proc -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}

Write-Host "Uninstalled." -ForegroundColor Green
Write-Host "Restart Explorer to clear it from memory:  Stop-Process -Name explorer -Force"
