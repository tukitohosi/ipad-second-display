#pragma once

#include <atomic>
#include <string>

namespace od {

struct NetworkSafetyResult {
    bool allowed = false;
    std::string reason;
    // Numeric endpoint produced by the same resolution that passed policy.
    // Callers use this value for connect, avoiding a second DNS resolution.
    std::string resolvedTarget;
    // Resolution failure/timeout/cancellation is local to one endpoint, not
    // evidence that the user's whole network violates the trusted-LAN policy.
    bool resolutionFailed = false;
};

// Enforces the product's trusted-LAN boundary before a screen frame can leave
// the PC. Loopback is always accepted for the bundled mock receiver. Remote
// targets must resolve to a private/link-local IPv4 address and Windows must
// report at least one connected Private or Domain network.
// Hostname resolution is limited to at most 2000 ms and can be cancelled. A
// numeric address takes the local parsing path without a DNS query.
NetworkSafetyResult CheckTrustedLan(const std::string& target, bool requirePrivateNetwork,
                                    const std::atomic<bool>* stopRequested = nullptr, int timeoutMs = 2000);

} // namespace od
