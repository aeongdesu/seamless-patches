#Requires -Version 5.1
param(
    [string]$SteamVRPath = "C:\Program Files (x86)\Steam\steamapps\common\SteamVR",
    [string]$InstallDir = "$env:LOCALAPPDATA\SeamlessPicoMT\seamless_pico_mt",
    [int]$UdpPort = 39575
)

$ErrorActionPreference = "Stop"
$VrPathReg = Join-Path $SteamVRPath "bin\win64\vrpathreg.exe"
if (Test-Path $VrPathReg) {
    & $VrPathReg removedriver $InstallDir
}
if (Test-Path $InstallDir) {
    Remove-Item -Recurse -Force $InstallDir
}
try {
    $ruleName = "Seamless PICO Motion Tracker SteamVR Driver (UDP $UdpPort)"
    Get-NetFirewallRule -DisplayName $ruleName -ErrorAction SilentlyContinue | Remove-NetFirewallRule -ErrorAction SilentlyContinue
} catch {
}
Write-Host "Uninstalled seamless_pico_mt SteamVR driver."
