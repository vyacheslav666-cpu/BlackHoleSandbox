# Render-performance benchmark and image-regression check.
#
# Runs a fixed set of scenes through --shot, reads the GPU timing each run
# prints, and tabulates the results.  The scenes are chosen so that the ray
# marcher diverges differently in each one -- mostly-background, mostly photon
# sphere, long paths through the disk, Kerr, and a high step budget -- because a
# change that helps one of those can easily hurt another, and a single average
# scene would hide it.
#
#   ./scripts/bench.ps1                    # time the scenes, compare to reference
#   ./scripts/bench.ps1 -UpdateReference   # adopt the current images as reference
#   ./scripts/bench.ps1 -Determinism       # also prove each scene renders bit-exactly
#   ./scripts/bench.ps1 -Only kerr         # one scene
#
# Exits non-zero when a scene's RMSE against its reference exceeds -Threshold,
# so it is usable as a gate.
#
# Deliberately not [CmdletBinding()]: under an advanced-function param block
# PowerShell evaluates the defaults in a scope where $PSScriptRoot is still
# empty, and every path below would silently resolve against the filesystem
# root instead of the repository.
param(
    [string]$Exe = "$PSScriptRoot/../out/build/ninja-msvc/bin/BlackHoleSandbox.exe",
    [string]$OutDir = "$PSScriptRoot/../bench/current",
    [string]$ReferenceDir = "$PSScriptRoot/../bench/reference",
    # RMSE is measured on 0..1 channel values, so 0.002 is about half a step of
    # an 8-bit channel spread over the whole image.
    [double]$Threshold = 0.002,
    [switch]$UpdateReference,
    [switch]$Determinism,
    [string]$Only = '',
    [int]$Samples = 96,
    [int]$Width = 1280,
    [int]$Height = 720
)

$ErrorActionPreference = 'Stop'
$invariant = [Globalization.CultureInfo]::InvariantCulture

# --time 0 and a fixed --samples are what make a run reproducible: the disk
# pattern is a function of the animation clock, and the sub-pixel jitter is a
# function of the sample index, so pinning both pins every pixel.
$commonArgs = @('--time', '0', '--samples', "$Samples", '--width', "$Width", '--height', "$Height")

$scenes = @(
    @{ name = 'wide';    args = @('--distance', '30', '--fov', '46');
       note = 'most rays reach the background' },
    @{ name = 'closeup'; args = @('--distance', '6', '--fov', '60');
       note = 'many rays orbit near the photon sphere' },
    @{ name = 'edge-on'; args = @('--distance', '14', '--pitch', '0');
       note = 'disk seen edge-on, long paths through it' },
    # The default spin is 0.85, so every scene here runs the Kerr solver, not
    # the planar Schwarzschild reduction. This one pushes the spin close to
    # extremal and the camera close in on top of that.
    @{ name = 'kerr';    args = @('--set', 'spin=0.95', '--distance', '8');
       note = 'near-extremal spin, camera deep in the strong field' },
    @{ name = 'ultra';   args = @('--quality', 'ultra', '--distance', '14');
       note = 'high step budget, two rays per pixel per frame' }
)

# --- Image comparison -------------------------------------------------------
# Done in C# rather than PowerShell: a 1280x720 image is 2.7 million channel
# values, and a PowerShell loop over that takes longer than the render it is
# checking.
if (-not ([System.Management.Automation.PSTypeName]'BhsPng').Type) {
    Add-Type -ReferencedAssemblies 'System.Drawing' -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class BhsPng
{
    private static byte[] Load(string path, out int width, out int height)
    {
        using (Bitmap bitmap = new Bitmap(path))
        {
            width = bitmap.Width;
            height = bitmap.Height;
            BitmapData data = bitmap.LockBits(new Rectangle(0, 0, width, height),
                                              ImageLockMode.ReadOnly,
                                              PixelFormat.Format24bppRgb);
            try
            {
                // Rows are copied one at a time because the locked buffer is
                // padded to a stride, which for an odd width is wider than the
                // pixels themselves.
                byte[] pixels = new byte[width * height * 3];
                for (int y = 0; y < height; y++)
                {
                    Marshal.Copy(IntPtr.Add(data.Scan0, y * data.Stride),
                                 pixels, y * width * 3, width * 3);
                }
                return pixels;
            }
            finally { bitmap.UnlockBits(data); }
        }
    }

    // Returns { rmse, maxDeviation, width, height, maxLevels }.  rmse and
    // maxDeviation are on 0..1 channel values; maxLevels is the same maximum
    // expressed in raw 8-bit steps, which is the more legible of the two when
    // deciding whether a difference matters.
    public static double[] Compare(string pathA, string pathB)
    {
        int widthA, heightA, widthB, heightB;
        byte[] a = Load(pathA, out widthA, out heightA);
        byte[] b = Load(pathB, out widthB, out heightB);
        if (widthA != widthB || heightA != heightB)
        {
            throw new InvalidOperationException(string.Format(
                "size mismatch: {0}x{1} vs {2}x{3}", widthA, heightA, widthB, heightB));
        }

        double sumSquares = 0.0;
        int maxDeviation = 0;
        for (int i = 0; i < a.Length; i++)
        {
            int difference = a[i] - b[i];
            if (difference < 0) { difference = -difference; }
            if (difference > maxDeviation) { maxDeviation = difference; }
            sumSquares += (double)difference * difference;
        }
        double rmse = Math.Sqrt(sumSquares / a.Length) / 255.0;
        return new double[] { rmse, maxDeviation / 255.0, widthA, heightA, maxDeviation };
    }
}
'@
}

# --- Helpers ----------------------------------------------------------------

# The renderer writes its numbers in the C locale.  Parsing them with the
# machine's locale would read "8.593" as 8593 on any comma-decimal system, so
# the culture is always stated explicitly here.
function ConvertTo-Number([string]$text) {
    $value = 0.0
    if ([double]::TryParse($text, [Globalization.NumberStyles]::Float, $invariant, [ref]$value)) {
        return $value
    }
    return [double]::NaN
}

function Format-Number([double]$value, [int]$decimals) {
    if ([double]::IsNaN($value)) { return '-' }
    return $value.ToString("F$decimals", $invariant)
}

# Picks the single machine-readable timing record out of the run's stdout.
function Read-TimingLine($lines) {
    $line = $lines | Where-Object { $_ -like 'BHS_TIMING *' } | Select-Object -Last 1
    if (-not $line) { return $null }
    $fields = @{}
    foreach ($token in ($line -split '\s+')) {
        if ($token -match '^([A-Za-z0-9_]+)=(.+)$') { $fields[$matches[1]] = $matches[2] }
    }
    return $fields
}

function Invoke-Shot($scene, [string]$path) {
    $arguments = @('--shot', $path) + $commonArgs + $scene.args
    $output = & $Exe @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$($scene.name): renderer exited with $LASTEXITCODE"
    }
    return Read-TimingLine $output
}

# --- Run --------------------------------------------------------------------

if (-not (Test-Path $Exe)) { throw "Build first: $Exe not found" }
$Exe = (Resolve-Path $Exe).Path
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path

$selected = $scenes | Where-Object { $Only -eq '' -or $_.name -like "*$Only*" }
if (-not $selected) { throw "No scene matches -Only '$Only'" }

Write-Host "Renderer : $Exe"
Write-Host "Output   : $OutDir"
Write-Host "Scenes   : $Width x $Height, $Samples samples, --time 0"
Write-Host ''

$results = @()
foreach ($scene in $selected) {
    $path = Join-Path $OutDir "$($scene.name).png"
    Write-Host ("  rendering {0,-8} ..." -f $scene.name) -NoNewline
    $timing = Invoke-Shot $scene $path

    $entry = [ordered]@{
        name   = $scene.name
        note   = $scene.note
        path   = $path
        median = [double]::NaN
        p95    = [double]::NaN
        min    = [double]::NaN
        max    = [double]::NaN
        frames = 0
    }
    if ($timing) {
        $entry.median = ConvertTo-Number $timing['median_ms']
        $entry.p95    = ConvertTo-Number $timing['p95_ms']
        $entry.min    = ConvertTo-Number $timing['min_ms']
        $entry.max    = ConvertTo-Number $timing['max_ms']
        $entry.frames = [int](ConvertTo-Number $timing['frames'])
        Write-Host (" {0} ms median" -f (Format-Number $entry.median 2))
    } else {
        # An old binary, or one whose timing line was swallowed.  The image is
        # still usable for the regression check, so this is reported rather than
        # treated as fatal.
        Write-Host ' no BHS_TIMING line' -ForegroundColor Yellow
    }
    $results += [pscustomobject]$entry
}

# --- Timing table -----------------------------------------------------------

Write-Host ''
Write-Host 'GPU time for the black_hole pass, per traced frame:'
Write-Host ('{0,-9} {1,9} {2,10} {3,10} {4,10} {5,8}  {6}' -f `
    'scene', 'median_ms', 'p95_ms', 'min_ms', 'max_ms', 'frames', 'scene shape')
foreach ($result in $results) {
    Write-Host ('{0,-9} {1,9} {2,10} {3,10} {4,10} {5,8}  {6}' -f `
        $result.name,
        (Format-Number $result.median 3), (Format-Number $result.p95 3),
        (Format-Number $result.min 3), (Format-Number $result.max 3),
        $result.frames, $result.note)
}

# --- Determinism ------------------------------------------------------------
# Two runs of the same scene must produce the same bytes.  If they ever stop
# doing so, every RMSE below is measuring that noise rather than the change
# under test, so this is worth checking before trusting the comparison.

$determinismFailed = $false
if ($Determinism) {
    Write-Host ''
    Write-Host 'Determinism (two runs of each scene must be byte-identical):'
    $repeatDir = Join-Path $OutDir '.repeat'
    New-Item -ItemType Directory -Force -Path $repeatDir | Out-Null
    foreach ($scene in $selected) {
        $first = Join-Path $OutDir "$($scene.name).png"
        $second = Join-Path $repeatDir "$($scene.name).png"
        Invoke-Shot $scene $second | Out-Null
        $hashA = (Get-FileHash $first -Algorithm SHA256).Hash
        $hashB = (Get-FileHash $second -Algorithm SHA256).Hash
        if ($hashA -eq $hashB) {
            Write-Host ('  {0,-9} identical  {1}' -f $scene.name, $hashA.Substring(0, 16))
        } else {
            Write-Host ('  {0,-9} DIFFERS    {1} vs {2}' -f `
                $scene.name, $hashA.Substring(0, 16), $hashB.Substring(0, 16)) -ForegroundColor Red
            $determinismFailed = $true
        }
    }
}

# --- Reference comparison ---------------------------------------------------

if ($UpdateReference) {
    New-Item -ItemType Directory -Force -Path $ReferenceDir | Out-Null
    $ReferenceDir = (Resolve-Path $ReferenceDir).Path
    foreach ($result in $results) {
        Copy-Item $result.path (Join-Path $ReferenceDir "$($result.name).png") -Force
    }
    Write-Host ''
    Write-Host "Reference images updated in $ReferenceDir" -ForegroundColor Yellow
    Write-Host 'Review them before committing: this overwrites the baseline every'
    Write-Host 'later run is measured against.'
    if ($determinismFailed) { exit 1 }
    exit 0
}

if (-not (Test-Path $ReferenceDir)) {
    Write-Host ''
    Write-Host "No reference directory at $ReferenceDir -- skipping the image comparison."
    Write-Host 'Create one with:  ./scripts/bench.ps1 -UpdateReference'
    if ($determinismFailed) { exit 1 }
    exit 0
}
$ReferenceDir = (Resolve-Path $ReferenceDir).Path

Write-Host ''
Write-Host "Image comparison against $ReferenceDir  (RMSE threshold $(Format-Number $Threshold 4))"
Write-Host ('{0,-9} {1,10} {2,12} {3,10}  {4}' -f 'scene', 'rmse', 'max_channel', 'max_steps', 'verdict')

$regressed = 0
$missing = 0
$broken = 0
foreach ($result in $results) {
    $reference = Join-Path $ReferenceDir "$($result.name).png"
    if (-not (Test-Path $reference)) {
        Write-Host ('{0,-9} {1,10} {2,12} {3,10}  {4}' -f $result.name, '-', '-', '-', 'no reference') `
            -ForegroundColor Yellow
        $missing++
        continue
    }
    try {
        $comparison = [BhsPng]::Compare($reference, $result.path)
    } catch {
        Write-Host ('{0,-9} {1,10} {2,12} {3,10}  {4}' -f `
            $result.name, '-', '-', '-', "ERROR: $($_.Exception.Message)") -ForegroundColor Red
        $broken++
        continue
    }

    $rmse = $comparison[0]
    $maxDeviation = $comparison[1]
    $maxSteps = [int]$comparison[4]
    if ($rmse -gt $Threshold) {
        Write-Host ('{0,-9} {1,10} {2,12} {3,10}  {4}' -f `
            $result.name, (Format-Number $rmse 6), (Format-Number $maxDeviation 6), $maxSteps,
            'FAIL') -ForegroundColor Red
        $regressed++
    } elseif ($maxSteps -eq 0) {
        Write-Host ('{0,-9} {1,10} {2,12} {3,10}  {4}' -f `
            $result.name, (Format-Number $rmse 6), (Format-Number $maxDeviation 6), $maxSteps,
            'identical')
    } else {
        Write-Host ('{0,-9} {1,10} {2,12} {3,10}  {4}' -f `
            $result.name, (Format-Number $rmse 6), (Format-Number $maxDeviation 6), $maxSteps,
            'ok')
    }
}

Write-Host ''
if ($missing -gt 0) {
    Write-Host "$missing scene(s) had no reference image; run -UpdateReference to add them." `
        -ForegroundColor Yellow
}
if ($determinismFailed) {
    Write-Host 'Determinism check failed: the renderer is not reproducible, so the' -ForegroundColor Red
    Write-Host 'RMSE figures above cannot separate a real change from run-to-run noise.' -ForegroundColor Red
}
if ($regressed -gt 0 -or $broken -gt 0) {
    Write-Host "$regressed scene(s) over the RMSE threshold, $broken could not be compared." `
        -ForegroundColor Red
    exit 1
}
if ($determinismFailed) { exit 1 }
Write-Host 'All compared scenes are within the RMSE threshold.' -ForegroundColor Green
exit 0
