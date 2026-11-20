#pragma once

#include <cstdint>

#include "openxr/openxr.h"

namespace seamless::pico_xr {

constexpr const char* kExtBdMotionTracking = "XR_BD_motion_tracking";
constexpr const char* kExtPicoBodyTracking = "XR_PICO_body_tracking";

// PICO runtime API class mask: pico-only (0x08) | private (0x10) | third-party public (0x20).
constexpr uint64_t kPicoApiClassVendorExtensions = 0x38u;

struct XrMotionTrackerSNBD {
    char value[24];
};
static_assert(sizeof(XrMotionTrackerSNBD) == 24, "XrMotionTrackerSNBD ABI mismatch");

struct XrMotionTrackerConnectStateBD {
    int32_t trackerSum;
    XrMotionTrackerSNBD trackersSN[6];
};
static_assert(sizeof(XrMotionTrackerConnectStateBD) == 148, "XrMotionTrackerConnectStateBD ABI mismatch");

struct XrMotionTrackerLocationDataBD {
    XrPosef pose;
    XrVector3f angularVelocity;
    XrVector3f linearVelocity;
    XrVector3f angularAcceleration;
    XrVector3f linearAcceleration;
    uint8_t confidence;
    uint8_t pad[3];
};
static_assert(sizeof(XrMotionTrackerLocationDataBD) == 80, "XrMotionTrackerLocationDataBD ABI mismatch");

struct XrMotionTrackerLocationsBD {
    XrMotionTrackerSNBD trackerSN;
    XrMotionTrackerLocationDataBD localLocation;
    XrMotionTrackerLocationDataBD globalLocation;
};
static_assert(sizeof(XrMotionTrackerLocationsBD) == 184, "XrMotionTrackerLocationsBD ABI mismatch");

using PFN_xrMarkApiClassPICO = XrResult(XRAPI_PTR*)(uint64_t apiClassMask);
using PFN_xrGetMotionTrackerConnectStateBD =
    XrResult(XRAPI_PTR*)(XrInstance instance, XrMotionTrackerConnectStateBD* state);
using PFN_xrGetMotionTrackerModeBD =
    XrResult(XRAPI_PTR*)(XrInstance instance, int32_t* mode);
using PFN_xrGetMotionTrackerLocationsBD =
    XrResult(XRAPI_PTR*)(XrInstance instance, int64_t predictTimeNs,
                         const XrMotionTrackerSNBD* trackerSN, XrMotionTrackerLocationsBD* locations);
using PFN_xrGetBodyTrackerBatteryPICO =
    XrResult(XRAPI_PTR*)(XrInstance instance, int32_t trackerId, int32_t* battery);

} // namespace seamless::pico_xr
