# Black Hole Sandbox - one-command build from any shell.
#
# Locates Visual Studio, imports its x64 developer environment (so cl.exe,
# cmake.exe and ninja.exe are on PATH), then configures and builds.
#
#   ./build.ps1                 # configure + build Release
#   ./build.ps1 -Run            # ... and launch the executable
#   ./build.ps1 -Clean          # wipe the build directory first
[CmdletBinding()]
param(
    [switch]$Run,
    [switch]$Clean,
    [string]$Preset = 'ninja-msvc'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Definition
$buildDir = Join-Path $root "out/build/$Preset"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found; is Visual Studio installed?" }
$vsPath = & $vswhere -latest -products * -property installationPath
if (-not $vsPath) { throw "No Visual Studio installation found." }

# vcvars64.bat only mutates a cmd.exe environment, so run it there and copy the
# resulting variables back into this PowerShell session.
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Force -Path "env:$($matches[1])" -Value $matches[2] }
}

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Removing $buildDir" -ForegroundColor Yellow
    Remove-Item -Recurse -Force $buildDir
}

Push-Location $root
try {
    cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed." }
    cmake --build $buildDir --parallel
    if ($LASTEXITCODE -ne 0) { throw "Build failed." }
    $exe = Join-Path $buildDir 'bin/BlackHoleSandbox.exe'
    Write-Host "`nBuilt: $exe" -ForegroundColor Green
    if ($Run) { & $exe }
} finally { Pop-Location }
