# Renders a set of stills for visual review.  Each entry is a label plus the
# argument list handed to BlackHoleSandbox.exe --shot.
param(
    [string]$OutDir = "$PSScriptRoot/../out/shots",
    [int]$Samples = 32,
    [int]$Width = 1000,
    [int]$Height = 600,
    [string]$Only = ""
)
$ErrorActionPreference = 'Stop'
$exe = Join-Path $PSScriptRoot "../out/build/ninja-msvc/bin/BlackHoleSandbox.exe"
if (-not (Test-Path $exe)) { throw "Build first: $exe not found" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$shots = @(
    @{ name = 'final_cinematic'; args = @('--distance','26','--pitch','-12','--fov','42') },
    @{ name = 'final_edge_on';   args = @('--distance','30','--pitch','1.8','--fov','40') },
    @{ name = 'final_high';      args = @('--distance','34','--pitch','-38','--fov','42') },
    @{ name = 'final_close';     args = @('--distance','8.5','--pitch','-7','--fov','66') },
    @{ name = 'dbg1_lensing';    args = @('--debug','1','--distance','26','--pitch','-12') },
    @{ name = 'dbg2_steps';      args = @('--debug','2','--distance','26','--pitch','-12') },
    @{ name = 'dbg3_classify';   args = @('--debug','3','--distance','26','--pitch','-12') },
    @{ name = 'dbg4_depth';      args = @('--debug','4','--distance','26','--pitch','-12') },
    @{ name = 'dbg5_shift';      args = @('--debug','5','--distance','26','--pitch','-12') },
    @{ name = 'dbg6_doppler';    args = @('--debug','6','--distance','26','--pitch','-12') },
    @{ name = 'dbg7_radii';      args = @('--debug','7','--distance','26','--pitch','-12') },
    @{ name = 'dbg9_disk';       args = @('--debug','9','--distance','26','--pitch','-12') }
)

foreach ($shot in $shots) {
    if ($Only -ne "" -and $shot.name -notlike "*$Only*") { continue }
    $path = Join-Path $OutDir "$($shot.name).png"
    $all = @('--shot', $path, '--width', "$Width", '--height', "$Height", '--samples', "$Samples") + $shot.args
    & $exe @all | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Host "FAILED: $($shot.name)" -ForegroundColor Red }
    else { Write-Host "ok  $($shot.name)" }
}
Write-Host "`nShots in $OutDir"
