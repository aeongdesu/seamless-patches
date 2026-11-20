# Steam Link VR — PICO Motion Tracker

Enables PICO Motion Tracker full-body tracking (`XR_BD_body_tracking`) and physical object tracking (`XR_BD_motion_tracking`) in the PICO build of Steam Link VR (`com.valvesoftware.steamlinkvr`), paired with a Windows SteamVR OpenVR driver ([`native/driver/`](native/driver/))

## Architecture

1. Headset Reseam Patch (`patch/`, `extensions/motiontracker/`) + OpenXR Implicit API Layer (`native/layer/`)
   - Adds `<meta-data android:name="PICO.swift.feature" android:value="1"/>` to `AndroidManifest.xml`
   - Bundles `libXrApiLayer_pico_motion_tracker.so` and `assets/openxr/1/api_layers/implicit.d/pico_motion_tracker.json`, loaded automatically by Steam Link's `libopenxr_loader.so`
   - Hooks `VRLink.onCreate` to pass patch options before OpenXR initializes, and auto-discovers the PC driver on the LAN via UDP broadcast when no host IP is configured
   - Calls `xrMarkApiClass(0x38)` on the PICO OpenXR runtime before extension enumeration so `XR_BD_body_tracking`, `XR_BD_motion_tracking`, and `XR_PICO_body_tracking` are advertised and enabled cleanly without root
   - Streams one 1,328-byte binary UDP packet (`SPMT` v1, default port `39575`, see [`native/protocol/seamless_pico_mt_protocol.h`](native/protocol/seamless_pico_mt_protocol.h)) per frame with the headset `VIEW`-in-`STAGE` pose, 24 full-body joints, and up to 6 physical object trackers with velocities and battery levels

2. PC SteamVR OpenVR Driver (`native/driver/`)
   - Exposes Vive Tracker-compatible `TrackedDeviceClass_GenericTracker` devices with opt-out hand selection, live battery reporting, and automatic `steamvr.vrsettings` tracker role assignment
   - Performs automatic 4-DOF gravity-aligned HMD-to-`STAGE` playspace calibration (`alignmentMode = "auto"`) against Steam Link's live HMD pose with instant recenter snapping (`"raw"` and `"joint"` also supported in `default.vrsettings`)
   - Automatically switches between Body Tracking and Object Tracking to match the headset setting (`trackingMode = "auto"`)

## Body Tracker Presets

| Preset | Active SteamVR Trackers (PICO Body Joints) |
|---|---|
| `3pt` | Waist (`PELVIS`), Left Foot (`LEFT_FOOT`), Right Foot (`RIGHT_FOOT`) |
| `6pt` *(default)* | `3pt` + Chest (`SPINE3`), Left Knee (`LEFT_KNEE`), Right Knee (`RIGHT_KNEE`) |
| `8pt` | `6pt` + Left Elbow (`LEFT_ELBOW`), Right Elbow (`RIGHT_ELBOW`) |
| `10pt` | `8pt` + Left Shoulder (`LEFT_SHOULDER`), Right Shoulder (`RIGHT_SHOULDER`) |
| `all` | `10pt` + Left/Right Hip (`LEFT_HIP`/`RIGHT_HIP`), Left/Right Wrist (`LEFT_WRIST`/`RIGHT_WRIST`) |

## Build & Install

### 1. Headset patch & OpenXR layer

```bash
# Rebuild arm64-v8a OpenXR API layer (requires Android NDK)
./apps/steamlink/native/layer/build.sh

# Build and sign the Reseam bundle
./gradlew bundle

# Patch Steam Link APK
reseam patch steamlink.apk \
  --bundle build/reseam/seamless-patches.reseam \
  --trust <public-key> \
  --preset none --enable "PICO Motion Tracker" \
  --output steamlink-patched.apk
```

### 2. PC SteamVR OpenVR driver

Download `seamless-pico-mt-driver-win64.zip` from Actions artifact and install driver;\:

```
powershell -ExecutionPolicy Bypass -File .\install-windows.ps1
```

## Troubleshooting

- Headset logs: `adb logcat -s PicoMT-Layer OpenXR-Loader`
- PC driver logs: check vrserver.txt in `C:\Program Files (x86)\Steam\logs\` for `[seamless_pico_mt]` lines
