#include <android/log.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#define XR_NO_PROTOTYPES 1
#include "openxr/openxr.h"
#include "openxr/openxr_loader_negotiation.h"

#include "pico_openxr_ext.h"
#include "seamless_pico_mt_protocol.h"

namespace {

using namespace seamless::pico_xr;
using namespace seamless::spmt;

constexpr const char* kLogTag = "PicoMT-Layer";
constexpr const char* kLayerName = "XR_APILAYER_SEAMLESS_pico_motion_tracker";
constexpr const char* kDefaultConfigPath =
    "/data/user/0/com.valvesoftware.steamlinkvr/files/pico_motion_tracker.conf";

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, kLogTag, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, kLogTag, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, kLogTag, __VA_ARGS__)

int64_t monotonicNowMs() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000LL + static_cast<int64_t>(ts.tv_nsec) / 1000000LL;
}

enum class TrackingModePref { Auto, Body, Object };

std::string trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n')) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) {
        --end;
    }
    return s.substr(start, end - start);
}

bool parseBool(const std::string& v, bool fallback) {
    if (v == "1" || v == "true" || v == "TRUE" || v == "yes") return true;
    if (v == "0" || v == "false" || v == "FALSE" || v == "no") return false;
    return fallback;
}

uint16_t parseRoleMaskFromPreset(const std::string& v) {
    if (v == "3pt")  return kPreset3Pt;
    if (v == "8pt")  return kPreset8Pt;
    if (v == "10pt") return kPreset10Pt;
    if (v == "all")  return kPresetAll;
    return kPreset6Pt;
}

TrackingModePref parseTrackingMode(const std::string& v) {
    if (v == "body")   return TrackingModePref::Body;
    if (v == "object") return TrackingModePref::Object;
    return TrackingModePref::Auto;
}

const char* trackingModeName(TrackingModePref m) {
    switch (m) {
        case TrackingModePref::Body:   return "body";
        case TrackingModePref::Object: return "object";
        case TrackingModePref::Auto:   return "auto";
    }
    return "auto";
}

struct RuntimeConfig {
    bool enabled = true;
    std::string host;
    int port = kDefaultUdpPort;
    uint16_t enabledRoleMask = kPreset6Pt;
    TrackingModePref trackingMode = TrackingModePref::Auto;
    std::string configPath;
    timespec lastFileMtim{};
    int64_t lastCheckMs = 0;
};

struct DispatchTable {
    PFN_xrGetInstanceProcAddr getInstanceProcAddr = nullptr;
    PFN_xrDestroyInstance destroyInstance = nullptr;
    PFN_xrGetSystemProperties getSystemProperties = nullptr;
    PFN_xrCreateSession createSession = nullptr;
    PFN_xrDestroySession destroySession = nullptr;
    PFN_xrBeginSession beginSession = nullptr;
    PFN_xrEndSession endSession = nullptr;
    PFN_xrPollEvent pollEvent = nullptr;
    PFN_xrCreateReferenceSpace createReferenceSpace = nullptr;
    PFN_xrDestroySpace destroySpace = nullptr;
    PFN_xrLocateSpace locateSpace = nullptr;
    PFN_xrWaitFrame waitFrame = nullptr;

    bool bodyTrackingExtEnabled = false;
    PFN_xrCreateBodyTrackerBD createBodyTrackerBD = nullptr;
    PFN_xrDestroyBodyTrackerBD destroyBodyTrackerBD = nullptr;
    PFN_xrLocateBodyJointsBD locateBodyJointsBD = nullptr;

    bool motionTrackingExtEnabled = false;
    PFN_xrGetMotionTrackerConnectStateBD getMotionTrackerConnectStateBD = nullptr;
    PFN_xrGetMotionTrackerModeBD getMotionTrackerModeBD = nullptr;
    PFN_xrGetMotionTrackerLocationsBD getMotionTrackerLocationsBD = nullptr;

    bool picoBodyTrackingExtEnabled = false;
    PFN_xrGetBodyTrackerBatteryPICO getBodyTrackerBatteryPICO = nullptr;
};

struct LayerState {
    std::mutex mutex;

    XrInstance instance = XR_NULL_HANDLE;
    DispatchTable next{};

    XrSession session = XR_NULL_HANDLE;
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    bool sessionBegun = false;
    bool sessionEndingSent = false;

    XrSpace appStageSpace = XR_NULL_HANDLE;
    bool appStageSpaceIsIdentity = false;
    XrSpace appLocalSpace = XR_NULL_HANDLE;
    XrSpace appViewSpace = XR_NULL_HANDLE;
    XrSpace ownedBaseSpace = XR_NULL_HANDLE;
    XrSpace ownedViewSpace = XR_NULL_HANDLE;

    XrBodyTrackerBD bodyTracker = XR_NULL_HANDLE;
    int64_t lastTrackerAttemptMs = 0;

    int64_t lastMetaPollMs = 0;
    int64_t lastBatteryPollMs = 0;
    int32_t runtimeMotionMode = -1;
    XrMotionTrackerConnectStateBD connectState{};
    std::array<uint8_t, kMaxObjectTrackers> batteryPercent{255, 255, 255, 255, 255, 255};

    uint32_t packetSeq = 0;
    uint64_t framesSent = 0;
    uint64_t framesBodyTracked = 0;
    uint64_t framesObjectTracked = 0;
    int64_t lastHeartbeatLogMs = 0;
    bool loggedNoHost = false;

    RuntimeConfig cfg;

    int udpFd = -1;
    sockaddr_in destAddr{};
    bool destValid = false;
    std::string activeHost;
    int activePort = 0;
    std::string discoveredHost;
    int discoveredPort = 0;
    int64_t lastDiscoveryProbeMs = 0;
    uint32_t discoveryNonce = 0;
};

LayerState gState;

void applyKeyValue(RuntimeConfig& cfg, const std::string& key, const std::string& value) {
    if (key == "enabled") {
        cfg.enabled = parseBool(value, cfg.enabled);
    } else if (key == "host") {
        cfg.host = value;
    } else if (key == "port") {
        int p = std::atoi(value.c_str());
        if (p >= 1 && p <= 65535) cfg.port = p;
    } else if (key == "tracker_preset") {
        cfg.enabledRoleMask = parseRoleMaskFromPreset(value);
    } else if (key == "tracking_mode") {
        cfg.trackingMode = parseTrackingMode(value);
    }
}

void loadConfigFromEnvLocked(RuntimeConfig& cfg) {
    auto readEnv = [](const char* name) -> std::string {
        const char* v = std::getenv(name);
        return v ? trim(v) : std::string();
    };

    if (auto v = readEnv("PICO_MT_ENABLED"); !v.empty()) applyKeyValue(cfg, "enabled", v);
    if (auto v = readEnv("PICO_MT_HOST"); !v.empty()) applyKeyValue(cfg, "host", v);
    if (auto v = readEnv("PICO_MT_PORT"); !v.empty()) applyKeyValue(cfg, "port", v);
    if (auto v = readEnv("PICO_MT_TRACKER_PRESET"); !v.empty()) applyKeyValue(cfg, "tracker_preset", v);
    if (auto v = readEnv("PICO_MT_TRACKING_MODE"); !v.empty()) applyKeyValue(cfg, "tracking_mode", v);
    if (auto v = readEnv("PICO_MT_CONFIG_PATH"); !v.empty()) {
        cfg.configPath = v;
    } else if (cfg.configPath.empty()) {
        cfg.configPath = kDefaultConfigPath;
    }
}

void loadConfigFileLocked(RuntimeConfig& cfg, bool force) {
    if (cfg.configPath.empty()) return;

    struct stat st {};
    if (stat(cfg.configPath.c_str(), &st) != 0) return;
    if (!force &&
        st.st_mtim.tv_sec == cfg.lastFileMtim.tv_sec &&
        st.st_mtim.tv_nsec == cfg.lastFileMtim.tv_nsec) {
        return;
    }

    FILE* fp = std::fopen(cfg.configPath.c_str(), "r");
    if (!fp) return;

    char buf[512];
    while (std::fgets(buf, sizeof(buf), fp) != nullptr) {
        std::string trimmed = trim(buf);
        if (trimmed.empty() || trimmed[0] == '#') continue;
        size_t eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        applyKeyValue(cfg, trim(trimmed.substr(0, eq)), trim(trimmed.substr(eq + 1)));
    }
    std::fclose(fp);

    cfg.lastFileMtim = st.st_mtim;
    LOGI("Loaded config %s: enabled=%d host=%s port=%d roleMask=0x%04x mode=%s",
         cfg.configPath.c_str(),
         cfg.enabled ? 1 : 0,
         cfg.host.empty() ? "<unset>" : cfg.host.c_str(),
         cfg.port,
         static_cast<unsigned>(cfg.enabledRoleMask),
         trackingModeName(cfg.trackingMode));
}

void refreshConfigIfNeededLocked(int64_t nowMs) {
    if (nowMs - gState.cfg.lastCheckMs < 1000) return;
    gState.cfg.lastCheckMs = nowMs;
    loadConfigFileLocked(gState.cfg, false);
}

void closeUdpSocketLocked() {
    if (gState.udpFd >= 0) {
        close(gState.udpFd);
        gState.udpFd = -1;
    }
    gState.destValid = false;
    gState.activeHost.clear();
    gState.activePort = 0;
    gState.discoveredHost.clear();
    gState.discoveredPort = 0;
    gState.lastDiscoveryProbeMs = 0;
}

bool ensureUdpFdOpenLocked() {
    if (gState.udpFd >= 0) return true;
    gState.udpFd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (gState.udpFd < 0) {
        LOGE("Failed to create UDP socket");
        return false;
    }
    int broadcast = 1;
    setsockopt(gState.udpFd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    return true;
}

void pollDiscoveryRepliesLocked() {
    if (gState.udpFd < 0) return;

    DiscoveryPacket reply{};
    sockaddr_in sender{};
    for (int i = 0; i < 8; ++i) {
        socklen_t senderLen = sizeof(sender);
        ssize_t n = recvfrom(gState.udpFd, &reply, sizeof(reply), MSG_DONTWAIT,
                             reinterpret_cast<sockaddr*>(&sender), &senderLen);
        if (n <= 0) break;
        if (n != static_cast<ssize_t>(sizeof(DiscoveryPacket))) continue;
        if (reply.magic != kProtocolMagic || reply.version != kProtocolVersion || reply.type != kDiscoveryReply) {
            continue;
        }

        char ipStr[INET_ADDRSTRLEN]{};
        if (inet_ntop(AF_INET, &sender.sin_addr, ipStr, sizeof(ipStr)) == nullptr) continue;

        int replyPort = (reply.listenPort >= 1) ? static_cast<int>(reply.listenPort) : gState.cfg.port;
        if (gState.discoveredHost != ipStr || gState.discoveredPort != replyPort) {
            gState.discoveredHost = ipStr;
            gState.discoveredPort = replyPort;
            LOGI("Discovered Seamless OpenVR driver at %s:%d via LAN UDP probe",
                 gState.discoveredHost.c_str(), gState.discoveredPort);
        }
    }
}

void sendDiscoveryProbeLocked(int64_t nowMs) {
    if (nowMs - gState.lastDiscoveryProbeMs < 1000) return;
    gState.lastDiscoveryProbeMs = nowMs;
    if (!ensureUdpFdOpenLocked()) return;

    DiscoveryPacket probe{};
    probe.magic = kProtocolMagic;
    probe.version = kProtocolVersion;
    probe.type = kDiscoveryProbe;
    probe.nonce = ++gState.discoveryNonce;

    const uint16_t netPort = htons(static_cast<uint16_t>(gState.cfg.port));
    std::vector<uint32_t> targets;
    targets.push_back(htonl(INADDR_BROADCAST));

    ifaddrs* ifaList = nullptr;
    if (getifaddrs(&ifaList) == 0 && ifaList != nullptr) {
        for (ifaddrs* ifa = ifaList; ifa != nullptr; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || !ifa->ifa_broadaddr) continue;
            if ((ifa->ifa_flags & (IFF_UP | IFF_BROADCAST)) != (IFF_UP | IFF_BROADCAST)) continue;
            if (ifa->ifa_flags & IFF_LOOPBACK) continue;
            if (ifa->ifa_addr->sa_family != AF_INET || ifa->ifa_broadaddr->sa_family != AF_INET) continue;
            uint32_t bcast = reinterpret_cast<const sockaddr_in*>(ifa->ifa_broadaddr)->sin_addr.s_addr;
            if (bcast != 0 && std::find(targets.begin(), targets.end(), bcast) == targets.end()) {
                targets.push_back(bcast);
            }
        }
        freeifaddrs(ifaList);
    }

    for (uint32_t ipNet : targets) {
        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port = netPort;
        dst.sin_addr.s_addr = ipNet;
        sendto(gState.udpFd, &probe, sizeof(probe), MSG_DONTWAIT,
               reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
    }
}

void ensureUdpSocketLocked(int64_t nowMs) {
    if (!ensureUdpFdOpenLocked()) {
        gState.destValid = false;
        return;
    }

    if (gState.cfg.host.empty()) pollDiscoveryRepliesLocked();

    const std::string& targetHost = !gState.cfg.host.empty() ? gState.cfg.host : gState.discoveredHost;
    const int targetPort = (!gState.cfg.host.empty() || gState.discoveredPort <= 0)
        ? gState.cfg.port : gState.discoveredPort;

    if (targetHost.empty()) {
        gState.destValid = false;
        sendDiscoveryProbeLocked(nowMs);
        if (!gState.loggedNoHost) {
            LOGI("PC host IP not set; broadcasting LAN discovery probes on UDP %d", gState.cfg.port);
            gState.loggedNoHost = true;
        }
        return;
    }

    if (gState.destValid && gState.activeHost == targetHost && gState.activePort == targetPort) return;

    in_addr addr{};
    if (inet_pton(AF_INET, targetHost.c_str(), &addr) != 1) {
        LOGE("Invalid IPv4 address for PC driver host: '%s'", targetHost.c_str());
        gState.destValid = false;
        return;
    }

    std::memset(&gState.destAddr, 0, sizeof(gState.destAddr));
    gState.destAddr.sin_family = AF_INET;
    gState.destAddr.sin_port = htons(static_cast<uint16_t>(targetPort));
    gState.destAddr.sin_addr = addr;
    gState.activeHost = targetHost;
    gState.activePort = targetPort;
    gState.destValid = true;
    gState.loggedNoHost = false;

    LOGI("SPMT UDP target set to %s:%d (roleMask=0x%04x, mode=%s)",
         gState.activeHost.c_str(), gState.activePort,
         static_cast<unsigned>(gState.cfg.enabledRoleMask),
         trackingModeName(gState.cfg.trackingMode));
}

void sendPacketLocked(const TrackingPacket& pkt) {
    if (gState.udpFd < 0 || !gState.destValid) return;
    sendto(gState.udpFd, &pkt, sizeof(pkt), MSG_DONTWAIT,
           reinterpret_cast<const sockaddr*>(&gState.destAddr), sizeof(gState.destAddr));
}

void sendSessionEndingLocked() {
    if (gState.udpFd < 0 || !gState.destValid || gState.sessionEndingSent) return;
    gState.sessionEndingSent = true;
    TrackingPacket pkt{};
    pkt.header.magic = kProtocolMagic;
    pkt.header.version = kProtocolVersion;
    pkt.header.flags = kFlagSessionEnding;
    pkt.header.motionTrackerMode = kModeUnknown;
    std::memset(pkt.header.batteryPercent, 255, sizeof(pkt.header.batteryPercent));
    for (int i = 0; i < 3; ++i) {
        pkt.header.seq = ++gState.packetSeq;
        sendPacketLocked(pkt);
    }
    LOGI("Sent SESSION_ENDING packet to %s:%d", gState.activeHost.c_str(), gState.activePort);
}

bool isIdentityPose(const XrPosef& p) {
    return std::fabs(p.position.x) < 1e-4f && std::fabs(p.position.y) < 1e-4f &&
           std::fabs(p.position.z) < 1e-4f && std::fabs(p.orientation.x) < 1e-4f &&
           std::fabs(p.orientation.y) < 1e-4f && std::fabs(p.orientation.z) < 1e-4f &&
           std::fabs(std::fabs(p.orientation.w) - 1.0f) < 1e-3f;
}

bool isFiniteVec(const XrVector3f& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool normalizeQuat(XrQuaternionf& q) {
    if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w)) return false;
    float lenSq = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (lenSq < 1e-10f) return false;
    float invLen = 1.0f / std::sqrt(lenSq);
    q.x *= invLen;
    q.y *= invLen;
    q.z *= invLen;
    q.w *= invLen;
    return true;
}

void fillPoseSample(PoseSample& dst, XrPosef pose, XrSpaceLocationFlags xrFlags) {
    std::memset(&dst, 0, sizeof(dst));
    constexpr XrSpaceLocationFlags kValid =
        XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if ((xrFlags & kValid) != kValid) return;
    if (!isFiniteVec(pose.position) || !normalizeQuat(pose.orientation)) return;

    dst.pos[0] = pose.position.x;
    dst.pos[1] = pose.position.y;
    dst.pos[2] = pose.position.z;
    dst.quat[0] = pose.orientation.x;
    dst.quat[1] = pose.orientation.y;
    dst.quat[2] = pose.orientation.z;
    dst.quat[3] = pose.orientation.w;

    uint32_t f = kPoseValidPosition | kPoseValidOrientation;
    if (xrFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT)    f |= kPoseTrackedPosition;
    if (xrFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) f |= kPoseTrackedOrientation;
    dst.flags = f;
}

void destroySessionResourcesLocked() {
    sendSessionEndingLocked();

    if (gState.bodyTracker != XR_NULL_HANDLE && gState.next.destroyBodyTrackerBD) {
        gState.next.destroyBodyTrackerBD(gState.bodyTracker);
        gState.bodyTracker = XR_NULL_HANDLE;
    }
    if (gState.ownedBaseSpace != XR_NULL_HANDLE && gState.next.destroySpace) {
        gState.next.destroySpace(gState.ownedBaseSpace);
        gState.ownedBaseSpace = XR_NULL_HANDLE;
    }
    if (gState.ownedViewSpace != XR_NULL_HANDLE && gState.next.destroySpace) {
        gState.next.destroySpace(gState.ownedViewSpace);
        gState.ownedViewSpace = XR_NULL_HANDLE;
    }

    gState.appStageSpace = XR_NULL_HANDLE;
    gState.appStageSpaceIsIdentity = false;
    gState.appLocalSpace = XR_NULL_HANDLE;
    gState.appViewSpace = XR_NULL_HANDLE;
    gState.session = XR_NULL_HANDLE;
    gState.sessionState = XR_SESSION_STATE_UNKNOWN;
    gState.sessionBegun = false;
    gState.lastTrackerAttemptMs = 0;
}

void resetInstanceLocked() {
    destroySessionResourcesLocked();
    closeUdpSocketLocked();
    gState.instance = XR_NULL_HANDLE;
    gState.next = DispatchTable{};
    gState.runtimeMotionMode = -1;
    gState.connectState = XrMotionTrackerConnectStateBD{};
    gState.batteryPercent.fill(255);
}

XrSpace createRefSpaceLocked(XrReferenceSpaceType type) {
    if (gState.session == XR_NULL_HANDLE || !gState.next.createReferenceSpace) return XR_NULL_HANDLE;
    XrReferenceSpaceCreateInfo sci{};
    sci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    sci.referenceSpaceType = type;
    sci.poseInReferenceSpace = XrPosef{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    XrSpace space = XR_NULL_HANDLE;
    return XR_SUCCEEDED(gState.next.createReferenceSpace(gState.session, &sci, &space)) ? space : XR_NULL_HANDLE;
}

XrSpace getOrCreateBaseSpaceLocked() {
    if (gState.appStageSpace != XR_NULL_HANDLE) return gState.appStageSpace;
    if (gState.ownedBaseSpace != XR_NULL_HANDLE) return gState.ownedBaseSpace;
    if (gState.appLocalSpace != XR_NULL_HANDLE) return gState.appLocalSpace;

    XrSpace space = createRefSpaceLocked(XR_REFERENCE_SPACE_TYPE_STAGE);
    if (space == XR_NULL_HANDLE) space = createRefSpaceLocked(XR_REFERENCE_SPACE_TYPE_LOCAL);
    if (space != XR_NULL_HANDLE) gState.ownedBaseSpace = space;
    return gState.ownedBaseSpace;
}

XrSpace getOrCreateViewSpaceLocked() {
    if (gState.appViewSpace != XR_NULL_HANDLE) return gState.appViewSpace;
    if (gState.ownedViewSpace != XR_NULL_HANDLE) return gState.ownedViewSpace;
    gState.ownedViewSpace = createRefSpaceLocked(XR_REFERENCE_SPACE_TYPE_VIEW);
    return gState.ownedViewSpace;
}

bool ensureBodyTrackerLocked(int64_t nowMs) {
    if (gState.bodyTracker != XR_NULL_HANDLE) return true;
    if (!gState.next.bodyTrackingExtEnabled || !gState.next.createBodyTrackerBD ||
        gState.session == XR_NULL_HANDLE) {
        return false;
    }
    if (!gState.sessionBegun &&
        gState.sessionState != XR_SESSION_STATE_SYNCHRONIZED &&
        gState.sessionState != XR_SESSION_STATE_VISIBLE &&
        gState.sessionState != XR_SESSION_STATE_FOCUSED) {
        return false;
    }
    if (nowMs - gState.lastTrackerAttemptMs < 3000) return false;
    gState.lastTrackerAttemptMs = nowMs;

    XrBodyTrackerCreateInfoBD ci{};
    ci.type = XR_TYPE_BODY_TRACKER_CREATE_INFO_BD;
    ci.jointSet = XR_BODY_JOINT_SET_FULL_BODY_JOINTS_BD;

    XrBodyTrackerBD tracker = XR_NULL_HANDLE;
    XrResult res = gState.next.createBodyTrackerBD(gState.session, &ci, &tracker);
    if (XR_SUCCEEDED(res) && tracker != XR_NULL_HANDLE) {
        gState.bodyTracker = tracker;
        LOGI("Created XrBodyTrackerBD (FULL_BODY_JOINTS, 24 joints) successfully");
        return true;
    }

    LOGW("xrCreateBodyTrackerBD failed (XrResult=%d) - retrying in 3s", static_cast<int>(res));
    return false;
}

void pollMotionTrackerMetadataLocked(int64_t nowMs) {
    if (gState.instance == XR_NULL_HANDLE) return;

    if (gState.next.motionTrackingExtEnabled && (nowMs - gState.lastMetaPollMs >= 500)) {
        gState.lastMetaPollMs = nowMs;

        if (gState.next.getMotionTrackerModeBD) {
            int32_t mode = -1;
            if (XR_SUCCEEDED(gState.next.getMotionTrackerModeBD(gState.instance, &mode)) &&
                mode != gState.runtimeMotionMode) {
                LOGI("PICO runtime motion tracker mode changed: %d -> %d (0=body, 1=object)",
                     gState.runtimeMotionMode, mode);
                gState.runtimeMotionMode = mode;
            }
        }

        if (gState.next.getMotionTrackerConnectStateBD) {
            XrMotionTrackerConnectStateBD cs{};
            if (XR_SUCCEEDED(gState.next.getMotionTrackerConnectStateBD(gState.instance, &cs))) {
                cs.trackerSum = std::clamp(cs.trackerSum, 0, static_cast<int32_t>(kMaxObjectTrackers));
                if (cs.trackerSum != gState.connectState.trackerSum) {
                    LOGI("Connected PICO Motion Trackers: %d", cs.trackerSum);
                }
                gState.connectState = cs;
            }
        }
    }

    if (gState.next.picoBodyTrackingExtEnabled && gState.next.getBodyTrackerBatteryPICO &&
        (nowMs - gState.lastBatteryPollMs >= 5000)) {
        gState.lastBatteryPollMs = nowMs;
        if (gState.next.motionTrackingExtEnabled && gState.connectState.trackerSum == 0) {
            gState.batteryPercent.fill(255);
            return;
        }
        const int count = std::min(gState.connectState.trackerSum > 0 ? gState.connectState.trackerSum : 3,
                                   static_cast<int>(kMaxObjectTrackers));
        for (int i = 0; i < count; ++i) {
            int32_t rawBat = -1;
            if (XR_SUCCEEDED(gState.next.getBodyTrackerBatteryPICO(gState.instance, i, &rawBat)) && rawBat >= 0) {
                // Runtime reports 0..5 level bars on some firmware or 0..100 percent directly.
                int pct = (rawBat <= 5) ? (rawBat * 20) : std::min(rawBat, 100);
                gState.batteryPercent[i] = static_cast<uint8_t>(pct);
            }
        }
    }
}

void onFrameTick(XrSession session, XrTime predictedDisplayTime) {
    std::lock_guard<std::mutex> lock(gState.mutex);

    if (session != gState.session || predictedDisplayTime <= 0) return;

    int64_t nowMs = monotonicNowMs();
    refreshConfigIfNeededLocked(nowMs);
    if (!gState.cfg.enabled) return;

    ensureUdpSocketLocked(nowMs);
    pollMotionTrackerMetadataLocked(nowMs);

    XrSpace baseSpace = getOrCreateBaseSpaceLocked();
    if (baseSpace == XR_NULL_HANDLE) return;

    TrackingPacket pkt{};
    pkt.header.magic = kProtocolMagic;
    pkt.header.version = kProtocolVersion;
    pkt.header.seq = ++gState.packetSeq;
    pkt.header.displayTimeNs = predictedDisplayTime;
    pkt.header.enabledRoleMask = gState.cfg.enabledRoleMask;
    pkt.header.connectedTrackerCount = static_cast<uint8_t>(std::clamp(gState.connectState.trackerSum, 0, 6));
    std::memcpy(pkt.header.batteryPercent, gState.batteryPercent.data(), sizeof(pkt.header.batteryPercent));

    uint8_t effectiveMode = kModeBodyTracking;
    if (gState.cfg.trackingMode == TrackingModePref::Object) {
        effectiveMode = kModeObjectTracking;
    } else if (gState.cfg.trackingMode == TrackingModePref::Auto && gState.runtimeMotionMode == 1) {
        effectiveMode = kModeObjectTracking;
    }
    pkt.header.motionTrackerMode = effectiveMode;

    XrSpace viewSpace = getOrCreateViewSpaceLocked();
    if (viewSpace != XR_NULL_HANDLE && gState.next.locateSpace) {
        XrSpaceLocation hmdLoc{XR_TYPE_SPACE_LOCATION, nullptr, 0, {}};
        if (XR_SUCCEEDED(gState.next.locateSpace(viewSpace, baseSpace, predictedDisplayTime, &hmdLoc))) {
            fillPoseSample(pkt.hmd, hmdLoc.pose, hmdLoc.locationFlags);
            if (pkt.hmd.flags != 0) pkt.header.flags |= kFlagHasHmdPose;
        }
    }

    size_t validJoints = 0;
    if (effectiveMode == kModeBodyTracking && ensureBodyTrackerLocked(nowMs) && gState.next.locateBodyJointsBD) {
        XrBodyJointLocationBD jointLocations[XR_BODY_JOINT_COUNT_BD]{};
        XrBodyJointLocationsBD locations{};
        locations.type = XR_TYPE_BODY_JOINT_LOCATIONS_BD;
        locations.jointLocationCount = XR_BODY_JOINT_COUNT_BD;
        locations.jointLocations = jointLocations;

        XrBodyJointsLocateInfoBD locateInfo{};
        locateInfo.type = XR_TYPE_BODY_JOINTS_LOCATE_INFO_BD;
        locateInfo.baseSpace = baseSpace;
        locateInfo.time = predictedDisplayTime;

        XrResult locateRes = gState.next.locateBodyJointsBD(gState.bodyTracker, &locateInfo, &locations);
        if (XR_SUCCEEDED(locateRes)) {
            const uint32_t count = std::min(locations.jointLocationCount, kBodyJointCount);
            for (uint32_t i = 0; i < count; ++i) {
                fillPoseSample(pkt.joints[i], jointLocations[i].pose, jointLocations[i].locationFlags);
                if (pkt.joints[i].flags != 0) ++validJoints;
            }
            if (validJoints > 0) {
                pkt.header.bodyJointCount = static_cast<uint8_t>(count);
                pkt.header.flags |= kFlagHasBodyJoints;
                if (locations.allJointPosesTracked) pkt.header.flags |= kFlagAllBodyTracked;
                ++gState.framesBodyTracked;
            }
        } else {
            LOGW("xrLocateBodyJointsBD failed (XrResult=%d); resetting body tracker", static_cast<int>(locateRes));
            if (gState.next.destroyBodyTrackerBD) gState.next.destroyBodyTrackerBD(gState.bodyTracker);
            gState.bodyTracker = XR_NULL_HANDLE;
            gState.lastTrackerAttemptMs = nowMs;
        }
    }

    uint8_t validObjects = 0;
    if (effectiveMode == kModeObjectTracking &&
        gState.next.motionTrackingExtEnabled &&
        gState.next.getMotionTrackerLocationsBD &&
        gState.connectState.trackerSum > 0) {
        const int count = std::min(gState.connectState.trackerSum, static_cast<int>(kMaxObjectTrackers));
        for (int i = 0; i < count; ++i) {
            const XrMotionTrackerSNBD& sn = gState.connectState.trackersSN[i];
            if (sn.value[0] == '\0') continue;

            XrMotionTrackerLocationsBD loc{};
            if (XR_FAILED(gState.next.getMotionTrackerLocationsBD(gState.instance, predictedDisplayTime, &sn, &loc))) {
                continue;
            }

            XrPosef pose = loc.globalLocation.pose;
            if (!isFiniteVec(pose.position) || !normalizeQuat(pose.orientation)) continue;

            auto& out = pkt.objects[validObjects];
            std::memcpy(out.serial, sn.value, kMaxSerialLen);
            out.serial[kMaxSerialLen - 1] = '\0';
            out.pos[0] = pose.position.x;
            out.pos[1] = pose.position.y;
            out.pos[2] = pose.position.z;
            out.quat[0] = pose.orientation.x;
            out.quat[1] = pose.orientation.y;
            out.quat[2] = pose.orientation.z;
            out.quat[3] = pose.orientation.w;
            if (isFiniteVec(loc.globalLocation.linearVelocity)) {
                out.linVel[0] = loc.globalLocation.linearVelocity.x;
                out.linVel[1] = loc.globalLocation.linearVelocity.y;
                out.linVel[2] = loc.globalLocation.linearVelocity.z;
            }
            if (isFiniteVec(loc.globalLocation.angularVelocity)) {
                out.angVel[0] = loc.globalLocation.angularVelocity.x;
                out.angVel[1] = loc.globalLocation.angularVelocity.y;
                out.angVel[2] = loc.globalLocation.angularVelocity.z;
            }
            out.confidence = loc.globalLocation.confidence;
            out.battery = gState.batteryPercent[i];
            out.valid = 1;
            ++validObjects;
        }
        if (validObjects > 0) {
            pkt.header.objectTrackerCount = validObjects;
            pkt.header.flags |= kFlagHasObjectTrackers;
            ++gState.framesObjectTracked;
        }
    }

    if (pkt.header.flags != 0 && gState.destValid) {
        gState.sessionEndingSent = false;
        sendPacketLocked(pkt);
        ++gState.framesSent;
    }

    if (nowMs - gState.lastHeartbeatLogMs >= 10000) {
        gState.lastHeartbeatLogMs = nowMs;
        LOGI("Heartbeat: dest=%s:%d mode=%u connected=%d hmd=%d joints=%zu objects=%u sent=%llu body=%llu obj=%llu",
             gState.destValid ? gState.activeHost.c_str() : "<discovering>",
             gState.destValid ? gState.activePort : gState.cfg.port,
             static_cast<unsigned>(effectiveMode),
             gState.connectState.trackerSum,
             (pkt.header.flags & kFlagHasHmdPose) ? 1 : 0,
             validJoints,
             static_cast<unsigned>(validObjects),
             static_cast<unsigned long long>(gState.framesSent),
             static_cast<unsigned long long>(gState.framesBodyTracked),
             static_cast<unsigned long long>(gState.framesObjectTracked));
    }
}

XrResult XRAPI_CALL Layer_xrDestroyInstance(XrInstance instance) {
    PFN_xrDestroyInstance next = nullptr;
    {
        std::lock_guard<std::mutex> lock(gState.mutex);
        if (instance == gState.instance) {
            next = gState.next.destroyInstance;
            resetInstanceLocked();
        }
    }
    return next ? next(instance) : XR_SUCCESS;
}

XrResult XRAPI_CALL Layer_xrCreateSession(XrInstance instance,
                                          const XrSessionCreateInfo* createInfo,
                                          XrSession* session) {
    PFN_xrCreateSession next = nullptr;
    PFN_xrGetSystemProperties getSysProps = nullptr;
    bool bodyExtEnabled = false;
    {
        std::lock_guard<std::mutex> lock(gState.mutex);
        next = gState.next.createSession;
        getSysProps = gState.next.getSystemProperties;
        bodyExtEnabled = gState.next.bodyTrackingExtEnabled;
    }
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;

    if (bodyExtEnabled && getSysProps && createInfo) {
        XrSystemBodyTrackingPropertiesBD btProps{XR_TYPE_SYSTEM_BODY_TRACKING_PROPERTIES_BD, nullptr, XR_FALSE};
        XrSystemProperties sysProps{};
        sysProps.type = XR_TYPE_SYSTEM_PROPERTIES;
        sysProps.next = &btProps;
        if (XR_SUCCEEDED(getSysProps(instance, createInfo->systemId, &sysProps))) {
            LOGI("System '%s': supportsBodyTracking=%u",
                 sysProps.systemName, static_cast<unsigned>(btProps.supportsBodyTracking));
        }
    }

    XrResult res = next(instance, createInfo, session);
    if (XR_SUCCEEDED(res) && session && *session != XR_NULL_HANDLE) {
        std::lock_guard<std::mutex> lock(gState.mutex);
        if (gState.session != XR_NULL_HANDLE && gState.session != *session) {
            destroySessionResourcesLocked();
        }
        gState.session = *session;
        gState.sessionState = XR_SESSION_STATE_IDLE;
        gState.sessionBegun = false;
        gState.sessionEndingSent = false;
        gState.lastTrackerAttemptMs = 0;
        gState.lastMetaPollMs = 0;
        gState.lastBatteryPollMs = 0;
        loadConfigFromEnvLocked(gState.cfg);
        loadConfigFileLocked(gState.cfg, false);
        LOGI("Captured XrSession=%p", reinterpret_cast<void*>(*session));
    }
    return res;
}

XrResult XRAPI_CALL Layer_xrDestroySession(XrSession session) {
    PFN_xrDestroySession next = nullptr;
    {
        std::lock_guard<std::mutex> lock(gState.mutex);
        next = gState.next.destroySession;
        if (session == gState.session) destroySessionResourcesLocked();
    }
    return next ? next(session) : XR_ERROR_FUNCTION_UNSUPPORTED;
}

XrResult XRAPI_CALL Layer_xrBeginSession(XrSession session, const XrSessionBeginInfo* beginInfo) {
    PFN_xrBeginSession next = nullptr;
    {
        std::lock_guard<std::mutex> lock(gState.mutex);
        next = gState.next.beginSession;
    }
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    XrResult res = next(session, beginInfo);
    if (XR_SUCCEEDED(res)) {
        std::lock_guard<std::mutex> lock(gState.mutex);
        if (session == gState.session) {
            gState.sessionBegun = true;
            gState.sessionEndingSent = false;
            gState.lastTrackerAttemptMs = 0;
        }
    }
    return res;
}

XrResult XRAPI_CALL Layer_xrEndSession(XrSession session) {
    PFN_xrEndSession next = nullptr;
    {
        std::lock_guard<std::mutex> lock(gState.mutex);
        next = gState.next.endSession;
        if (session == gState.session) {
            gState.sessionBegun = false;
            sendSessionEndingLocked();
            if (gState.bodyTracker != XR_NULL_HANDLE && gState.next.destroyBodyTrackerBD) {
                gState.next.destroyBodyTrackerBD(gState.bodyTracker);
                gState.bodyTracker = XR_NULL_HANDLE;
            }
        }
    }
    return next ? next(session) : XR_ERROR_FUNCTION_UNSUPPORTED;
}

XrResult XRAPI_CALL Layer_xrPollEvent(XrInstance instance, XrEventDataBuffer* eventData) {
    PFN_xrPollEvent next = nullptr;
    {
        std::lock_guard<std::mutex> lock(gState.mutex);
        next = gState.next.pollEvent;
    }
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;

    XrResult res = next(instance, eventData);
    if (res == XR_SUCCESS && eventData && eventData->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
        const auto* ev = reinterpret_cast<const XrEventDataSessionStateChanged*>(eventData);
        std::lock_guard<std::mutex> lock(gState.mutex);
        if (ev->session == gState.session) {
            gState.sessionState = ev->state;
            if (ev->state == XR_SESSION_STATE_STOPPING ||
                ev->state == XR_SESSION_STATE_EXITING ||
                ev->state == XR_SESSION_STATE_LOSS_PENDING) {
                sendSessionEndingLocked();
            }
        }
    }
    return res;
}

XrResult XRAPI_CALL Layer_xrCreateReferenceSpace(XrSession session,
                                                 const XrReferenceSpaceCreateInfo* createInfo,
                                                 XrSpace* space) {
    PFN_xrCreateReferenceSpace next = nullptr;
    {
        std::lock_guard<std::mutex> lock(gState.mutex);
        next = gState.next.createReferenceSpace;
    }
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;

    XrResult res = next(session, createInfo, space);
    if (XR_SUCCEEDED(res) && createInfo && space && *space != XR_NULL_HANDLE) {
        std::lock_guard<std::mutex> lock(gState.mutex);
        if (session == gState.session) {
            bool ident = isIdentityPose(createInfo->poseInReferenceSpace);
            if (createInfo->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_STAGE &&
                (gState.appStageSpace == XR_NULL_HANDLE || (ident && !gState.appStageSpaceIsIdentity))) {
                gState.appStageSpace = *space;
                gState.appStageSpaceIsIdentity = ident;
                LOGI("Captured app STAGE XrSpace=%p", reinterpret_cast<void*>(*space));
            } else if (createInfo->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL &&
                       (ident || gState.appLocalSpace == XR_NULL_HANDLE)) {
                gState.appLocalSpace = *space;
            } else if (createInfo->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_VIEW &&
                       (ident || gState.appViewSpace == XR_NULL_HANDLE)) {
                gState.appViewSpace = *space;
            }
        }
    }
    return res;
}

XrResult XRAPI_CALL Layer_xrDestroySpace(XrSpace space) {
    PFN_xrDestroySpace next = nullptr;
    {
        std::lock_guard<std::mutex> lock(gState.mutex);
        next = gState.next.destroySpace;
        if (space == gState.appStageSpace) {
            gState.appStageSpace = XR_NULL_HANDLE;
            gState.appStageSpaceIsIdentity = false;
        }
        if (space == gState.appLocalSpace) gState.appLocalSpace = XR_NULL_HANDLE;
        if (space == gState.appViewSpace)  gState.appViewSpace = XR_NULL_HANDLE;
    }
    return next ? next(space) : XR_ERROR_FUNCTION_UNSUPPORTED;
}

XrResult XRAPI_CALL Layer_xrWaitFrame(XrSession session,
                                      const XrFrameWaitInfo* frameWaitInfo,
                                      XrFrameState* frameState) {
    PFN_xrWaitFrame next = nullptr;
    {
        std::lock_guard<std::mutex> lock(gState.mutex);
        next = gState.next.waitFrame;
    }
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;

    XrResult res = next(session, frameWaitInfo, frameState);
    if (res == XR_SUCCESS && frameState && frameState->predictedDisplayTime > 0) {
        onFrameTick(session, frameState->predictedDisplayTime);
    }
    return res;
}

XrResult XRAPI_CALL Layer_xrGetInstanceProcAddr(XrInstance instance,
                                                const char* name,
                                                PFN_xrVoidFunction* function);

XrResult XRAPI_CALL Layer_xrCreateApiLayerInstance(const XrInstanceCreateInfo* info,
                                                   const XrApiLayerCreateInfo* apiLayerInfo,
                                                   XrInstance* instance) {
    if (!info || !apiLayerInfo || !instance || !apiLayerInfo->nextInfo) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    if (apiLayerInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO ||
        apiLayerInfo->structVersion != XR_API_LAYER_CREATE_INFO_STRUCT_VERSION ||
        apiLayerInfo->structSize != sizeof(XrApiLayerCreateInfo) ||
        apiLayerInfo->nextInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO ||
        apiLayerInfo->nextInfo->structVersion != XR_API_LAYER_NEXT_INFO_STRUCT_VERSION ||
        apiLayerInfo->nextInfo->structSize != sizeof(XrApiLayerNextInfo)) {
        LOGE("Incompatible OpenXR API layer create info struct");
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    PFN_xrGetInstanceProcAddr nextGipa = apiLayerInfo->nextInfo->nextGetInstanceProcAddr;
    PFN_xrCreateApiLayerInstance nextCreate = apiLayerInfo->nextInfo->nextCreateApiLayerInstance;
    if (!nextGipa || !nextCreate) return XR_ERROR_INITIALIZATION_FAILED;

    // Mirror libpico_openxr_loader.so so stock Khronos loaders can enable PICO vendor extensions.
    PFN_xrMarkApiClassPICO pfnMarkApiClass = nullptr;
    if (XR_SUCCEEDED(nextGipa(XR_NULL_HANDLE, "xrMarkApiClass",
                              reinterpret_cast<PFN_xrVoidFunction*>(&pfnMarkApiClass))) &&
        pfnMarkApiClass) {
        XrResult markRes = pfnMarkApiClass(kPicoApiClassVendorExtensions);
        LOGI("Enabled PICO vendor extension API classes (mask=0x%llx, res=%d)",
             static_cast<unsigned long long>(kPicoApiClassVendorExtensions), static_cast<int>(markRes));
    }

    bool hasExtBody = false;
    bool hasExtMotion = false;
    bool hasExtPicoBody = false;

    PFN_xrEnumerateInstanceExtensionProperties pfnEnumExt = nullptr;
    if (XR_SUCCEEDED(nextGipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
                              reinterpret_cast<PFN_xrVoidFunction*>(&pfnEnumExt))) &&
        pfnEnumExt) {
        uint32_t extCount = 0;
        if (XR_SUCCEEDED(pfnEnumExt(nullptr, 0, &extCount, nullptr)) && extCount > 0) {
            std::vector<XrExtensionProperties> exts(extCount);
            for (auto& e : exts) e.type = XR_TYPE_EXTENSION_PROPERTIES;
            if (XR_SUCCEEDED(pfnEnumExt(nullptr, extCount, &extCount, exts.data()))) {
                for (uint32_t i = 0; i < extCount; ++i) {
                    if (std::strcmp(exts[i].extensionName, XR_BD_BODY_TRACKING_EXTENSION_NAME) == 0) {
                        hasExtBody = true;
                    } else if (std::strcmp(exts[i].extensionName, kExtBdMotionTracking) == 0) {
                        hasExtMotion = true;
                    } else if (std::strcmp(exts[i].extensionName, kExtPicoBodyTracking) == 0) {
                        hasExtPicoBody = true;
                    }
                }
            }
        }
    }

    auto listContainsExt = [](const char* const* list, uint32_t count, const char* target) {
        for (uint32_t i = 0; i < count; ++i) {
            if (list[i] && std::strcmp(list[i], target) == 0) return true;
        }
        return false;
    };

    const bool origBody = listContainsExt(info->enabledExtensionNames, info->enabledExtensionCount, XR_BD_BODY_TRACKING_EXTENSION_NAME);
    const bool origMotion = listContainsExt(info->enabledExtensionNames, info->enabledExtensionCount, kExtBdMotionTracking);
    const bool origPicoBody = listContainsExt(info->enabledExtensionNames, info->enabledExtensionCount, kExtPicoBodyTracking);

    bool bodyEnabled = origBody;
    bool motionEnabled = origMotion;
    bool picoBodyEnabled = origPicoBody;

    const bool tryBody = hasExtBody && !origBody;
    const bool tryMotion = hasExtMotion && !origMotion;
    const bool tryPicoBody = hasExtPicoBody && !origPicoBody;

    XrApiLayerCreateInfo nextLayerInfo = *apiLayerInfo;
    nextLayerInfo.nextInfo = apiLayerInfo->nextInfo->next;

    std::vector<const char*> extNames;
    auto tryCreateWith = [&](bool addBody, bool addMotion, bool addPicoBody) -> XrResult {
        extNames.assign(info->enabledExtensionNames, info->enabledExtensionNames + info->enabledExtensionCount);
        bodyEnabled = origBody;
        motionEnabled = origMotion;
        picoBodyEnabled = origPicoBody;

        if (addBody && !bodyEnabled)         { extNames.push_back(XR_BD_BODY_TRACKING_EXTENSION_NAME); bodyEnabled = true; }
        if (addMotion && !motionEnabled)     { extNames.push_back(kExtBdMotionTracking); motionEnabled = true; }
        if (addPicoBody && !picoBodyEnabled) { extNames.push_back(kExtPicoBodyTracking); picoBodyEnabled = true; }

        XrInstanceCreateInfo modifiedInfo = *info;
        modifiedInfo.enabledExtensionCount = static_cast<uint32_t>(extNames.size());
        modifiedInfo.enabledExtensionNames = extNames.data();

        LOGI("Injecting PICO tracker extensions (body=%d motion=%d picoBody=%d)",
             bodyEnabled ? 1 : 0, motionEnabled ? 1 : 0, picoBodyEnabled ? 1 : 0);
        return nextCreate(&modifiedInfo, &nextLayerInfo, instance);
    };

    XrResult res = XR_ERROR_INITIALIZATION_FAILED;
    if (tryBody || tryMotion || tryPicoBody) {
        res = tryCreateWith(tryBody, tryMotion, tryPicoBody);
        if (XR_FAILED(res) && tryMotion && tryPicoBody) {
            LOGW("Instance creation with all 3 extensions failed (%d); retrying body+motion", static_cast<int>(res));
            res = tryCreateWith(tryBody, tryMotion, false);
        }
        if (XR_FAILED(res) && tryBody && (tryMotion || tryPicoBody)) {
            LOGW("Instance creation with motion/picoBody failed (%d); retrying body only", static_cast<int>(res));
            res = tryCreateWith(tryBody, false, false);
        }
        if (XR_FAILED(res)) {
            LOGW("Instance creation with body tracking failed (%d); retrying unmodified", static_cast<int>(res));
            res = nextCreate(info, &nextLayerInfo, instance);
            bodyEnabled = origBody;
            motionEnabled = origMotion;
            picoBodyEnabled = origPicoBody;
        }
    } else {
        if (!origBody && !origMotion && !origPicoBody) {
            LOGW("Runtime did not advertise PICO body/motion tracking extensions");
        }
        res = nextCreate(info, &nextLayerInfo, instance);
    }

    if (XR_FAILED(res)) return res;

    std::lock_guard<std::mutex> lock(gState.mutex);
    gState.instance = *instance;
    gState.next = DispatchTable{};
    gState.next.getInstanceProcAddr = nextGipa;
    gState.next.bodyTrackingExtEnabled = bodyEnabled;
    gState.next.motionTrackingExtEnabled = motionEnabled;
    gState.next.picoBodyTrackingExtEnabled = picoBodyEnabled;

    auto resolve = [&](const char* fnName, auto& outPtr) {
        PFN_xrVoidFunction fn = nullptr;
        outPtr = XR_SUCCEEDED(nextGipa(*instance, fnName, &fn))
            ? reinterpret_cast<std::decay_t<decltype(outPtr)>>(fn) : nullptr;
    };

    resolve("xrDestroyInstance", gState.next.destroyInstance);
    resolve("xrGetSystemProperties", gState.next.getSystemProperties);
    resolve("xrCreateSession", gState.next.createSession);
    resolve("xrDestroySession", gState.next.destroySession);
    resolve("xrBeginSession", gState.next.beginSession);
    resolve("xrEndSession", gState.next.endSession);
    resolve("xrPollEvent", gState.next.pollEvent);
    resolve("xrCreateReferenceSpace", gState.next.createReferenceSpace);
    resolve("xrDestroySpace", gState.next.destroySpace);
    resolve("xrLocateSpace", gState.next.locateSpace);
    resolve("xrWaitFrame", gState.next.waitFrame);

    if (bodyEnabled) {
        resolve("xrCreateBodyTrackerBD", gState.next.createBodyTrackerBD);
        resolve("xrDestroyBodyTrackerBD", gState.next.destroyBodyTrackerBD);
        resolve("xrLocateBodyJointsBD", gState.next.locateBodyJointsBD);
    }
    if (motionEnabled) {
        resolve("xrGetMotionTrackerConnectStateBD", gState.next.getMotionTrackerConnectStateBD);
        resolve("xrGetMotionTrackerModeBD", gState.next.getMotionTrackerModeBD);
        resolve("xrGetMotionTrackerLocationsBD", gState.next.getMotionTrackerLocationsBD);
    }
    if (picoBodyEnabled) {
        resolve("xrGetBodyTrackerBatteryPICO", gState.next.getBodyTrackerBatteryPICO);
    }

    LOGI("Resolved entry points: body=(%p,%p) motion=(%p,%p,%p) battery=%p",
         reinterpret_cast<void*>(gState.next.createBodyTrackerBD),
         reinterpret_cast<void*>(gState.next.locateBodyJointsBD),
         reinterpret_cast<void*>(gState.next.getMotionTrackerConnectStateBD),
         reinterpret_cast<void*>(gState.next.getMotionTrackerModeBD),
         reinterpret_cast<void*>(gState.next.getMotionTrackerLocationsBD),
         reinterpret_cast<void*>(gState.next.getBodyTrackerBatteryPICO));

    loadConfigFromEnvLocked(gState.cfg);
    loadConfigFileLocked(gState.cfg, true);

    return XR_SUCCESS;
}

struct HookEntry {
    const char* name;
    PFN_xrVoidFunction fn;
};

XrResult XRAPI_CALL Layer_xrGetInstanceProcAddr(XrInstance instance,
                                                const char* name,
                                                PFN_xrVoidFunction* function) {
    if (!name || !function) return XR_ERROR_HANDLE_INVALID;
    *function = nullptr;

    if (std::strcmp(name, "xrGetInstanceProcAddr") == 0) {
        *function = reinterpret_cast<PFN_xrVoidFunction>(Layer_xrGetInstanceProcAddr);
        return XR_SUCCESS;
    }
    if (std::strcmp(name, "xrCreateApiLayerInstance") == 0) {
        *function = reinterpret_cast<PFN_xrVoidFunction>(Layer_xrCreateApiLayerInstance);
        return XR_SUCCESS;
    }
    if (instance == XR_NULL_HANDLE) return XR_ERROR_FUNCTION_UNSUPPORTED;

    static const HookEntry kInstanceHooks[] = {
        {"xrDestroyInstance", reinterpret_cast<PFN_xrVoidFunction>(Layer_xrDestroyInstance)},
        {"xrCreateSession", reinterpret_cast<PFN_xrVoidFunction>(Layer_xrCreateSession)},
        {"xrDestroySession", reinterpret_cast<PFN_xrVoidFunction>(Layer_xrDestroySession)},
        {"xrBeginSession", reinterpret_cast<PFN_xrVoidFunction>(Layer_xrBeginSession)},
        {"xrEndSession", reinterpret_cast<PFN_xrVoidFunction>(Layer_xrEndSession)},
        {"xrPollEvent", reinterpret_cast<PFN_xrVoidFunction>(Layer_xrPollEvent)},
        {"xrCreateReferenceSpace", reinterpret_cast<PFN_xrVoidFunction>(Layer_xrCreateReferenceSpace)},
        {"xrDestroySpace", reinterpret_cast<PFN_xrVoidFunction>(Layer_xrDestroySpace)},
        {"xrWaitFrame", reinterpret_cast<PFN_xrVoidFunction>(Layer_xrWaitFrame)},
    };
    for (const auto& entry : kInstanceHooks) {
        if (std::strcmp(name, entry.name) == 0) {
            *function = entry.fn;
            return XR_SUCCESS;
        }
    }

    PFN_xrGetInstanceProcAddr nextGipa = nullptr;
    {
        std::lock_guard<std::mutex> lock(gState.mutex);
        if (instance == gState.instance) nextGipa = gState.next.getInstanceProcAddr;
    }
    return nextGipa ? nextGipa(instance, name, function) : XR_ERROR_HANDLE_INVALID;
}

} // namespace

extern "C" __attribute__((visibility("default")))
XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo* loaderInfo,
                                                       const char* layerName,
                                                       XrNegotiateApiLayerRequest* apiLayerRequest) {
    if (!loaderInfo || !apiLayerRequest) return XR_ERROR_INITIALIZATION_FAILED;
    if (loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
        loaderInfo->structSize != sizeof(XrNegotiateLoaderInfo) ||
        apiLayerRequest->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        apiLayerRequest->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION ||
        apiLayerRequest->structSize != sizeof(XrNegotiateApiLayerRequest)) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    if (layerName && std::strcmp(layerName, kLayerName) != 0) {
        LOGW("Unexpected layer name '%s' (expected '%s')", layerName, kLayerName);
    }
    if (loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    apiLayerRequest->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    apiLayerRequest->layerApiVersion = XR_API_VERSION_1_0;
    apiLayerRequest->getInstanceProcAddr = Layer_xrGetInstanceProcAddr;
    apiLayerRequest->createApiLayerInstance = Layer_xrCreateApiLayerInstance;

    LOGI("Negotiated OpenXR API layer %s", kLayerName);
    return XR_SUCCESS;
}
