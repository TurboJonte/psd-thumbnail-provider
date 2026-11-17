<#
.SYNOPSIS
    Builds PsdThumbnailProvider.dll (x64) and the test harnesses.
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string] $Configuration = 'Release',
    [switch] $Tests
)

$ErrorActionPreference = 'Stop'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found - install Visual Studio or the Build Tools." }

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "No Visual Studio C++ toolset found." }

$msbuild = Join-Path $vsPath 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path $msbuild)) { throw "MSBuild not found at $msbuild" }

$proj = "$PSScriptRoot\PsdThumbnailProvider\PsdThumbnailProvider.vcxproj"

Write-Host "Building $Configuration x64 ..." -ForegroundColor Cyan
& $msbuild $proj -nologo -v:m `
    -p:Configuration=$Configuration `
    -p:Platform=x64 `
    -p:PlatformToolset=v143 `
    -p:WindowsTargetPlatformVersion=10.0.26100.0
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

$dll = "$PSScriptRoot\PsdThumbnailProvider\x64\$Configuration\PsdThumbnailProvider.dll"
Write-Host "Built: $dll" -ForegroundColor Green

if ($Tests) {
    Push-Location "$PSScriptRoot\Test"
    try {
        foreach ($bat in 'build_test.bat', 'build_compare.bat', 'build_preview.bat', 'build_dlltest.bat') {
            Write-Host "Building $bat ..." -ForegroundColor Cyan
            & cmd.exe /c ".\$bat"
        }
    } finally { Pop-Location }
}
