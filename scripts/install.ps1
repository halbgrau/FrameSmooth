# FrameSmooth installer (per-user, no admin rights needed).
# - copies files to %LOCALAPPDATA%\Programs\FrameSmooth
# - registers the OpenXR API layer under HKCU (one registry value)
# - creates a Start Menu shortcut (and optionally an autostart shortcut)
# Nothing belonging to Steam, SteamVR or the headset is touched.
param([switch]$Autostart)
$ErrorActionPreference = 'Stop'

# If this script runs inside a packaged (MSIX) app's container, writes to AppData and HKCU are
# silently redirected into that app's private storage and games would never see the install.
# Detect that and relaunch through Windows Explorer, which runs outside any container.
function Test-Redirected {
    $name = 'fs_probe_' + [guid]::NewGuid().ToString('N')
    $p = Join-Path $env:LOCALAPPDATA $name
    New-Item -ItemType Directory -Path $p | Out-Null
    $hit = @(Get-ChildItem "$env:LOCALAPPDATA\Packages\*\LocalCache\Local\$name" -Directory -ErrorAction SilentlyContinue).Count -gt 0
    Remove-Item $p -Force -ErrorAction SilentlyContinue
    return $hit
}
if (Test-Redirected) {
    Write-Host 'Running inside an app container; relaunching the installer through Windows Explorer...'
    Start-Process explorer.exe -ArgumentList "`"$(Join-Path $PSScriptRoot 'Install.cmd')`""
    exit 0
}

$src = $PSScriptRoot
$dst = Join-Path $env:LOCALAPPDATA 'Programs\FrameSmooth'
New-Item -ItemType Directory -Force -Path $dst | Out-Null

Get-Process FrameSmoothDashboard -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300

# Remove leftovers from earlier updates (only succeeds once no game uses them any more)
Get-ChildItem $dst -Filter '*.old-*' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue

foreach ($f in 'FrameSmoothLayer.dll', 'FrameSmoothOVR.dll', 'FrameSmoothDashboard.exe', 'openvr_api.dll', 'uninstall.ps1', 'Uninstall.cmd', 'README.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md') {
    if (-not (Test-Path (Join-Path $src $f))) { continue }
    $target = Join-Path $dst $f
    try {
        Copy-Item -Path (Join-Path $src $f) -Destination $target -Force -ErrorAction Stop
    } catch {
        # File is in use (e.g. loaded by a running game): move it aside, the new version is used from the next start
        Rename-Item -Path $target -NewName ("$f.old-" + [DateTime]::Now.Ticks) -Force
        Copy-Item -Path (Join-Path $src $f) -Destination $target -Force
        Write-Host "$f was in use; the new version takes effect the next time that game starts."
    }
}

$manifest = Join-Path $dst 'FrameSmoothLayer.json'
$json = @'
{
    "file_format_version": "1.0.0",
    "api_layer": {
        "name": "XR_APILAYER_FRAMESMOOTH_reprojection",
        "library_path": ".\\FrameSmoothLayer.dll",
        "api_version": "1.0",
        "implementation_version": "1",
        "description": "FrameSmooth: half-rate rendering with motion reprojection",
        "disable_environment": "DISABLE_FRAMESMOOTH_LAYER"
    }
}
'@
[IO.File]::WriteAllText($manifest, $json)

$k = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey('SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit')
# 1 = disabled. The FrameSmooth app switches it to 0 only while it is running (and no anti-cheat game runs),
# so the layer is never loaded into games while FrameSmooth is closed.
$k.SetValue($manifest, 1, [Microsoft.Win32.RegistryValueKind]::DWord)
$k.Close()

$shell = New-Object -ComObject WScript.Shell
$programs = [Environment]::GetFolderPath('Programs')
$desktop = [Environment]::GetFolderPath('Desktop')
$exe = Join-Path $dst 'FrameSmoothDashboard.exe'

# Shortcuts from earlier versions
foreach ($old in 'FrameSmooth.lnk', 'Exit FrameSmooth.lnk') { Remove-Item (Join-Path $programs $old) -ErrorAction SilentlyContinue }

# One "Start FrameSmooth" shortcut in the Start Menu and on the Desktop (exiting is done in the dashboard tab)
foreach ($dir in $programs, $desktop) {
    $lnk = $shell.CreateShortcut((Join-Path $dir 'Start FrameSmooth.lnk'))
    $lnk.TargetPath = $exe
    $lnk.WorkingDirectory = $dst
    $lnk.IconLocation = "$exe,0"
    $lnk.Description = 'Start FrameSmooth (it appears as a tab in the SteamVR dashboard)'
    $lnk.Save()
}

if ($Autostart) {
    $startup = [Environment]::GetFolderPath('Startup')
    $lnk = $shell.CreateShortcut((Join-Path $startup 'FrameSmooth.lnk'))
    $lnk.TargetPath = $exe
    $lnk.Arguments = '--autostart'
    $lnk.WorkingDirectory = $dst
    $lnk.Save()
}

Start-Process (Join-Path $dst 'FrameSmoothDashboard.exe') -WorkingDirectory $dst
Write-Host "FrameSmooth installed to $dst"
Write-Host "Open the SteamVR dashboard and look for the 'FrameSmooth' tab."
