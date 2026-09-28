#pragma once

#include "app/Config.h"
#include "net/Mdns.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace od {

struct NeighborRecord {
    std::string address;
    std::string mac;
    std::string networkScope;
    uint32_t interfaceIndex = 0;
    bool reachable = false;
};

struct NeighborSnapshot {
    std::vector<NeighborRecord> records;
    std::string status;
};

// Canonical six-byte unicast MAC, or empty for malformed/broadcast/multicast/zero.
std::string NormalizeMacAddress(std::string_view mac);

// Call on a worker. Reads the current neighbor cache and network profiles. If
// requested, refreshes at most four explicit numeric, on-link targets; no scan.
NeighborSnapshot ReadNeighborSnapshot(const std::vector<std::string>& knownTargets = {},
                                      bool resolveMissing = false);

// Pure selection: online matching Bonjour -> verified scoped MAC -> host -> IP.
// Bounded to eight endpoints. MAC never contributes without an expected ID.
std::vector<std::string> BuildDeviceCandidates(const DeviceConfig& device,
                                             const std::vector<DiscoveryRecord>& discovered,
                                             const std::vector<NeighborRecord>& neighbors);

// Caller must supply the actual connected endpoint and call only after Streaming.
// Requires the already-learned expected ID to match the successful hello exactly.
// Ambiguous observations are ignored. Returns true only when bindings changed.
bool LearnVerifiedMacBinding(DeviceConfig& device, const std::string& connectedAddress,
                             const std::string& helloId,
                             const std::vector<NeighborRecord>& neighbors);

} // namespace od
