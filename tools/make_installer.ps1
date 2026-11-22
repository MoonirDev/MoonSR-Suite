# Builds the MoonSR installer.
#
#   tools\make_installer.ps1                       # Release build into build\release, installer into dist\
#   tools\make_installer.ps1 -SkipBuild            # reuse the existing build\release binaries
#   tools\make_installer.ps1 -IsccPath "D:\...\ISCC.exe" -OutDir dist
#
# The version is read from CMakeLists.txt (project(MoonSR VERSION x.y.z ...)). -VersionOverride is for tests only.
# ISCC (Inno Setup 6) is found via -IsccPath, PATH, the per-user Programs folder, then Program Files.
param(
    [string]$BuildDir = "build\release",
    [string]$Config = "Release",
    [string]$OutDir = "dist",
    [string]$IsccPath = "",
    [string]$VersionOverride = "",
    [switch]$SkipBuild
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

# Version from CMakeLists.txt.
$cmake = Get-Content (Join-Path $root "CMakeLists.txt") -Raw
if ($cmake -notmatch 'project\(\s*MoonSR\s+VERSION\s+(\d+\.\d+\.\d+(?:\.\d+)?)') { throw "Could not read the version from CMakeLists.txt." }
$version = $Matches[1]
if ($VersionOverride) { $version = $VersionOverride }

# Locate ISCC.
function Find-Iscc {
    param([string]$Explicit)
    if ($Explicit) {
        if (Test-Path $Explicit) { return (Resolve-Path $Explicit).Path }
        throw "ISCC not found at -IsccPath '$Explicit'."
    }
    $cmd = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $candidates = @(
        (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe"),
        (Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6\ISCC.exe"),
        (Join-Path $env:ProgramFiles "Inno Setup 6\ISCC.exe")
    )
    foreach ($c in $candidates) { if ($c -and (Test-Path $c)) { return $c } }
    throw "ISCC.exe (Inno Setup 6) not found. Install it or pass -IsccPath."
}
$iscc = Find-Iscc $IsccPath

# Build.
$bd = Join-Path $root $BuildDir
if (-not $SkipBuild) {
    # MoonSR depends on the bridge, host and forwarder targets, so building MoonSR builds all four shipped binaries.
    & (Join-Path $PSScriptRoot "build.ps1") -BuildDir $BuildDir -Config $Config -Target MoonSR
}
$bin = Join-Path $bd "bin"
$required = "MoonSR.exe", "MoonSR_NrHost.exe", "MoonSR_NrBridge.dll", "NeuralRendering_nvngx.dll"
foreach ($f in $required) { if (-not (Test-Path (Join-Path $bin $f))) { throw "Missing build output: $bin\$f" } }

# Package.
$out = if ([IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { Join-Path $root $OutDir }
New-Item -ItemType Directory -Force $out | Out-Null
$iss = Join-Path $root "installer\MoonSR.iss"
& $iscc "/DMyAppVersion=$version" "/DBuildBin=$bin" "/DOutDir=$out" $iss
if ($LASTEXITCODE -ne 0) { throw "ISCC failed ($LASTEXITCODE)." }

$setup = Join-Path $out "MoonSR-Setup-$version.exe"
if (-not (Test-Path $setup)) { throw "Installer was not produced: $setup" }
$len = (Get-Item $setup).Length
Write-Host ("Installer: {0}" -f $setup)
Write-Host ("Size: {0:N0} bytes ({1:N2} MiB)" -f $len, ($len / 1MB))
