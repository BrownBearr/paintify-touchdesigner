$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe = Join-Path $repoRoot 'build-package\gpu-sbr.exe'
$output = Join-Path $PSScriptRoot 'painted.png'
$rawLog = Join-Path $PSScriptRoot 'raw-runs.txt'
$csv = Join-Path $PSScriptRoot 'results.csv'

if (-not (Test-Path -LiteralPath $exe)) {
    throw "Renderer not found: $exe"
}
Remove-Item -LiteralPath $rawLog, $csv -Force -ErrorAction SilentlyContinue

$cases = @(
    @{ Name = '720p-p4-r0'; Width = 1280; Height = 720; Passes = 4; Relax = 0 },
    @{ Name = '720p-p8-r0'; Width = 1280; Height = 720; Passes = 8; Relax = 0 },
    @{ Name = '720p-p4-r4'; Width = 1280; Height = 720; Passes = 4; Relax = 4 },
    @{ Name = '1080p-p4-r0'; Width = 1920; Height = 1080; Passes = 4; Relax = 0 },
    @{ Name = '1080p-p8-r0'; Width = 1920; Height = 1080; Passes = 8; Relax = 0 }
)

foreach ($case in $cases) {
    $input = Join-Path $PSScriptRoot "source_$($case.Width)x$($case.Height).png"
    if (-not (Test-Path -LiteralPath $input)) {
        throw "Benchmark input not found: $input"
    }
    $warmupArgs = @('--headless', '--in', $input, '--out', $output,
                    '--passes', [string]$case.Passes, '--relax', [string]$case.Relax)
    $null = & $exe @warmupArgs 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Renderer warm-up failed for $($case.Name)"
    }
}

$records = foreach ($trial in 1..4) {
    foreach ($case in $cases) {
        $input = Join-Path $PSScriptRoot "source_$($case.Width)x$($case.Height).png"
        $rendererArgs = @('--headless', '--in', $input, '--out', $output,
                          '--passes', [string]$case.Passes, '--relax', [string]$case.Relax)
        $timer = [System.Diagnostics.Stopwatch]::StartNew()
        $lines = & $exe @rendererArgs 2>&1
        $exitCode = $LASTEXITCODE
        $timer.Stop()
        $text = ($lines | ForEach-Object { [string]$_ }) -join "`n"
        Add-Content -LiteralPath $rawLog -Value "`n=== $($case.Name) trial $trial ===`n$text"
        if ($exitCode -ne 0) {
            throw "Renderer exited $exitCode for $($case.Name), trial $trial. See $rawLog"
        }
        $gpu = [regex]::Match($text, 'reference\s+([\d.]+)\s+\|.*?relax\s+([\d.]+)\s+\|.*?total\s+([\d.]+) ms')
        if (-not $gpu.Success) {
            throw "Could not parse GPU timings for $($case.Name), trial $trial. See $rawLog"
        }
        [pscustomobject]@{
            Case = $case.Name
            Trial = $trial
            Width = $case.Width
            Height = $case.Height
            Passes = $case.Passes
            Relax = $case.Relax
            GpuTotalMs = [double]$gpu.Groups[3].Value
            GpuRelaxMs = [double]$gpu.Groups[2].Value
            ProcessWallMs = [math]::Round($timer.Elapsed.TotalMilliseconds, 1)
        }
    }
}

$records | Export-Csv -LiteralPath $csv -NoTypeInformation
$records | Format-Table -AutoSize
"Raw output: $rawLog"
"CSV: $csv"
