#ifdef NDEBUG
#undef NDEBUG
#endif

#include "app/Config.h"
#include "app/UiModel.h"

#include <cassert>
#include <string>

int main()
{
    using namespace od;

    assert(ConnectionPhaseTitle(ConnectionPhase::WaitingHello) == L"等待 OpenDisplay");
    assert(ConnectionPhaseTitle(ConnectionPhase::UnsafeNetwork) == L"已阻止连接");
    assert(FailureHelpText(FailureReason::UsbUnavailable).find(L"解锁") != std::wstring::npos);
    assert(FailureHelpText(FailureReason::DriverUnavailable).find(L"Parsec") != std::wstring::npos);

    assert(InferStreamProfile(60, 30) == StreamProfile::Balanced);
    assert(InferStreamProfile(60, 15) == StreamProfile::LowBandwidth);
    assert(InferStreamProfile(60, 45) == StreamProfile::Sharp);
    assert(InferStreamProfile(120, 30) == StreamProfile::Custom);

    const auto devices = ParseDeviceEntriesText(
        L"  192.168.1.42 iPad Pro\r\n192.168.1.42 重复名称\n10.0.0.8 Mini  \n\n");
    assert(devices.size() == 2);
    assert(devices[0].lastIpv4 == "192.168.1.42");
    assert(devices[0].name == "iPad Pro");
    assert(devices[1].lastIpv4 == "10.0.0.8");
    assert(devices[1].name == "Mini");

    DeviceConfig baselineDevice;
    baselineDevice.id = "stable-device";
    baselineDevice.name = "Study iPad";
    baselineDevice.lastIpv4 = "192.168.1.42";
    baselineDevice.bonjourHost = "study.local";
    baselineDevice.autoConnect = false;
    DeviceConfig currentDevice = baselineDevice;
    currentDevice.lastIpv4 = "192.168.1.77";
    currentDevice.name = "Current advertised name";
    currentDevice.lastSeen = 12345;
    currentDevice.macMatchingEnabled = false;
    currentDevice.macBindings.push_back({"02:11:22:33:44:55", "home/adapter"});
    const std::wstring baselineText = L"192.168.1.42 Study iPad\r\n";
    const auto untouched = MergeEditedDeviceEntries({currentDevice}, {baselineDevice}, baselineText, baselineText, 9001);
    assert(untouched && untouched->size() == 1);
    DeviceConfig expectedCurrent = currentDevice;
    expectedCurrent.port = 9001;
    assert(untouched->front() == expectedCurrent); // changing only quality never restores the stale IP

    const auto renamed = MergeEditedDeviceEntries({currentDevice}, {baselineDevice}, baselineText,
                                                  L"192.168.1.42 New name", 9002);
    assert(renamed && renamed->size() == 1);
    expectedCurrent.name = "New name";
    expectedCurrent.port = 9002;
    assert(renamed->front() == expectedCurrent); // old displayed IP maps by baseline ID to latest metadata
    const auto reformatted = MergeEditedDeviceEntries({currentDevice}, {baselineDevice}, baselineText,
                                                       L" 192.168.1.42 Study iPad  \n", 9000);
    assert(reformatted && reformatted->front().name == currentDevice.name);
    assert(reformatted->front().lastIpv4 == currentDevice.lastIpv4);
    const auto explicitAddress = MergeEditedDeviceEntries({currentDevice}, {baselineDevice}, baselineText,
                                                          L"192.168.1.99 Study iPad", 9000);
    assert(explicitAddress && explicitAddress->front().id.empty()); // even the same name cannot transfer identity
    assert(explicitAddress->front().macBindings.empty());
    const auto removedName = MergeEditedDeviceEntries({currentDevice}, {baselineDevice}, baselineText,
                                                      L"192.168.1.42", 9000);
    assert(removedName && removedName->front().name.empty());
    assert(removedName->front().lastIpv4 == "192.168.1.77");
    const auto removedAll = MergeEditedDeviceEntries({currentDevice}, {baselineDevice}, baselineText, L"", 9000);
    assert(removedAll && removedAll->empty());
    assert(!MergeEditedDeviceEntries({}, {baselineDevice}, baselineText, L"192.168.1.42 Renamed", 9000));
    assert(!MergeEditedDeviceEntries({currentDevice, currentDevice}, {baselineDevice}, baselineText,
                                    L"192.168.1.42 Renamed", 9000));
    assert(!MergeEditedDeviceEntries({currentDevice}, {baselineDevice}, baselineText,
                                    L"192.168.1.42 Study iPad\n192.168.1.77 New device", 9000));
    DeviceConfig unboundBaseline = baselineDevice;
    unboundBaseline.id.clear();
    assert(!MergeEditedDeviceEntries({currentDevice}, {unboundBaseline}, baselineText,
                                    L"192.168.1.42 Renamed", 9000)); // cannot guess an initially unbound row after DHCP
    DeviceConfig learnedAtSameAddress = currentDevice;
    learnedAtSameAddress.lastIpv4 = baselineDevice.lastIpv4;
    const auto newlyLearned = MergeEditedDeviceEntries({learnedAtSameAddress}, {unboundBaseline}, baselineText,
                                                       L"192.168.1.42 Renamed", 9000);
    assert(newlyLearned && newlyLearned->front().id == "stable-device");
    const auto pendingBaseline = MergeEditedDeviceEntries({currentDevice}, {unboundBaseline}, baselineText, baselineText, 9000);
    assert(pendingBaseline && pendingBaseline->front().id == "stable-device");
    assert(!MergeEditedDeviceEntries({currentDevice}, {baselineDevice}, baselineText, baselineText, 0));

    const std::string redacted = RedactDiagnosticsText(
        "C:\\Users\\Tester\\AppData [192.168.1.42] usbmux://00008030-ABCDEF end",
        "C:\\Users\\Tester");
    assert(redacted.find("Tester") == std::string::npos);
    assert(redacted.find("192.168.1.42") == std::string::npos);
    assert(redacted.find("00008030-ABCDEF") == std::string::npos);
    assert(redacted.find("<profile>") != std::string::npos);
    assert(redacted.find("<ip>") != std::string::npos);
    assert(redacted.find("usbmux://<device>") != std::string::npos);
    const std::string networkRedacted = RedactDiagnosticsText(
        "MAC=02:ab:cd:ef:12:34 other=02-11-22-33-44-55 scope={01234567-89ab-cdef-0123-456789abcdef} 19:31:01 fps=60");
    assert(networkRedacted.find("02:ab:cd:ef:12:34") == std::string::npos);
    assert(networkRedacted.find("02-11-22-33-44-55") == std::string::npos);
    assert(networkRedacted.find("01234567-89ab-cdef-0123-456789abcdef") == std::string::npos);
    assert(networkRedacted.find("<mac>") != std::string::npos);
    assert(networkRedacted.find("<id>") != std::string::npos);
    assert(networkRedacted.find("19:31:01 fps=60") != std::string::npos);

    const Config legacySingle = Config::Parse(
        R"({"ip":"192.168.1.42 iPad Pro","port":9000,"autoReconnect":false,"fps":60,"bitrateMbps":30})");
    assert(legacySingle.devices.size() == 1);
    assert(legacySingle.devices[0].lastIpv4 == "192.168.1.42");
    assert(legacySingle.devices[0].name == "iPad Pro");
    assert(!legacySingle.autoReconnect);
    assert(legacySingle.streamProfile == StreamProfile::Balanced);

    const Config release011 = Config::Parse(
        R"({"devices":["192.168.1.42"],"port":9000,"autoReconnect":true,"fps":60,"bitrateMbps":80,"lastConnectionTransport":"usb","lastWifiAddress":"192.168.1.42","lastUsbTarget":"usbmux://secret"})");
    assert(release011.streamProfile == StreamProfile::Custom);
    assert(release011.lastConnectionTransport == "usb");
    assert(release011.lastWifiAddress == "192.168.1.42");
    assert(release011.lastUsbTarget == "usbmux://secret");

    const Config release012 = Config::Parse(
        R"({"devices":[],"fps":60,"bitrateMbps":45,"streamProfile":2,"launcherMode":2,"darkTheme":true})");
    assert(release012.streamProfile == StreamProfile::Sharp);
    assert(release012.launcherMode == LauncherMode::Hidden);
    assert(release012.darkTheme);

    return 0;
}
