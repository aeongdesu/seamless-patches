# seamless-patches
> [!WARNING]
> FULL AI SLOP!!

[![Add to Reseam](.github/assets/add-to-reseam.svg)](https://reseam.app/add/?url=https%3A%2F%2Fgithub.com%2Faeong%2Fseamless-patches)

## Patches

<!-- patches:start -->

### `com.valvesoftware.steamlinkvr`

| Patch | What it does |
|---|---|
| Allow Remote VR | Removes local-network and same-subnet checks in Steam Link VR so VR mode can connect over WireGuard VPN or routed non-local networks |
| Change package name | Changes the Steam Link package name so it can be installed alongside the original app, updating VRLink scene launches and shared preferences |
| PICO Motion Tracker | Injects an OpenXR implicit API layer that streams PICO full-body tracking (XR_BD_body_tracking) and physical object tracker poses (XR_BD_motion_tracking) to the companion SteamVR OpenVR driver over UDP |

<!-- patches:end -->
