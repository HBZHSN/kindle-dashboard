param([switch]$Demo, [switch]$Once, [double]$Duration = 0, [switch]$NoPreview)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    wsl -d Ubuntu -- cmake -S . -B .build/native -DCMAKE_BUILD_TYPE=Release -DDASHBOARD_WINDOW=ON
    if ($LASTEXITCODE) { throw 'CMake configure failed' }
    wsl -d Ubuntu -- cmake --build .build/native -j 4
    if ($LASTEXITCODE) { throw 'C++ build failed' }
    $dashboardArgs = @('--simulate', '--output', 'preview')
    if ($Demo) { $dashboardArgs += @('--config', 'tests/demo.yaml', '--offline', '--snapshot', 'tests/sample.json', '--freeze-time') }
    if ($Once) { $dashboardArgs += '--once' }
    if ($Duration -gt 0) { $dashboardArgs += @('--duration', $Duration.ToString([Globalization.CultureInfo]::InvariantCulture)) }
    if (-not $NoPreview) { $dashboardArgs += '--window' }
    Write-Host 'WSLg native window: R rotates, Esc or closing the window exits. Ctrl+C also stops.'
    wsl -d Ubuntu -- .build/native/kindle-dashboard @dashboardArgs
    if ($LASTEXITCODE) { throw 'Dashboard exited with an error' }
} finally {
    Pop-Location
}
