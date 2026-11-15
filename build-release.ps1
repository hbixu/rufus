<#
.SYNOPSIS
    Builds Rufus in Release configuration using MSBuild (Visual Studio).

.PARAMETER Platform
    Target platform: x64 (default), x86 or arm64.

.PARAMETER Toolset
    MSVC platform toolset to use (e.g. v143, v145). By default, the toolset defined
    in the project is used if installed; otherwise the newest installed one is used.

.PARAMETER Rebuild
    Perform a full rebuild instead of an incremental build.

.EXAMPLE
    .\build-release.ps1
    .\build-release.ps1 -Platform arm64 -Rebuild
#>
[CmdletBinding()]
param(
    [ValidateSet('x64', 'x86', 'arm64')]
    [string]$Platform = 'x64',
    [string]$Toolset,
    [switch]$Rebuild
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$solution = Join-Path $root 'rufus.sln'

# Locate MSBuild via vswhere
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found. Is Visual Studio installed?' }
$vsPath = & $vswhere -latest -prerelease -products * -requires Microsoft.Component.MSBuild Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'No Visual Studio installation with MSBuild and C++ tools found.' }
$msbuild = Join-Path $vsPath 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path $msbuild)) { throw "MSBuild not found at $msbuild" }

# Pick a platform toolset that is actually installed
$installed = Get-ChildItem (Join-Path $vsPath 'MSBuild\Microsoft\VC') -Recurse -Directory -Filter 'v14*' -ErrorAction SilentlyContinue |
    Where-Object { $_.Parent.Name -eq 'PlatformToolsets' } | Select-Object -ExpandProperty Name -Unique | Sort-Object
$projectToolset = ([xml](Get-Content (Join-Path $root '.vs\rufus.vcxproj') -Raw)).Project.PropertyGroup.PlatformToolset |
    Where-Object { $_ } | Select-Object -First 1
if (-not $Toolset) {
    if ($installed -contains $projectToolset) { $Toolset = $projectToolset }
    elseif ($installed) {
        $Toolset = $installed | Select-Object -Last 1
        Write-Warning "Project toolset '$projectToolset' is not installed; using '$Toolset' instead."
    }
}

$target = if ($Rebuild) { 'Rebuild' } else { 'Build' }
$msbuildArgs = @(
    $solution,
    "/t:$target",
    '/p:Configuration=Release',
    "/p:Platform=$Platform",
    '/m',
    '/nologo',
    '/v:minimal'
)
if ($Toolset) { $msbuildArgs += "/p:PlatformToolset=$Toolset" }

Write-Host "MSBuild : $msbuild"
Write-Host "Config  : Release | $Platform | toolset $Toolset" -ForegroundColor Cyan
& $msbuild @msbuildArgs
if ($LASTEXITCODE -ne 0) { throw "Build failed (exit code $LASTEXITCODE)." }

$exe = Join-Path $root "$Platform\Release\rufus.exe"
if (Test-Path $exe) {
    Write-Host "Build OK: $exe" -ForegroundColor Green
} else {
    Write-Host 'Build OK.' -ForegroundColor Green
}
