#Requires -Version 5.1
param(
    [string]$SteamVRPath = "C:\Program Files (x86)\Steam\steamapps\common\SteamVR",
    [string]$InstallDir = "$env:LOCALAPPDATA\SeamlessPicoMT\seamless_pico_mt",
    [int]$UdpPort = 39575
)

$ErrorActionPreference = "Stop"
$SourceDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$DllPath = Join-Path $SourceDir "bin\win64\driver_seamless_pico_mt.dll"

if (-not (Test-Path $DllPath)) {
    throw "Driver binary not found at $DllPath. Build it first via apps/steamlink/native/driver/build.sh or CMake."
}

$VrPathReg = Join-Path $SteamVRPath "bin\win64\vrpathreg.exe"
if (-not (Test-Path $VrPathReg)) {
    throw "SteamVR vrpathreg.exe not found at $VrPathReg. Pass -SteamVRPath if installed elsewhere."
}

Write-Host "Copying driver files to $InstallDir..."
New-Item -ItemType Directory -Force -Path (Join-Path $InstallDir "bin\win64") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $InstallDir "resources\settings") | Out-Null
Copy-Item -Force $DllPath (Join-Path $InstallDir "bin\win64\driver_seamless_pico_mt.dll")
Copy-Item -Force (Join-Path $SourceDir "driver.vrdrivermanifest") (Join-Path $InstallDir "driver.vrdrivermanifest")
Copy-Item -Force (Join-Path $SourceDir "resources\settings\default.vrsettings") (Join-Path $InstallDir "resources\settings\default.vrsettings")

Write-Host "Registering driver in SteamVR..."
& $VrPathReg adddriver $InstallDir

# Ensure SteamVR activateMultipleDrivers is enabled so this driver loads alongside Steam Link (vrlink).
$SteamConfigDir = Join-Path (Split-Path (Split-Path (Split-Path $SteamVRPath))) "config"
$VrSettingsPath = Join-Path $SteamConfigDir "steamvr.vrsettings"
if (Test-Path $VrSettingsPath) {
    $json = Get-Content -Raw -Path $VrSettingsPath | ConvertFrom-Json
    if (-not $json.steamvr) {
        $json | Add-Member -NotePropertyName "steamvr" -NotePropertyValue ([pscustomobject]@{})
    }
    if ($null -eq $json.steamvr.activateMultipleDrivers) {
        $json.steamvr | Add-Member -NotePropertyName "activateMultipleDrivers" -NotePropertyValue $true
    } else {
        $json.steamvr.activateMultipleDrivers = $true
    }
    $json | ConvertTo-Json -Depth 16 | Set-Content -Path $VrSettingsPath -Encoding UTF8
    Write-Host "Enabled steamvr.activateMultipleDrivers in $VrSettingsPath"
}

Write-Host "Adding Windows Firewall inbound UDP rule for port $UdpPort (Private/Domain)..."
try {
    $ruleName = "Seamless PICO Motion Tracker SteamVR Driver (UDP $UdpPort)"
    Get-NetFirewallRule -DisplayName $ruleName -ErrorAction SilentlyContinue | Remove-NetFirewallRule -ErrorAction SilentlyContinue
    New-NetFirewallRule -DisplayName $ruleName -Direction Inbound -Protocol UDP -LocalPort $UdpPort -Action Allow -Profile Private,Domain | Out-Null
    Write-Host "Firewall rule added."
} catch {
    Write-Warning "Could not add Windows Firewall rule automatically (run PowerShell as Administrator or allow UDP port $UdpPort manually)."
}

Write-Host "Done! Restart SteamVR to load seamless_pico_mt alongside Steam Link."
