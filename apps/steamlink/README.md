## pico motion tracker integration

### how to install driver
https://nightly.link/aeongdesu/seamless-patches/workflows/steamlink-openvr-driver/main/seamless-pico-mt-driver-win64.zip

run `install-windows.ps1` with admin

### Troubleshooting

- headset logs: `adb logcat -s PicoMT-Layer OpenXR-Loader`
- pc driver logs: check vrserver.txt in `C:\Program Files (x86)\Steam\logs\` for `[seamless_pico_mt]` lines
