#pragma once

#include <cstdint>

namespace seamless::spmt {

// Little-endian UDP wire protocol shared by the Android OpenXR layer and Windows OpenVR driver.
constexpr uint32_t kProtocolMagic = 0x544D5053u; // "SPMT"
constexpr uint16_t kProtocolVersion = 1u;
constexpr uint16_t kDefaultUdpPort = 39575u;

constexpr uint32_t kBodyJointCount = 24u;
constexpr uint32_t kMaxObjectTrackers = 6u;
constexpr uint32_t kMaxSerialLen = 24u;
constexpr uint32_t kBodyRoleCount = 14u;

constexpr uint16_t kFlagHasHmdPose        = 1u << 0;
constexpr uint16_t kFlagHasBodyJoints     = 1u << 1;
constexpr uint16_t kFlagHasObjectTrackers = 1u << 2;
constexpr uint16_t kFlagAllBodyTracked    = 1u << 3;
constexpr uint16_t kFlagSessionEnding     = 1u << 4;

constexpr uint8_t kDiscoveryProbe = 1u;
constexpr uint8_t kDiscoveryReply = 2u;

struct DiscoveryPacket {
    uint32_t magic;
    uint16_t version;
    uint8_t type;
    uint8_t reserved0;
    uint32_t nonce;
    uint16_t listenPort;
    uint16_t reserved1;
};
static_assert(sizeof(DiscoveryPacket) == 16, "DiscoveryPacket must be 16 bytes");

constexpr uint32_t kPoseValidPosition      = 1u << 0;
constexpr uint32_t kPoseValidOrientation   = 1u << 1;
constexpr uint32_t kPoseTrackedPosition    = 1u << 2;
constexpr uint32_t kPoseTrackedOrientation = 1u << 3;

constexpr uint8_t kModeBodyTracking   = 0u;
constexpr uint8_t kModeObjectTracking = 1u;
constexpr uint8_t kModeUnknown        = 255u;

// Matches PICO Connect driver_pico.dll BodyJointTypeToTrackerRole indices (0..13).
enum BodyRole : uint8_t {
    RoleWaist         = 0,
    RoleLeftHip       = 1,
    RoleRightHip      = 2,
    RoleLeftKnee      = 3,
    RoleRightKnee     = 4,
    RoleChest         = 5,
    RoleLeftFoot      = 6,
    RoleRightFoot     = 7,
    RoleLeftShoulder  = 8,
    RoleRightShoulder = 9,
    RoleLeftElbow     = 10,
    RoleRightElbow    = 11,
    RoleLeftWrist     = 12,
    RoleRightWrist    = 13,
};

constexpr uint16_t roleBit(BodyRole role) {
    return static_cast<uint16_t>(1u << static_cast<uint8_t>(role));
}

constexpr uint16_t kPreset3Pt =
    roleBit(RoleWaist) | roleBit(RoleLeftFoot) | roleBit(RoleRightFoot);
constexpr uint16_t kPreset6Pt =
    kPreset3Pt | roleBit(RoleChest) | roleBit(RoleLeftKnee) | roleBit(RoleRightKnee);
constexpr uint16_t kPreset8Pt =
    kPreset6Pt | roleBit(RoleLeftElbow) | roleBit(RoleRightElbow);
constexpr uint16_t kPreset10Pt =
    kPreset8Pt | roleBit(RoleLeftShoulder) | roleBit(RoleRightShoulder);
constexpr uint16_t kPresetAll =
    kPreset10Pt | roleBit(RoleLeftHip) | roleBit(RoleRightHip) |
    roleBit(RoleLeftWrist) | roleBit(RoleRightWrist);

struct PoseSample {
    float pos[3];
    float quat[4];
    uint32_t flags;
};
static_assert(sizeof(PoseSample) == 32, "PoseSample must be 32 bytes");

struct ObjectTrackerSample {
    char serial[kMaxSerialLen];
    float pos[3];
    float quat[4];
    float linVel[3];
    float angVel[3];
    uint8_t confidence;
    uint8_t battery;
    uint8_t valid;
    uint8_t reserved;
};
static_assert(sizeof(ObjectTrackerSample) == 80, "ObjectTrackerSample must be 80 bytes");

struct PacketHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t seq;
    uint32_t reserved0;
    int64_t displayTimeNs;
    uint8_t motionTrackerMode;
    uint8_t connectedTrackerCount;
    uint8_t bodyJointCount;
    uint8_t objectTrackerCount;
    uint16_t enabledRoleMask;
    uint8_t batteryPercent[kMaxObjectTrackers];
    uint8_t reserved1[12];
};
static_assert(sizeof(PacketHeader) == 48, "PacketHeader must be 48 bytes");

struct TrackingPacket {
    PacketHeader header;
    PoseSample hmd;
    PoseSample joints[kBodyJointCount];
    ObjectTrackerSample objects[kMaxObjectTrackers];
};
static_assert(sizeof(TrackingPacket) == 1328, "TrackingPacket must fit in one 1472-byte UDP MTU");

} // namespace seamless::spmt
