#ifdef NDEBUG
#undef NDEBUG
#endif

#include "app/Config.h"
#include "app/DeviceCoordinator.h"
#include "net/Mdns.h"
#include "net/NetworkSafety.h"
#include "net/NeighborDiscovery.h"

#include <winsock2.h>
#include <windows.h>

#include <cassert>
#include <chrono>
#include <fstream>

int main()
{
    using namespace od;

    WSADATA wsa{};
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    const NetworkSafetyResult localhost = CheckTrustedLan("localhost", true);
    assert(localhost.allowed);
    assert(!localhost.resolvedTarget.empty());

    const Config migrated = Config::Parse(R"({"devices":["192.168.1.42 iPad Pro","10.0.0.8 Mini"],"port":9001})");
    assert(migrated.version == Config::CurrentVersion);
    assert(migrated.devices.size() == 2);
    assert(migrated.devices[0].lastIpv4 == "192.168.1.42");
    assert(migrated.devices[0].name == "iPad Pro");
    assert(migrated.devices[0].port == 9001);
    assert(!migrated.taskbarRouting); // v3 migrates to the native Windows desktop
    assert(!migrated.showLauncher);
    assert(migrated.launcherMode == LauncherMode::Hidden);
    assert(!Config{}.taskbarRouting); // new configuration is opt-in
    assert(!Config{}.showLauncher);
    const Config legacyOverlay = Config::Parse(
        R"({"version":2,"showLauncher":true,"launcherMode":2,"taskbarRouting":true,"devices":[{"id":"known","lastIpv4":"10.0.0.8","macHint":"02:11:22:33:44:55","macBindings":[{"mac":"02:11:22:33:44:55","networkScope":"old"}]}]})");
    assert(!legacyOverlay.showLauncher && legacyOverlay.launcherMode == LauncherMode::Hidden && !legacyOverlay.taskbarRouting);
    assert(legacyOverlay.devices[0].macHint == "02:11:22:33:44:55");
    assert(legacyOverlay.devices[0].macBindings.empty());
    const Config duplicateIds = Config::Parse(
        R"({"version":2,"devices":[{"id":"same","lastIpv4":"10.0.0.1"},{"id":"same","lastIpv4":"10.0.0.2"}]})");
    assert(duplicateIds.devices.size() == 1);

    Config special;
    DeviceConfig quoted;
    quoted.id = "install-\"id\\path";
    quoted.name = "书房 \"iPad\"\n第二行";
    quoted.lastIpv4 = "192.168.1.9";
    quoted.bonjourHost = "Li\\iPad.local";
    quoted.macHint = "diagnostic-only";
    quoted.macBindings.push_back({"02:11:22:33:44:55", "network/adapter"});
    special.devices.push_back(quoted);
    special.preferredDeviceId = quoted.id;
    const Config roundTrip = Config::Parse(special.Serialize());
    assert(roundTrip.devices == special.devices);
    assert(roundTrip.preferredDeviceId == special.preferredDeviceId);

    wchar_t tempBase[MAX_PATH]{};
    assert(GetTempPathW(MAX_PATH, tempBase) > 0);
    const std::wstring tempDir = std::wstring(tempBase) + L"ipad-connect-config-test-" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(tempDir.c_str(), nullptr);
    const std::wstring configPath = tempDir + L"\\config.json";
    Config first = special;
    first.fps = 60;
    assert(first.SaveToFile(configPath));
    Config second = special;
    second.fps = 90;
    assert(second.SaveToFile(configPath));
    Config third = special;
    third.fps = 120;
    assert(third.SaveToFile(configPath));
    {
        std::ofstream corrupt(configPath, std::ios::binary | std::ios::trunc);
        corrupt << "{broken";
    }
    const Config recovered = Config::LoadFromFile(configPath);
    assert(recovered.fps == 90); // previous atomic target recovered from .bak
    const std::wstring missingParent = tempDir + L"\\missing\\config.json";
    assert(!second.SaveToFile(missingParent));

    wchar_t* oldOverride = nullptr;
    size_t oldOverrideLength = 0;
    _wdupenv_s(&oldOverride, &oldOverrideLength, L"IPAD_CONNECT_CONFIG_DIR");
    assert(_wputenv_s(L"IPAD_CONNECT_CONFIG_DIR", tempDir.c_str()) == 0);
    assert(Config::FilePath() == configPath);
    assert(Config::Load().devices == special.devices); // isolated path and .bak recovery
    const std::wstring emptyOverride = tempDir + L"\\empty-isolated-directory";
    assert(_wputenv_s(L"IPAD_CONNECT_CONFIG_DIR", emptyOverride.c_str()) == 0);
    assert(Config::Load().devices.empty()); // never migrate a real legacy config
    assert(_wputenv_s(L"IPAD_CONNECT_CONFIG_DIR", L"relative-preview") == 0);
    assert(Config::FilePath().find(L"relative-preview") == std::wstring::npos);
    assert(_wputenv_s(L"IPAD_CONNECT_CONFIG_DIR", oldOverride ? oldOverride : L"") == 0);
    free(oldOverride);

    DiscoveryCache cache;
    const auto now = std::chrono::steady_clock::now();
    DiscoveryRecord firstAddress;
    firstAddress.id = "stable-id";
    firstAddress.name = "iPad";
    firstAddress.instance = "iPad";
    firstAddress.address = "192.168.1.10";
    firstAddress.addresses = {firstAddress.address};
    firstAddress.host = "ipad.local";
    firstAddress.port = 9000;
    firstAddress.ttlSeconds = 2;
    cache.Merge({firstAddress}, now);
    assert(cache.FindById("stable-id")->online);
    assert(cache.FindById("stable-id")->address == "192.168.1.10");

    DiscoveryRecord changed = firstAddress;
    changed.address = "192.168.1.77";
    changed.addresses = {changed.address};
    cache.Merge({changed}, now + std::chrono::seconds(1));
    auto deduplicated = cache.FindById("stable-id");
    assert(cache.Records().size() == 1);
    assert(deduplicated->address == "192.168.1.77");
    assert(deduplicated->addresses.size() == 2);
    cache.Expire(now + std::chrono::seconds(4));
    assert(!cache.FindById("stable-id")->online);

    DiscoveryRecord goodbye;
    goodbye.instance = "iPad"; // a real goodbye may carry only the PTR name
    goodbye.goodbye = true;
    goodbye.ttlSeconds = 0;
    cache.Merge({goodbye}, now + std::chrono::seconds(5));
    assert(!cache.FindById("stable-id")->online);
    assert(cache.FindById("stable-id")->goodbye);

    assert(NormalizeMacAddress("02-ab-cd-ef-12-34") == "02:AB:CD:EF:12:34");
    for (const std::string invalid : {"", "garbage", "00:00:00:00:00:00", "FF:FF:FF:FF:FF:FF", "01:11:22:33:44:55", "02:11-22:33:44:55"})
        assert(NormalizeMacAddress(invalid).empty());

    DeviceConfig candidateDevice;
    candidateDevice.id = "stable-id";
    candidateDevice.lastIpv4 = "192.168.1.10";
    candidateDevice.bonjourHost = "ipad.local";
    candidateDevice.macBindings = {{"02:11:22:33:44:55", "home/adapter"}};
    const NeighborRecord currentNeighbor{"192.168.1.88", "02:11:22:33:44:55", "home/adapter", 7, true};
    DiscoveryRecord online = firstAddress;
    online.address = "192.168.1.77";
    online.addresses = {online.address, "192.168.1.78"};
    assert(BuildDeviceCandidates(candidateDevice, {online}, {currentNeighbor}) ==
           (std::vector<std::string>{"192.168.1.77", "192.168.1.78", "192.168.1.88", "ipad.local", "192.168.1.10"}));
    online.online = false;
    assert(BuildDeviceCandidates(candidateDevice, {online}, {}) ==
           (std::vector<std::string>{"ipad.local", "192.168.1.10"}));
    NeighborRecord wrongNetwork = currentNeighbor;
    wrongNetwork.networkScope = "cafe/adapter"; // same interface/MAC on another network is not a match
    assert(BuildDeviceCandidates(candidateDevice, {}, {wrongNetwork}).size() == 2);
    DeviceConfig noIdentity = candidateDevice;
    noIdentity.id.clear();
    assert(BuildDeviceCandidates(noIdentity, {}, {currentNeighbor}).size() == 2);
    noIdentity.id = "stable-id";
    noIdentity.macMatchingEnabled = false;
    assert(BuildDeviceCandidates(noIdentity, {}, {currentNeighbor}).size() == 2);
    NeighborRecord duplicateMac = currentNeighbor;
    duplicateMac.address = "192.168.1.89";
    assert(BuildDeviceCandidates(candidateDevice, {}, {currentNeighbor, duplicateMac}).size() == 2);
    NeighborRecord reusedAddress = currentNeighbor;
    reusedAddress.mac = "02:AA:BB:CC:DD:EE";
    assert(BuildDeviceCandidates(candidateDevice, {}, {currentNeighbor, reusedAddress}).size() == 2);
    assert(!LearnVerifiedMacBinding(candidateDevice, currentNeighbor.address, "wrong-id", {currentNeighbor}));
    assert(!LearnVerifiedMacBinding(candidateDevice, currentNeighbor.address, "stable-id", {currentNeighbor, duplicateMac}));
    NeighborRecord stale = reusedAddress;
    stale.reachable = false;
    assert(!LearnVerifiedMacBinding(candidateDevice, stale.address, "stable-id", {stale}));
    assert(LearnVerifiedMacBinding(candidateDevice, reusedAddress.address, "stable-id", {reusedAddress}));
    assert(candidateDevice.macBindings.size() == 1);
    assert(candidateDevice.macBindings[0].mac == reusedAddress.mac); // Apple private MAC rotation replaces this network only
    assert(!LearnVerifiedMacBinding(candidateDevice, reusedAddress.address, "stable-id", {reusedAddress}));
    assert(LearnVerifiedMacBinding(candidateDevice, wrongNetwork.address, "stable-id", {wrongNetwork}));
    assert(candidateDevice.macBindings.size() == 2);
    candidateDevice.macBindings.push_back({"02:77:66:55:44:33", "home/adapter"});
    assert(BuildDeviceCandidates(candidateDevice, {}, {reusedAddress}).size() == 2); // conflicting persisted binding ignored
    assert(LearnVerifiedMacBinding(candidateDevice, reusedAddress.address, "stable-id", {reusedAddress}));
    assert(candidateDevice.macBindings.size() == 2); // successful identity observation repairs duplicate scope
    online.online = true;
    online.addresses.clear();
    for (int n = 1; n <= 20; ++n) online.addresses.push_back("192.168.2." + std::to_string(n));
    assert(BuildDeviceCandidates(candidateDevice, {online}, {}).size() == 8);

    std::vector<DeviceConfig> devices(3);
    devices[0].id = "first";
    devices[1].id = "second";
    devices[2].id = "third";
    DeviceCoordinator flow;
    assert(flow.Begin(devices, "second") == 1);
    assert(flow.Order() == std::vector<size_t>({1, 0, 2}));
    assert(flow.ReportFailure(DeviceFailureClass::Device) == 0);
    assert(flow.State() == AutoConnectState::WaitingNext);
    assert(flow.ReportFailure(DeviceFailureClass::Device) == 2);
    assert(!flow.ReportFailure(DeviceFailureClass::Device));
    assert(flow.State() == AutoConnectState::Exhausted);

    assert(flow.Begin(devices, {}) == 0);
    assert(!flow.ReportFailure(DeviceFailureClass::Global));
    assert(flow.State() == AutoConnectState::Blocked);
    assert(flow.RequestSwitch(2));
    assert(!flow.RequestSwitch(2)); // duplicate click cannot create a second target
    flow.CompleteSwitch(false);
    assert(flow.State() == AutoConnectState::Trying);
    flow.ReportStreaming("third");
    assert(flow.State() == AutoConnectState::Streaming);
    assert(flow.StreamingDeviceId() == "third");

    DeleteFileW((configPath + L".bak").c_str());
    DeleteFileW(configPath.c_str());
    RemoveDirectoryW(tempDir.c_str());
    WSACleanup();
    return 0;
}
