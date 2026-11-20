#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "openvr/openvr_driver.h"
#include "pico_tracker_device.h"
#include "seamless_pico_mt_protocol.h"

namespace seamless::driver {

namespace {

constexpr const char* kSettingsSection = "driver_seamless_pico_mt";
constexpr double kPi = 3.14159265358979323846;

struct QuatD {
    double x = 0.0, y = 0.0, z = 0.0, w = 1.0;
};

struct Vec3D {
    double x = 0.0, y = 0.0, z = 0.0;
};

QuatD quatNormalize(QuatD q) {
    double lenSq = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (lenSq < 1e-12) return {0.0, 0.0, 0.0, 1.0};
    double inv = 1.0 / std::sqrt(lenSq);
    return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

QuatD quatConj(const QuatD& q) {
    return {-q.x, -q.y, -q.z, q.w};
}

QuatD quatMul(const QuatD& a, const QuatD& b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
    };
}

Vec3D quatRotate(const QuatD& q, const Vec3D& v) {
    double tx = 2.0 * (q.y * v.z - q.z * v.y);
    double ty = 2.0 * (q.z * v.x - q.x * v.z);
    double tz = 2.0 * (q.x * v.y - q.y * v.x);
    return {
        v.x + q.w * tx + (q.y * tz - q.z * ty),
        v.y + q.w * ty + (q.z * tx - q.x * tz),
        v.z + q.w * tz + (q.x * ty - q.y * tx)
    };
}

QuatD quatFromMatrix34(const vr::HmdMatrix34_t& m) {
    double trace = m.m[0][0] + m.m[1][1] + m.m[2][2];
    QuatD q{};
    if (trace > 0.0) {
        double s = 0.5 / std::sqrt(trace + 1.0);
        q.w = 0.25 / s;
        q.x = (m.m[2][1] - m.m[1][2]) * s;
        q.y = (m.m[0][2] - m.m[2][0]) * s;
        q.z = (m.m[1][0] - m.m[0][1]) * s;
    } else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {
        double s = 2.0 * std::sqrt(1.0 + m.m[0][0] - m.m[1][1] - m.m[2][2]);
        q.w = (m.m[2][1] - m.m[1][2]) / s;
        q.x = 0.25 * s;
        q.y = (m.m[0][1] + m.m[1][0]) / s;
        q.z = (m.m[0][2] + m.m[2][0]) / s;
    } else if (m.m[1][1] > m.m[2][2]) {
        double s = 2.0 * std::sqrt(1.0 + m.m[1][1] - m.m[0][0] - m.m[2][2]);
        q.w = (m.m[0][2] - m.m[2][0]) / s;
        q.x = (m.m[0][1] + m.m[1][0]) / s;
        q.y = 0.25 * s;
        q.z = (m.m[1][2] + m.m[2][1]) / s;
    } else {
        double s = 2.0 * std::sqrt(1.0 + m.m[2][2] - m.m[0][0] - m.m[1][1]);
        q.w = (m.m[1][0] - m.m[0][1]) / s;
        q.x = (m.m[0][2] + m.m[2][0]) / s;
        q.y = (m.m[1][2] + m.m[2][1]) / s;
        q.z = 0.25 * s;
    }
    return quatNormalize(q);
}

QuatD quatFromYaw(double yawRad) {
    double half = 0.5 * yawRad;
    return {0.0, std::sin(half), 0.0, std::cos(half)};
}

double wrapAngle(double rad) {
    while (rad > kPi)  rad -= 2.0 * kPi;
    while (rad < -kPi) rad += 2.0 * kPi;
    return rad;
}

double extractYaw(const QuatD& q) {
    Vec3D fwd = quatRotate(q, {0.0, 0.0, -1.0});
    return std::atan2(-fwd.x, -fwd.z);
}

double nowSteadySec() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

// Auto4Dof: gravity-aligned yaw+XYZ; Raw: identity; Joint6Dof: direct HMD-relative.
enum class AlignmentMode { Auto4Dof, Raw, Joint6Dof };

class SeamlessPicoServerProvider final : public vr::IServerTrackedDeviceProvider {
public:
    vr::EVRInitError Init(vr::IVRDriverContext* pDriverContext) override {
        VR_INIT_SERVER_DRIVER_CONTEXT(pDriverContext);

        log("Initializing Seamless PICO Motion Tracker OpenVR driver (v1.0)");
        loadSettings();

        if (!enabled_) {
            log("Driver disabled in settings.");
            return vr::VRInitError_None;
        }

        for (uint8_t i = 0; i < seamless::spmt::kBodyRoleCount; ++i) {
            const RoleDescriptor* desc = getBodyRoleDescriptor(i);
            bodyTrackers_[i] = std::make_unique<PicoTrackerDevice>(
                desc->serial,
                desc->name,
                desc->controllerType,
                desc->trackerRole ? desc->trackerRole : "",
                false);
        }

        if (!startUdpListener()) {
            log("WARNING: Failed to bind UDP socket; trackers will remain offline.");
        }

        return vr::VRInitError_None;
    }

    void Cleanup() override {
        stopUdpListener();
        disconnectAllTrackers();
        objectTrackers_.clear();
        for (auto& dev : bodyTrackers_) dev.reset();
        log("Cleanup complete.");
        VR_CLEANUP_SERVER_DRIVER_CONTEXT();
    }

    const char* const* GetInterfaceVersions() override {
        return vr::k_InterfaceVersions;
    }

    void RunFrame() override {
        if (!enabled_) return;

        double now = nowSteadySec();
        seamless::spmt::TrackingPacket pkt{};
        bool hasNewPacket = false;
        double packetRecvSec = 0.0;
        std::vector<std::string> logsToFlush;

        {
            std::lock_guard<std::mutex> lock(pktMutex_);
            if (!pendingLogs_.empty()) logsToFlush.swap(pendingLogs_);
            if (packetDirty_) {
                pkt = latestPacket_;
                packetRecvSec = latestPacketSec_;
                packetDirty_ = false;
                hasNewPacket = true;
            } else {
                packetRecvSec = latestPacketSec_;
            }
        }

        for (const auto& line : logsToFlush) log(line);

        if (hasNewPacket) {
            if (pkt.header.flags & seamless::spmt::kFlagSessionEnding) {
                if (wasStreaming_) log("Received SESSION_ENDING from headset; disconnecting trackers.");
                disconnectAllTrackers();
                wasStreaming_ = false;
                alignInitialized_ = false;
                return;
            }
            applyPacket(pkt, packetRecvSec);
            wasStreaming_ = true;
        } else if (wasStreaming_ && (now - packetRecvSec > timeoutSec_)) {
            log("Headset UDP stream timed out; disconnecting trackers.");
            disconnectAllTrackers();
            wasStreaming_ = false;
            alignInitialized_ = false;
        }
    }

    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override {}
    void LeaveStandby() override {}

private:
    void log(const std::string& msg) {
        if (vr::VRDriverLog() != nullptr) {
            std::string line = "[seamless_pico_mt] " + msg + "\n";
            vr::VRDriverLog()->Log(line.c_str());
        }
    }

    void loadSettings() {
        auto* settings = vr::VRSettings();
        if (!settings) return;

        vr::EVRSettingsError err = vr::VRSettingsError_None;
        bool en = settings->GetBool(kSettingsSection, "enable", &err);
        if (err == vr::VRSettingsError_None) enabled_ = en;

        err = vr::VRSettingsError_None;
        int32_t port = settings->GetInt32(kSettingsSection, "port", &err);
        if (err == vr::VRSettingsError_None && port >= 1 && port <= 65535) {
            udpPort_ = static_cast<uint16_t>(port);
        }

        err = vr::VRSettingsError_None;
        float offset = settings->GetFloat(kSettingsSection, "poseTimeOffset", &err);
        if (err == vr::VRSettingsError_None && std::isfinite(offset)) {
            poseTimeOffsetSec_ = static_cast<double>(offset);
        }

        err = vr::VRSettingsError_None;
        float timeout = settings->GetFloat(kSettingsSection, "timeoutSeconds", &err);
        if (err == vr::VRSettingsError_None && timeout >= 0.25f && timeout <= 30.0f) {
            timeoutSec_ = static_cast<double>(timeout);
        }

        err = vr::VRSettingsError_None;
        int32_t roleMask = settings->GetInt32(kSettingsSection, "roleMaskOverride", &err);
        if (err == vr::VRSettingsError_None && roleMask > 0) {
            roleMaskOverride_ = static_cast<uint16_t>(roleMask & 0x3FFF);
        }

        char modeBuf[64]{};
        err = vr::VRSettingsError_None;
        settings->GetString(kSettingsSection, "alignmentMode", modeBuf, sizeof(modeBuf), &err);
        if (err == vr::VRSettingsError_None) {
            std::string mode(modeBuf);
            if (mode == "raw") {
                alignmentMode_ = AlignmentMode::Raw;
            } else if (mode == "joint") {
                alignmentMode_ = AlignmentMode::Joint6Dof;
            } else {
                alignmentMode_ = AlignmentMode::Auto4Dof;
            }
        }

        char summary[160]{};
        std::snprintf(summary, sizeof(summary),
                      "Settings: port=%u alignment=%s roleMaskOverride=0x%04x",
                      static_cast<unsigned>(udpPort_),
                      alignmentMode_ == AlignmentMode::Raw ? "raw" :
                      alignmentMode_ == AlignmentMode::Joint6Dof ? "joint" : "auto",
                      static_cast<unsigned>(roleMaskOverride_));
        log(summary);
    }

    bool startUdpListener() {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        wsaStarted_ = true;

        SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) {
            WSACleanup();
            wsaStarted_ = false;
            return false;
        }

        BOOL reuse = TRUE;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        DWORD timeoutMs = 20;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

        sockaddr_in bindAddr{};
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
        bindAddr.sin_port = htons(udpPort_);

        if (bind(sock, reinterpret_cast<const sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR) {
            closesocket(sock);
            WSACleanup();
            wsaStarted_ = false;
            return false;
        }

        udpSocket_ = sock;
        running_.store(true);
        rxThread_ = std::thread(&SeamlessPicoServerProvider::udpThreadLoop, this);
        log("Listening for PICO Motion Tracker UDP packets on port " + std::to_string(udpPort_));
        return true;
    }

    void stopUdpListener() {
        running_.store(false);
        if (udpSocket_ != INVALID_SOCKET) {
            closesocket(udpSocket_);
            udpSocket_ = INVALID_SOCKET;
        }
        if (rxThread_.joinable()) rxThread_.join();
        if (wsaStarted_) {
            WSACleanup();
            wsaStarted_ = false;
        }
    }

    void queueLog(std::string msg) {
        std::lock_guard<std::mutex> lock(pktMutex_);
        if (pendingLogs_.size() < 32) pendingLogs_.push_back(std::move(msg));
    }

    void udpThreadLoop() {
        union {
            seamless::spmt::TrackingPacket tracking;
            seamless::spmt::DiscoveryPacket discovery;
        } buf{};
        uint32_t lastSeq = 0;
        bool hasLastSeq = false;
        double lastRecvTime = 0.0;
        double lastDiscoveryLogTime = -100.0;
        uint32_t activeSenderIp = 0;

        while (running_.load()) {
            sockaddr_in sender{};
            int senderLen = sizeof(sender);
            int n = recvfrom(udpSocket_,
                             reinterpret_cast<char*>(&buf),
                             sizeof(buf),
                             0,
                             reinterpret_cast<sockaddr*>(&sender),
                             &senderLen);

            if (n == static_cast<int>(sizeof(seamless::spmt::DiscoveryPacket))) {
                const auto& probe = buf.discovery;
                if (probe.magic == seamless::spmt::kProtocolMagic &&
                    probe.version == seamless::spmt::kProtocolVersion &&
                    probe.type == seamless::spmt::kDiscoveryProbe) {
                    seamless::spmt::DiscoveryPacket reply{};
                    reply.magic = seamless::spmt::kProtocolMagic;
                    reply.version = seamless::spmt::kProtocolVersion;
                    reply.type = seamless::spmt::kDiscoveryReply;
                    reply.nonce = probe.nonce;
                    reply.listenPort = udpPort_;
                    sendto(udpSocket_,
                           reinterpret_cast<const char*>(&reply),
                           sizeof(reply),
                           0,
                           reinterpret_cast<const sockaddr*>(&sender),
                           senderLen);

                    double now = nowSteadySec();
                    if (now - lastDiscoveryLogTime >= 5.0) {
                        lastDiscoveryLogTime = now;
                        char ipStr[INET_ADDRSTRLEN]{};
                        inet_ntop(AF_INET, &sender.sin_addr, ipStr, sizeof(ipStr));
                        char msg[160]{};
                        std::snprintf(msg, sizeof(msg),
                                      "Replied to headset discovery probe from %s:%u",
                                      ipStr[0] ? ipStr : "?",
                                      static_cast<unsigned>(ntohs(sender.sin_port)));
                        queueLog(msg);
                    }
                }
                continue;
            }

            if (n != static_cast<int>(sizeof(seamless::spmt::TrackingPacket))) continue;
            const auto& pkt = buf.tracking;
            if (pkt.header.magic != seamless::spmt::kProtocolMagic ||
                pkt.header.version != seamless::spmt::kProtocolVersion) {
                continue;
            }

            double now = nowSteadySec();
            const bool isEnding = (pkt.header.flags & seamless::spmt::kFlagSessionEnding) != 0;
            // Drop stale out-of-order packets while accepting stream restarts after a >1s gap or SESSION_ENDING.
            const bool isRestart = !hasLastSeq || (now - lastRecvTime > 1.0) || isEnding;
            int32_t seqDelta = static_cast<int32_t>(pkt.header.seq - lastSeq);
            if (!isRestart && seqDelta <= 0) continue;

            const bool streamStarting = !isEnding &&
                (!hasLastSeq || (now - lastRecvTime > timeoutSec_) || (sender.sin_addr.s_addr != activeSenderIp));

            lastSeq = pkt.header.seq;
            hasLastSeq = !isEnding;
            lastRecvTime = now;
            activeSenderIp = isEnding ? 0 : sender.sin_addr.s_addr;

            std::string startLog;
            if (streamStarting) {
                char ipStr[INET_ADDRSTRLEN]{};
                inet_ntop(AF_INET, &sender.sin_addr, ipStr, sizeof(ipStr));
                char msg[200]{};
                std::snprintf(msg, sizeof(msg),
                              "Receiving SPMT stream from %s (hmd=%d mode=%u connected=%u joints=%u objects=%u roleMask=0x%04x)",
                              ipStr[0] ? ipStr : "?",
                              (pkt.header.flags & seamless::spmt::kFlagHasHmdPose) ? 1 : 0,
                              static_cast<unsigned>(pkt.header.motionTrackerMode),
                              static_cast<unsigned>(pkt.header.connectedTrackerCount),
                              static_cast<unsigned>(pkt.header.bodyJointCount),
                              static_cast<unsigned>(pkt.header.objectTrackerCount),
                              static_cast<unsigned>(pkt.header.enabledRoleMask));
                startLog = msg;
            }

            std::lock_guard<std::mutex> lock(pktMutex_);
            latestPacket_ = pkt;
            latestPacketSec_ = now;
            packetDirty_ = true;
            if (!startLog.empty() && pendingLogs_.size() < 32) {
                pendingLogs_.push_back(std::move(startLog));
            }
        }
    }

    void updateAlignment(const seamless::spmt::TrackingPacket& pkt) {
        if (alignmentMode_ == AlignmentMode::Raw) {
            worldFromDriver_ = RigidTransform{};
            return;
        }
        if ((pkt.header.flags & seamless::spmt::kFlagHasHmdPose) == 0) return;

        vr::TrackedDevicePose_t hmdPoses[1]{};
        vr::VRServerDriverHost()->GetRawTrackedDevicePoses(0.0f, hmdPoses, 1);
        const auto& svHmd = hmdPoses[0];
        if (!svHmd.bDeviceIsConnected || !svHmd.bPoseIsValid) return;

        Vec3D pSv{
            static_cast<double>(svHmd.mDeviceToAbsoluteTracking.m[0][3]),
            static_cast<double>(svHmd.mDeviceToAbsoluteTracking.m[1][3]),
            static_cast<double>(svHmd.mDeviceToAbsoluteTracking.m[2][3])
        };
        QuatD qSv = quatFromMatrix34(svHmd.mDeviceToAbsoluteTracking);

        Vec3D pXr{pkt.hmd.pos[0], pkt.hmd.pos[1], pkt.hmd.pos[2]};
        QuatD qXr = quatNormalize({pkt.hmd.quat[0], pkt.hmd.quat[1], pkt.hmd.quat[2], pkt.hmd.quat[3]});

        if (alignmentMode_ == AlignmentMode::Joint6Dof) {
            QuatD qDelta = quatNormalize(quatMul(qSv, quatConj(qXr)));
            Vec3D pRot = quatRotate(qDelta, pXr);
            worldFromDriver_ = {qDelta.x, qDelta.y, qDelta.z, qDelta.w, pSv.x - pRot.x, pSv.y - pRot.y, pSv.z - pRot.z};
            alignInitialized_ = true;
            return;
        }

        // Refine 4-DOF yaw+XYZ slowly while head angular speed is low so jitter never sways feet; snap on recenter.
        double angSpeed = std::sqrt(
            static_cast<double>(svHmd.vAngularVelocity.v[0]) * svHmd.vAngularVelocity.v[0] +
            static_cast<double>(svHmd.vAngularVelocity.v[1]) * svHmd.vAngularVelocity.v[1] +
            static_cast<double>(svHmd.vAngularVelocity.v[2]) * svHmd.vAngularVelocity.v[2]);

        QuatD qRel = quatMul(qSv, quatConj(qXr));
        double measYaw = extractYaw(qRel);
        Vec3D pXrRot = quatRotate(quatFromYaw(measYaw), pXr);
        Vec3D measTrans{pSv.x - pXrRot.x, pSv.y - pXrRot.y, pSv.z - pXrRot.z};

        double yawErr = wrapAngle(measYaw - alignYaw_);
        double dx = measTrans.x - alignTrans_.x;
        double dy = measTrans.y - alignTrans_.y;
        double dz = measTrans.z - alignTrans_.z;
        double posErr = std::sqrt(dx * dx + dy * dy + dz * dz);

        constexpr double kSnapPosMeters = 0.15;
        constexpr double kSnapYawRad = 12.0 * (kPi / 180.0);

        if (!alignInitialized_ || (angSpeed < 0.8 && (posErr > kSnapPosMeters || std::fabs(yawErr) > kSnapYawRad))) {
            alignYaw_ = measYaw;
            alignTrans_ = measTrans;
            alignInitialized_ = true;
        } else if (angSpeed < 0.5) {
            constexpr double kSmoothAlpha = 0.04;
            alignYaw_ = wrapAngle(alignYaw_ + yawErr * kSmoothAlpha);
            alignTrans_.x += dx * kSmoothAlpha;
            alignTrans_.y += dy * kSmoothAlpha;
            alignTrans_.z += dz * kSmoothAlpha;
        }

        QuatD qFinal = quatFromYaw(alignYaw_);
        worldFromDriver_ = {qFinal.x, qFinal.y, qFinal.z, qFinal.w, alignTrans_.x, alignTrans_.y, alignTrans_.z};
    }

    void ensureRegistered(PicoTrackerDevice& dev) {
        if (dev.isRegistered()) return;
        bool ok = vr::VRServerDriverHost()->TrackedDeviceAdded(
            dev.serial().c_str(),
            vr::TrackedDeviceClass_GenericTracker,
            dev.asOpenVrDriver());
        if (ok) {
            dev.markRegistered();
            log("Registered OpenVR tracker: " + dev.serial());
        }
    }

    uint8_t averageBattery(const seamless::spmt::TrackingPacket& pkt) {
        int sum = 0;
        int cnt = 0;
        for (uint32_t i = 0; i < seamless::spmt::kMaxObjectTrackers; ++i) {
            if (pkt.header.batteryPercent[i] <= 100) {
                sum += pkt.header.batteryPercent[i];
                ++cnt;
            }
        }
        return cnt > 0 ? static_cast<uint8_t>(sum / cnt) : 255;
    }

    void applyPacket(const seamless::spmt::TrackingPacket& pkt, double recvSec) {
        updateAlignment(pkt);

        const bool isObjectMode = (pkt.header.motionTrackerMode == seamless::spmt::kModeObjectTracking);
        if (isObjectMode) {
            for (auto& dev : bodyTrackers_) if (dev) dev->setDisconnected();
        } else {
            for (auto& kv : objectTrackers_) if (kv.second) kv.second->setDisconnected();
        }

        if (!isObjectMode && (pkt.header.flags & seamless::spmt::kFlagHasBodyJoints)) {
            uint16_t mask = (roleMaskOverride_ != 0) ? roleMaskOverride_ : pkt.header.enabledRoleMask;
            if (mask == 0) mask = seamless::spmt::kPreset6Pt;
            uint8_t bat = averageBattery(pkt);

            for (uint8_t roleIdx = 0; roleIdx < seamless::spmt::kBodyRoleCount; ++roleIdx) {
                auto& dev = bodyTrackers_[roleIdx];
                if (!dev) continue;

                if ((mask & (1u << roleIdx)) == 0) {
                    dev->setDisconnected();
                    continue;
                }

                const RoleDescriptor* desc = getBodyRoleDescriptor(roleIdx);
                if (!desc || desc->bodyJointIndex >= pkt.header.bodyJointCount) continue;

                const auto& sample = pkt.joints[desc->bodyJointIndex];
                const bool poseValid =
                    (sample.flags & (seamless::spmt::kPoseValidPosition | seamless::spmt::kPoseValidOrientation)) ==
                    (seamless::spmt::kPoseValidPosition | seamless::spmt::kPoseValidOrientation);

                if (poseValid && !dev->isRegistered()) ensureRegistered(*dev);
                if (dev->isActivated()) {
                    dev->updatePose(worldFromDriver_, sample.pos, sample.quat, nullptr, nullptr,
                                    recvSec, poseTimeOffsetSec_, poseValid, bat);
                }
            }
        } else if (isObjectMode && (pkt.header.flags & seamless::spmt::kFlagHasObjectTrackers)) {
            std::vector<std::string> seenSerials;
            const uint32_t count = std::min<uint32_t>(pkt.header.objectTrackerCount, seamless::spmt::kMaxObjectTrackers);
            for (uint32_t i = 0; i < count; ++i) {
                const auto& obj = pkt.objects[i];
                if (obj.serial[0] == '\0') continue;

                std::string rawSn(obj.serial, strnlen(obj.serial, seamless::spmt::kMaxSerialLen));
                std::string fullSerial = "PICO_MT_Obj_" + rawSn;
                seenSerials.push_back(fullSerial);

                auto it = objectTrackers_.find(fullSerial);
                if (it == objectTrackers_.end()) {
                    auto dev = std::make_unique<PicoTrackerDevice>(
                        fullSerial, "Object_" + rawSn, "vive_tracker", "", true);
                    it = objectTrackers_.emplace(fullSerial, std::move(dev)).first;
                }

                auto& dev = *it->second;
                if (obj.valid && !dev.isRegistered()) ensureRegistered(dev);
                if (dev.isActivated()) {
                    dev.updatePose(worldFromDriver_, obj.pos, obj.quat, obj.linVel, obj.angVel,
                                   recvSec, poseTimeOffsetSec_, obj.valid != 0, obj.battery);
                }
            }

            for (auto& kv : objectTrackers_) {
                if (std::find(seenSerials.begin(), seenSerials.end(), kv.first) == seenSerials.end() && kv.second) {
                    kv.second->setDisconnected();
                }
            }
        }
    }

    void disconnectAllTrackers() {
        for (auto& dev : bodyTrackers_) if (dev) dev->setDisconnected();
        for (auto& kv : objectTrackers_) if (kv.second) kv.second->setDisconnected();
    }

    bool enabled_ = true;
    uint16_t udpPort_ = seamless::spmt::kDefaultUdpPort;
    AlignmentMode alignmentMode_ = AlignmentMode::Auto4Dof;
    double poseTimeOffsetSec_ = 0.0;
    double timeoutSec_ = 2.0;
    uint16_t roleMaskOverride_ = 0;

    bool wsaStarted_ = false;
    SOCKET udpSocket_ = INVALID_SOCKET;
    std::atomic<bool> running_{false};
    std::thread rxThread_;

    std::mutex pktMutex_;
    seamless::spmt::TrackingPacket latestPacket_{};
    double latestPacketSec_ = 0.0;
    bool packetDirty_ = false;
    bool wasStreaming_ = false;
    std::vector<std::string> pendingLogs_;

    bool alignInitialized_ = false;
    double alignYaw_ = 0.0;
    Vec3D alignTrans_{};
    RigidTransform worldFromDriver_{};

    std::array<std::unique_ptr<PicoTrackerDevice>, seamless::spmt::kBodyRoleCount> bodyTrackers_{};
    std::unordered_map<std::string, std::unique_ptr<PicoTrackerDevice>> objectTrackers_;
};

SeamlessPicoServerProvider gProvider;

} // namespace

} // namespace seamless::driver

extern "C" __declspec(dllexport) void* HmdDriverFactory(const char* pInterfaceName, int* pReturnCode) {
    if (pInterfaceName && std::strcmp(vr::IServerTrackedDeviceProvider_Version, pInterfaceName) == 0) {
        if (pReturnCode) *pReturnCode = vr::VRInitError_None;
        return &seamless::driver::gProvider;
    }
    if (pReturnCode) *pReturnCode = vr::VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
