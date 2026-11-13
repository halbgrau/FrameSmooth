# FrameSmooth uninstaller: removes everything install.ps1 created.
# -Here: clean up in the current (possibly app-container-redirected) view instead of relaunching.
param([switch]$Here)
$ErrorActionPreference = 'Continue'

if (-not $Here) {
    $name = 'fs_probe_' + [guid]::NewGuid().ToString('N')
    $p = Join-Path $env:LOCALAPPDATA $name
    New-Item -ItemType Directory -Path $p | Out-Null
    $redirected = @(Get-ChildItem "$env:LOCALAPPDATA\Packages\*\LocalCache\Local\$name" -Directory -ErrorAction SilentlyContinue).Count -gt 0
    Remove-Item $p -Force -ErrorAction SilentlyContinue
    if ($redirected) {
        Write-Host 'Running inside an app container; relaunching the uninstaller through Windows Explorer...'
        Start-Process explorer.exe -ArgumentList "`"$(Join-Path $PSScriptRoot 'Uninstall.cmd')`""
        exit 0
    }
}

Get-Process FrameSmoothDashboard -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300

# Registry: remove our layer value(s), then delete keys only if they are left empty
$path = 'SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit'
$k = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($path, $true)
if ($k) {
    foreach ($n in $k.GetValueNames()) { if ($n -like '*FrameSmoothLayer.json') { $k.DeleteValue($n, $false) } }
    $k.Close()
    foreach ($p in 'SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit', 'SOFTWARE\Khronos\OpenXR\1\ApiLayers',
                   'SOFTWARE\Khronos\OpenXR\1', 'SOFTWARE\Khronos\OpenXR', 'SOFTWARE\Khronos') {
        $kk = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($p)
        if (-not $kk) { continue }
        $empty = ($kk.SubKeyCount -eq 0 -and $kk.ValueCount -eq 0)
        $kk.Close()
        if ($empty) { [Microsoft.Win32.Registry]::CurrentUser.DeleteSubKey($p, $false) } else { break }
    }
}

Remove-Item (Join-Path ([Environment]::GetFolderPath('Programs')) 'FrameSmooth.lnk') -ErrorAction SilentlyContinue
Remove-Item (Join-Path ([Environment]::GetFolderPath('Programs')) 'Exit FrameSmooth.lnk') -ErrorAction SilentlyContinue
Remove-Item (Join-Path ([Environment]::GetFolderPath('Startup')) 'FrameSmooth.lnk') -ErrorAction SilentlyContinue
Remove-Item (Join-Path $env:LOCALAPPDATA 'FrameSmooth') -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $env:LOCALAPPDATA 'Programs\FrameSmooth') -Recurse -Force -ErrorAction SilentlyContinue

Write-Host 'FrameSmooth removed.'
