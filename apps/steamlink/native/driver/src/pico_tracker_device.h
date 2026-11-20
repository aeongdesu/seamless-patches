#pragma once

#include <cstdint>
#include <string>

#include "openvr/openvr_driver.h"
#include "seamless_pico_mt_protocol.h"

namespace seamless::driver {

struct RoleDescriptor {
    seamless::spmt::BodyRole role;
    uint32_t bodyJointIndex;
    const char* name;
    const char* serial;
    const char* controllerType;
    const char* trackerRole;
};

const RoleDescriptor* getBodyRoleDescriptor(uint8_t roleIndex);

struct RigidTransform {
    double qx = 0.0;
    double qy = 0.0;
    double qz = 0.0;
    double qw = 1.0;
    double tx = 0.0;
    double ty = 0.0;
    double tz = 0.0;
};

class PicoTrackerDevice {
public:
    PicoTrackerDevice(std::string serial,
                      std::string roleName,
                      std::string controllerType,
                      std::string steamVrTrackerRole,
                      bool isObjectTracker);
    ~PicoTrackerDevice();

    const std::string& serial() const { return serial_; }
    bool isRegistered() const { return registered_; }
    void markRegistered() { registered_ = true; }
    bool isActivated() const { return objectId_ != vr::k_unTrackedDeviceIndexInvalid; }

    // Returns an MSVC-ABI-compatible ITrackedDeviceServerDriver pointer for vrserver.exe.
    vr::ITrackedDeviceServerDriver* asOpenVrDriver();

    vr::EVRInitError onActivate(uint32_t unObjectId);
    void onDeactivate();
    void onEnterStandby();
    void* onGetComponent(const char* pchComponentNameAndVersion);
    void onDebugRequest(const char* pchRequest, char* pchResponseBuffer, uint32_t unResponseBufferSize);
    vr::DriverPose_t onGetPose();

    void updatePose(const RigidTransform& worldFromDriver,
                    const float pos[3],
                    const float quat[4],
                    const float* explicitLinVel,
                    const float* explicitAngVel,
                    double timestampSec,
                    double poseTimeOffsetSec,
                    bool trackingValid,
                    uint8_t batteryPercent);

    void setDisconnected();

private:
    void pushBatteryIfChanged(uint8_t batteryPercent);

    std::string serial_;
    std::string roleName_;
    std::string controllerType_;
    std::string steamVrTrackerRole_;
    bool isObjectTracker_ = false;
    bool registered_ = false;

    uint32_t objectId_ = vr::k_unTrackedDeviceIndexInvalid;
    vr::PropertyContainerHandle_t propContainer_ = vr::k_ulInvalidPropertyContainer;
    vr::VRInputComponentHandle_t powerButtonHandle_ = vr::k_ulInvalidInputComponentHandle;

    vr::DriverPose_t lastPose_{};
    bool hasPrevPos_ = false;
    double prevTimestampSec_ = 0.0;
    double prevPos_[3] = {0.0, 0.0, 0.0};
    double smoothVel_[3] = {0.0, 0.0, 0.0};
    int lastBatteryPercent_ = -1;

    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace seamless::driver
