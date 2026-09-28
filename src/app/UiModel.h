#pragma once

#include "app/Config.h"
#include "app/SenderApp.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace od {

std::wstring ConnectionPhaseTitle(ConnectionPhase phase);
std::wstring FailureHelpText(FailureReason failure, const std::string& detail = {});
StreamProfile InferStreamProfile(uint32_t fps, uint32_t bitrateMbps);
std::vector<DeviceConfig> ParseDeviceEntriesText(const std::wstring& text, uint16_t defaultPort = 9000);
std::wstring DeviceEntryText(const DeviceConfig& device);

// The edit box may still show an old DHCP address. Unedited text keeps current
// records; edited old endpoints resolve through the displayed baseline's ID,
// retaining the latest address/bindings. An explicitly new endpoint is unbound.
// nullopt means an old row can no longer be mapped uniquely; callers must refresh
// the edit baseline rather than silently dropping its identity protection.
std::optional<std::vector<DeviceConfig>> MergeEditedDeviceEntries(
    const std::vector<DeviceConfig>& currentDevices, const std::vector<DeviceConfig>& baselineDevices,
    const std::wstring& baselineText, const std::wstring& editedText, uint16_t port);

// Removes local profile paths, IPv4/MAC addresses, UUIDs and USB identifiers from
// a report before it is written to a user-shareable diagnostics file.
std::string RedactDiagnosticsText(std::string text, const std::string& userProfile = {});

} // namespace od
