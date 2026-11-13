# Creates the release zip from a finished build: release\FrameSmooth-<version>.zip
param(
    [string]$BuildDir = (Join-Path $PSScriptRoot 'build'),
    [string]$Version = 'v0.1.0'
)
$ErrorActionPreference = 'Stop'
$out = Join-Path $BuildDir 'out'
$name = "FrameSmooth-$Version"
$rel = Join-Path $PSScriptRoot 'release'
$stage = Join-Path $rel $name
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

foreach ($f in 'FrameSmoothDashboard.exe', 'FrameSmoothLayer.dll', 'FrameSmoothOVR.dll', 'openvr_api.dll') {
    Copy-Item (Join-Path $out $f) $stage
}
Copy-Item (Join-Path $PSScriptRoot 'scripts\*') $stage
foreach ($f in 'README.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md') { Copy-Item (Join-Path $PSScriptRoot $f) $stage }

$zip = Join-Path $rel "$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $stage -DestinationPath $zip
Write-Host "Created $zip"
Get-FileHash $zip -Algorithm SHA256 | ForEach-Object { Write-Host "SHA256: $($_.Hash)" }
