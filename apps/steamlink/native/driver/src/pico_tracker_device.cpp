#include "pico_tracker_device.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace seamless::driver {

namespace {

constexpr RoleDescriptor kBodyRoles[seamless::spmt::kBodyRoleCount] = {
    {seamless::spmt::RoleWaist,         0,  "Waist",         "PICO_MT_Waist",         "vive_tracker_waist",          "TrackerRole_Waist"},
    {seamless::spmt::RoleLeftHip,       1,  "LeftHip",       "PICO_MT_LeftHip",       "vive_tracker",                nullptr},
    {seamless::spmt::RoleRightHip,      2,  "RightHip",      "PICO_MT_RightHip",      "vive_tracker",                nullptr},
    {seamless::spmt::RoleLeftKnee,      4,  "LeftKnee",      "PICO_MT_LeftKnee",      "vive_tracker_left_knee",      "TrackerRole_LeftKnee"},
    {seamless::spmt::RoleRightKnee,     5,  "RightKnee",     "PICO_MT_RightKnee",     "vive_tracker_right_knee",     "TrackerRole_RightKnee"},
    {seamless::spmt::RoleChest,         9,  "Chest",         "PICO_MT_Chest",         "vive_tracker_chest",          "TrackerRole_Chest"},
    {seamless::spmt::RoleLeftFoot,      10, "LeftFoot",      "PICO_MT_LeftFoot",      "vive_tracker_left_foot",      "TrackerRole_LeftFoot"},
    {seamless::spmt::RoleRightFoot,     11, "RightFoot",     "PICO_MT_RightFoot",     "vive_tracker_right_foot",     "TrackerRole_RightFoot"},
    {seamless::spmt::RoleLeftShoulder,  16, "LeftShoulder",  "PICO_MT_LeftShoulder",  "vive_tracker_left_shoulder",  "TrackerRole_LeftShoulder"},
    {seamless::spmt::RoleRightShoulder, 17, "RightShoulder", "PICO_MT_RightShoulder", "vive_tracker_right_shoulder", "TrackerRole_RightShoulder"},
    {seamless::spmt::RoleLeftElbow,     18, "LeftElbow",     "PICO_MT_LeftElbow",     "vive_tracker_left_elbow",     "TrackerRole_LeftElbow"},
    {seamless::spmt::RoleRightElbow,    19, "RightElbow",    "PICO_MT_RightElbow",    "vive_tracker_right_elbow",    "TrackerRole_RightElbow"},
    {seamless::spmt::RoleLeftWrist,     20, "LeftWrist",     "PICO_MT_LeftWrist",     "vive_tracker_left_wrist",     "TrackerRole_LeftWrist"},
    {seamless::spmt::RoleRightWrist,    21, "RightWrist",    "PICO_MT_RightWrist",    "vive_tracker_right_wrist",    "TrackerRole_RightWrist"},
};

vr::DriverPose_t makeDisconnectedPose() {
    vr::DriverPose_t pose{};
    pose.qWorldFromDriverRotation = {1.0, 0.0, 0.0, 0.0};
    pose.qDriverFromHeadRotation = {1.0, 0.0, 0.0, 0.0};
    pose.qRotation = {1.0, 0.0, 0.0, 0.0};
    pose.poseIsValid = false;
    pose.deviceIsConnected = false;
    pose.willDriftInYaw = false;
    pose.shouldApplyHeadModel = false;
    pose.result = vr::TrackingResult_Calibrating_OutOfRange;
    return pose;
}

} // namespace

const RoleDescriptor* getBodyRoleDescriptor(uint8_t roleIndex) {
    if (roleIndex >= seamless::spmt::kBodyRoleCount) return nullptr;
    return &kBodyRoles[roleIndex];
}

// MinGW swaps sret/this for GetPose(); bridge with a 6-slot C vtable matching MSVC x64 (RCX=this, RDX=out, RAX=out).
#if defined(__MINGW32__) || defined(__MINGW64__)

struct PicoTrackerDevice::Impl {
    struct MsvcVTable {
        vr::EVRInitError (*Activate)(Impl* self, uint32_t unObjectId);
        void (*Deactivate)(Impl* self);
        void (*EnterStandby)(Impl* self);
        void* (*GetComponent)(Impl* self, const char* pchComponentNameAndVersion);
        void (*DebugRequest)(Impl* self, const char* pchRequest, char* pchResponseBuffer, uint32_t unResponseBufferSize);
        vr::DriverPose_t* (*GetPose)(Impl* self, vr::DriverPose_t* outPose);
    };

    const MsvcVTable* vptr;
    PicoTrackerDevice* owner;

    static vr::EVRInitError thunkActivate(Impl* self, uint32_t id) { return self->owner->onActivate(id); }
    static void thunkDeactivate(Impl* self) { self->owner->onDeactivate(); }
    static void thunkEnterStandby(Impl* self) { self->owner->onEnterStandby(); }
    static void* thunkGetComponent(Impl* self, const char* name) { return self->owner->onGetComponent(name); }
    static void thunkDebugRequest(Impl* self, const char* req, char* resp, uint32_t size) {
        self->owner->onDebugRequest(req, resp, size);
    }
    static vr::DriverPose_t* thunkGetPose(Impl* self, vr::DriverPose_t* outPose) {
        *outPose = self->owner->onGetPose();
        return outPose;
    }

    static const MsvcVTable kVTable;

    explicit Impl(PicoTrackerDevice* o) : vptr(&kVTable), owner(o) {}
};

const PicoTrackerDevice::Impl::MsvcVTable PicoTrackerDevice::Impl::kVTable = {
    &PicoTrackerDevice::Impl::thunkActivate,
    &PicoTrackerDevice::Impl::thunkDeactivate,
    &PicoTrackerDevice::Impl::thunkEnterStandby,
    &PicoTrackerDevice::Impl::thunkGetComponent,
    &PicoTrackerDevice::Impl::thunkDebugRequest,
    &PicoTrackerDevice::Impl::thunkGetPose,
};

#else

struct PicoTrackerDevice::Impl final : public vr::ITrackedDeviceServerDriver {
    PicoTrackerDevice* owner;
    explicit Impl(PicoTrackerDevice* o) : owner(o) {}

    vr::EVRInitError Activate(uint32_t unObjectId) override { return owner->onActivate(unObjectId); }
    void Deactivate() override { owner->onDeactivate(); }
    void EnterStandby() override { owner->onEnterStandby(); }
    void* GetComponent(const char* name) override { return owner->onGetComponent(name); }
    void DebugRequest(const char* req, char* resp, uint32_t size) override { owner->onDebugRequest(req, resp, size); }
    vr::DriverPose_t GetPose() override { return owner->onGetPose(); }
};

#endif

PicoTrackerDevice::PicoTrackerDevice(std::string serial,
                                     std::string roleName,
                                     std::string controllerType,
                                     std::string steamVrTrackerRole,
                                     bool isObjectTracker)
    : serial_(std::move(serial)),
      roleName_(std::move(roleName)),
      controllerType_(std::move(controllerType)),
      steamVrTrackerRole_(std::move(steamVrTrackerRole)),
      isObjectTracker_(isObjectTracker),
      lastPose_(makeDisconnectedPose()),
      impl_(new Impl(this)) {}

PicoTrackerDevice::~PicoTrackerDevice() {
    delete impl_;
    impl_ = nullptr;
}

vr::ITrackedDeviceServerDriver* PicoTrackerDevice::asOpenVrDriver() {
    return reinterpret_cast<vr::ITrackedDeviceServerDriver*>(impl_);
}

vr::EVRInitError PicoTrackerDevice::onActivate(uint32_t unObjectId) {
    objectId_ = unObjectId;
    propContainer_ = vr::VRProperties()->TrackedDeviceToPropertyContainer(objectId_);

    auto* props = vr::VRProperties();
    props->SetStringProperty(propContainer_, vr::Prop_TrackingSystemName_String, "pico_mt");
    props->SetStringProperty(propContainer_, vr::Prop_ManufacturerName_String, "PICO");
    props->SetStringProperty(propContainer_, vr::Prop_ModelNumber_String,
                             isObjectTracker_ ? "PICO Motion Tracker (Object)" : "PICO Motion Tracker");
    props->SetStringProperty(propContainer_, vr::Prop_SerialNumber_String, serial_.c_str());
    props->SetStringProperty(propContainer_, vr::Prop_RenderModelName_String, "{htc}vr_tracker_vive_3_0");
    props->SetStringProperty(propContainer_, vr::Prop_InputProfilePath_String, "{htc}/input/vive_tracker_profile.json");
    props->SetStringProperty(propContainer_, vr::Prop_ControllerType_String, controllerType_.c_str());

    std::string registeredType = "seamless_pico_mt/tracker/" + roleName_;
    props->SetStringProperty(propContainer_, vr::Prop_RegisteredDeviceType_String, registeredType.c_str());
    props->SetStringProperty(propContainer_, vr::Prop_HardwareRevision_String, "PICO Motion Tracker");
    props->SetBoolProperty(propContainer_, vr::Prop_DeviceIsWireless_Bool, true);
    props->SetBoolProperty(propContainer_, vr::Prop_DeviceIsCharging_Bool, false);
    props->SetBoolProperty(propContainer_, vr::Prop_DeviceProvidesBatteryStatus_Bool, true);
    props->SetFloatProperty(propContainer_, vr::Prop_DeviceBatteryPercentage_Float, 1.0f);
    props->SetBoolProperty(propContainer_, vr::Prop_DeviceCanPowerOff_Bool, true);
    props->SetBoolProperty(propContainer_, vr::Prop_Identifiable_Bool, false);
    props->SetBoolProperty(propContainer_, vr::Prop_HasDisplayComponent_Bool, false);
    props->SetInt32Property(propContainer_, vr::Prop_DeviceClass_Int32, vr::TrackedDeviceClass_GenericTracker);
    props->SetInt32Property(propContainer_, vr::Prop_ControllerRoleHint_Int32, vr::TrackedControllerRole_OptOut);
    props->SetInt32Property(propContainer_, vr::Prop_ControllerHandSelectionPriority_Int32, -1);

    if (!steamVrTrackerRole_.empty() && vr::VRSettings() != nullptr) {
        std::string devicePath = "/devices/seamless_pico_mt/" + serial_;
        vr::VRSettings()->SetString(vr::k_pch_Trackers_Section, devicePath.c_str(), steamVrTrackerRole_.c_str());
    }

    if (vr::VRDriverInput() != nullptr) {
        vr::VRDriverInput()->CreateBooleanComponent(propContainer_, "/input/power/click", &powerButtonHandle_);
    }

    if (vr::VRDriverLog() != nullptr) {
        std::string msg = "[seamless_pico_mt] Activated tracker " + serial_ +
                          " (role=" + roleName_ + ", type=" + controllerType_ + ", id=" + std::to_string(objectId_) + ")\n";
        vr::VRDriverLog()->Log(msg.c_str());
    }

    return vr::VRInitError_None;
}

void PicoTrackerDevice::onDeactivate() {
    objectId_ = vr::k_unTrackedDeviceIndexInvalid;
    propContainer_ = vr::k_ulInvalidPropertyContainer;
    powerButtonHandle_ = vr::k_ulInvalidInputComponentHandle;
    hasPrevPos_ = false;
    lastBatteryPercent_ = -1;
}

void PicoTrackerDevice::onEnterStandby() {}

void* PicoTrackerDevice::onGetComponent(const char*) {
    return nullptr;
}

void PicoTrackerDevice::onDebugRequest(const char*, char* pchResponseBuffer, uint32_t unResponseBufferSize) {
    if (pchResponseBuffer && unResponseBufferSize > 0) pchResponseBuffer[0] = '\0';
}

vr::DriverPose_t PicoTrackerDevice::onGetPose() {
    return lastPose_;
}

void PicoTrackerDevice::pushBatteryIfChanged(uint8_t batteryPercent) {
    if (batteryPercent > 100 || propContainer_ == vr::k_ulInvalidPropertyContainer) return;
    int pct = static_cast<int>(batteryPercent);
    if (pct == lastBatteryPercent_) return;
    lastBatteryPercent_ = pct;
    vr::VRProperties()->SetFloatProperty(
        propContainer_,
        vr::Prop_DeviceBatteryPercentage_Float,
        static_cast<float>(pct) / 100.0f);
}

void PicoTrackerDevice::updatePose(const RigidTransform& worldFromDriver,
                                   const float pos[3],
                                   const float quat[4],
                                   const float* explicitLinVel,
                                   const float* explicitAngVel,
                                   double timestampSec,
                                   double poseTimeOffsetSec,
                                   bool trackingValid,
                                   uint8_t batteryPercent) {
    if (!isActivated()) return;

    vr::DriverPose_t pose{};
    pose.poseTimeOffset = poseTimeOffsetSec;
    pose.qWorldFromDriverRotation = {worldFromDriver.qw, worldFromDriver.qx, worldFromDriver.qy, worldFromDriver.qz};
    pose.vecWorldFromDriverTranslation[0] = worldFromDriver.tx;
    pose.vecWorldFromDriverTranslation[1] = worldFromDriver.ty;
    pose.vecWorldFromDriverTranslation[2] = worldFromDriver.tz;
    pose.qDriverFromHeadRotation = {1.0, 0.0, 0.0, 0.0};
    pose.willDriftInYaw = false;
    pose.shouldApplyHeadModel = false;
    pose.deviceIsConnected = true;

    if (trackingValid) {
        pose.poseIsValid = true;
        pose.result = vr::TrackingResult_Running_OK;
        pose.vecPosition[0] = static_cast<double>(pos[0]);
        pose.vecPosition[1] = static_cast<double>(pos[1]);
        pose.vecPosition[2] = static_cast<double>(pos[2]);
        pose.qRotation = {static_cast<double>(quat[3]), static_cast<double>(quat[0]),
                          static_cast<double>(quat[1]), static_cast<double>(quat[2])};

        if (explicitLinVel != nullptr) {
            pose.vecVelocity[0] = static_cast<double>(explicitLinVel[0]);
            pose.vecVelocity[1] = static_cast<double>(explicitLinVel[1]);
            pose.vecVelocity[2] = static_cast<double>(explicitLinVel[2]);
        } else if (hasPrevPos_) {
            double dt = timestampSec - prevTimestampSec_;
            if (dt > 0.002 && dt < 0.2) {
                constexpr double kAlpha = 0.35;
                constexpr double kMaxVel = 8.0;
                for (int i = 0; i < 3; ++i) {
                    double v = std::clamp((pose.vecPosition[i] - prevPos_[i]) / dt, -kMaxVel, kMaxVel);
                    smoothVel_[i] = smoothVel_[i] * (1.0 - kAlpha) + v * kAlpha;
                    pose.vecVelocity[i] = smoothVel_[i];
                }
            }
        }

        if (explicitAngVel != nullptr) {
            pose.vecAngularVelocity[0] = static_cast<double>(explicitAngVel[0]);
            pose.vecAngularVelocity[1] = static_cast<double>(explicitAngVel[1]);
            pose.vecAngularVelocity[2] = static_cast<double>(explicitAngVel[2]);
        }

        prevPos_[0] = pose.vecPosition[0];
        prevPos_[1] = pose.vecPosition[1];
        prevPos_[2] = pose.vecPosition[2];
        prevTimestampSec_ = timestampSec;
        hasPrevPos_ = true;
    } else {
        pose.poseIsValid = false;
        pose.result = vr::TrackingResult_Running_OutOfRange;
        pose.vecPosition[0] = lastPose_.vecPosition[0];
        pose.vecPosition[1] = lastPose_.vecPosition[1];
        pose.vecPosition[2] = lastPose_.vecPosition[2];
        pose.qRotation = lastPose_.qRotation;
        hasPrevPos_ = false;
    }

    lastPose_ = pose;
    pushBatteryIfChanged(batteryPercent);
    vr::VRServerDriverHost()->TrackedDevicePoseUpdated(objectId_, lastPose_, sizeof(vr::DriverPose_t));
}

void PicoTrackerDevice::setDisconnected() {
    if (!isActivated()) return;
    if (!lastPose_.deviceIsConnected && !lastPose_.poseIsValid) return;

    lastPose_ = makeDisconnectedPose();
    hasPrevPos_ = false;
    vr::VRServerDriverHost()->TrackedDevicePoseUpdated(objectId_, lastPose_, sizeof(vr::DriverPose_t));
}

} // namespace seamless::driver
