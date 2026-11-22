# MoonSR build script. Imports the Visual Studio x64 toolchain environment, then configures and builds with CMake + Ninja.
#
#   tools\build.ps1                               # RelWithDebInfo, all targets, build\main
#   tools\build.ps1 -Config Debug
#   tools\build.ps1 -BuildDir build\myagent -Target moonsr_gpu
#
# Parallel agents must each use their own -BuildDir to avoid Ninja lock conflicts.
param(
    [string]$Config = "RelWithDebInfo",
    [string]$BuildDir = "build\main",
    [string]$Target = "",
    [string[]]$ConfigureArgs = @(),
    [switch]$Clean
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found; install Visual Studio with the C++ desktop workload." }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "No Visual Studio installation with the MSVC x64 toolset was found." }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"

# Import the vcvars environment into this PowerShell process.
$envDump = cmd /c "`"$vcvars`" >nul 2>&1 && set"
foreach ($line in $envDump) {
    $i = $line.IndexOf('=')
    if ($i -gt 0) { Set-Item -Path ("env:" + $line.Substring(0, $i)) -Value $line.Substring($i + 1) }
}

$bd = Join-Path $root $BuildDir
if ($Clean -and (Test-Path $bd)) { Remove-Item -Recurse -Force $bd }

cmake -S $root -B $bd -G Ninja "-DCMAKE_BUILD_TYPE=$Config" @ConfigureArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)." }

if ($Target) { cmake --build $bd --target $Target } else { cmake --build $bd }
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)." }
Write-Host "Build OK: $bd"
