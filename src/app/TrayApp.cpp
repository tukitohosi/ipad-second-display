#include "app/TrayApp.h"

#include "app/Config.h"
#include "app/PanelLayout.h"
#include "app/PanelScroll.h"
#include "app/DeviceCoordinator.h"
#include "app/Log.h"
#include "app/SenderApp.h"

#include "app/UiModel.h"
#include "app/resources.h"
#include "display/DisplayCatalog.h"
#include "net/Mdns.h"
#include "net/NeighborDiscovery.h"
#include "net/NetworkSafety.h"
#include "net/UsbMux.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <ws2tcpip.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "shell32.lib")

namespace od {

namespace {

constexpr UINT WM_APP_TRAY = WM_APP + 1;
constexpr UINT kStatusTimerId = 1;
constexpr UINT kStatusTimerMs = 1000;
UINT gTaskbarCreated = 0;
UINT gShowControlPanel = 0;

enum : UINT {
    IDM_CONNECT = 40001,
    IDM_DISCONNECT,
    IDM_SETTINGS,
    IDM_RUNASADMIN,
    IDM_EXIT,
    IDM_DISCONNECT_USB,
    IDM_OPEN_CONTROL_PANEL,

    // One command per configured iPad, IDM_DEVICE_FIRST + index. Toggles that
    // device's sender on its own, so the others keep streaming.
    IDM_DEVICE_FIRST = 41000,
    IDM_USB_DEVICE_FIRST = 42000,
};

const wchar_t* const kWndClass = L"MouseLinkTrayWindow";
const wchar_t* const kPreviewWndClass = L"IpadConnectPreviewTrayWindow";
enum : int {
    IDC_PANEL_THEME = 1301, IDC_PANEL_PRIMARY, IDC_PANEL_SECONDARY, IDC_PANEL_DISPLAY_SETTINGS,
    IDC_PANEL_PLACE_RIGHT, IDC_PANEL_DIAGNOSTICS, IDC_PANEL_ADVANCED, IDC_PANEL_ADD_DISCOVERED,
    IDC_PANEL_AUTO_RECONNECT, IDC_PANEL_PRIVATE_NETWORK, IDC_PANEL_SAVE, IDC_PANEL_PROFILE_BALANCED,
    IDC_PANEL_PROFILE_LOW, IDC_PANEL_PROFILE_SHARP, IDC_PANEL_DEVICE_ADDRESS, IDC_PANEL_DEVICE_MAC,
    IDC_PANEL_DISCOVERY_ADDRESS,
};

enum class ConnectionTransport {
    None,
    Wifi,
    Usb,
};

struct StartupConnectionAttempt {
    ConnectionTransport transport = ConnectionTransport::None;
    bool fallbackUsed = false;
    std::chrono::steady_clock::time_point started{};
    std::vector<size_t> wifiOrder;
    size_t wifiPosition = 0;
    size_t activeDeviceIndex = static_cast<size_t>(-1);
    bool waitingNext = false;
    bool waitingTransportStop = false;
};

constexpr auto kStartupConnectionAttemptTimeout = std::chrono::seconds(8);

struct TrayContext {
    HINSTANCE hInstance = nullptr;
    HWND trayWindow = nullptr;
    HWND controlPanel = nullptr;
    // One sender per configured iPad, index-aligned with cfg.devices: each
    // drives its own virtual monitor, capture, encoder and input injection.
    // unique_ptr because SenderApp owns a thread and can't be moved.
    std::vector<std::unique_ptr<SenderApp>> apps;
    std::vector<std::unique_ptr<SenderApp>> retiredApps;
    Config cfg;
    NOTIFYICONDATAW nid{};
    bool trayAdded = false;
    HICON iconGreen = nullptr; // at least one iPad streaming
    HICON iconRed = nullptr;   // configured but nothing streaming
    HICON iconGrey = nullptr;  // no iPad configured

    // Names the iPads advertise over Bonjour, keyed by address — filled by a
    // background browse, because a lookup takes a moment and the menu must not
    // wait for it.
    mutable std::mutex discoveredMutex;
    std::map<std::string, std::string> discovered;
    DiscoveryCache discoveryCache;
    NeighborSnapshot neighbors;
    std::vector<std::string> neighborTargets;
    bool discoveryCompleted = false;
    std::thread discovery;
    std::atomic<bool> discoveryRunning{false};

    // USBMux is local and cheap, but probing it on every paint would still be
    // needless. A cable session is intentionally runtime-only: the same iPad
    // must never auto-start through both its Wi-Fi address and USB selector.
    std::vector<UsbMuxDevice> usbDevices;
    std::chrono::steady_clock::time_point lastUsbRefresh{};
    std::unique_ptr<SenderApp> usbApp;
    std::string activeUsbTarget;

    // The connection settings are intentionally native child edits inside the
    // otherwise custom-drawn control panel.  It keeps keyboard, selection and
    // IME behavior familiar while the surrounding UI remains consistent.
    HWND panelDevices = nullptr;
    HWND panelPort = nullptr;
    HWND panelFps = nullptr;
    HWND panelBitrate = nullptr;
    HWND panelDiscovery = nullptr;
    HWND panelDeviceSelector = nullptr;
    HWND panelDeviceConnect = nullptr;
    HWND panelDeviceDefault = nullptr;
    HWND panelDeviceUp = nullptr;
    HWND panelDeviceRemove = nullptr;
    HWND panelDeviceAddress = nullptr;
    HWND panelDeviceMac = nullptr;
    HWND panelDiscoveryAddress = nullptr;
    HFONT panelFont = nullptr;
    std::vector<std::pair<int, HWND>> panelButtons;
    std::vector<std::string> panelDiscoveryKeys;
    std::vector<std::wstring> panelDiscoveryRows;
    std::vector<std::string> panelDeviceKeys;
    bool panelDiscoveryInitialized = false;
    bool preview = false;
    bool previewEmpty = false;
    bool previewLong = false;
    unsigned previewDpi = 0;
    std::vector<std::string> panelDiscoveryAddresses;
    std::vector<std::wstring> panelDeviceRows;
    std::vector<DeviceConfig> panelDeviceBaseline;
    std::wstring panelDeviceTextBaseline;
    bool panelAutoReconnect = false;
    bool panelRequirePrivateNetwork = true;
    StreamProfile panelStreamProfile = StreamProfile::Balanced;
    bool panelAdvancedSettings = false;
    HBRUSH panelEditBrush = nullptr;
    std::wstring panelNotice;
    std::chrono::steady_clock::time_point panelNoticeUntil{};
    bool exitWhenControlPanelCloses = false;
    StartupConnectionAttempt startupConnection;
    DeviceCoordinator deviceFlow;

    enum class CommandType { Start, Stop, Retry, Switch };
    struct DeviceCommand { CommandType type; size_t index = static_cast<size_t>(-1); };
    std::deque<DeviceCommand> commands;
    std::optional<size_t> switchingTo;
    bool switching = false;
    std::vector<size_t> restartAfterStop;
    std::string restartUsbTarget;
    std::wstring autoConnectMessage;
    int panelScrollY = 0;
    int panelWheelRemainder = 0;
    int panelLayoutWidth = -1;
    int panelLayoutScrollY = -1;
    unsigned panelLayoutDpi = 0;
    bool panelLayoutAdvanced = false;
};

void SetPanelNotice(TrayContext* ctx, std::wstring text, int durationMs = 4000);

// Rebuilds the sender list to match cfg.devices, stopping every running sender
// first. Called after the device list changed — a live stream survives only the
// settings changes that leave the list alone (see IDM_SETTINGS).
void RebuildSenders(TrayContext* ctx)
{
    if (ctx->usbApp) {
        ctx->usbApp->Stop();
        ctx->usbApp.reset();
        ctx->activeUsbTarget.clear();
    }
    for (auto& app : ctx->apps)
        app->Stop();
    ctx->apps.clear();
    for (size_t i = 0; i < ctx->cfg.devices.size(); ++i)
        ctx->apps.push_back(std::make_unique<SenderApp>());
}

StreamSettings MakeStreamSettings(const Config& cfg, bool usb = false)
{
    StreamSettings settings;
    // The receiver presents a 60 Hz virtual monitor. Preserve the selected
    // bitrate on both transports; USB only caps the presentation frame rate.
    settings.fps = usb ? std::min(cfg.fps, 60u) : cfg.fps;
    settings.bitrateBps = cfg.bitrateMbps * 1'000'000u;
    settings.requirePrivateNetwork = cfg.requirePrivateNetwork;
    return settings;
}

void RefreshUsbDevices(TrayContext* ctx, bool force = false)
{
    auto now = std::chrono::steady_clock::now();
    if (!force && now - ctx->lastUsbRefresh < std::chrono::seconds(2))
        return;
    ctx->lastUsbRefresh = now;
    ctx->usbDevices = ListUsbMuxDevices();
}

// The label an iPad publishes for itself. Its Bonjour instance name is what the
// app's settings call the name — but that field falls back to the system name,
// and iOS hands out a plain "iPad" there, so a generic instance name is passed
// over for the host name (the iOS device name, e.g. "iPad-Pro.local").
std::string ReceiverLabel(const MdnsReceiver& receiver)
{
    bool generic = receiver.instance.empty() || receiver.instance == "iPad" || receiver.instance == "iPhone" ||
                   receiver.instance == "OpenDisplay";
    if (!generic)
        return receiver.instance;

    std::string host = receiver.host;
    const std::string suffix = ".local";
    if (host.size() > suffix.size() && host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0)
        host.resize(host.size() - suffix.size());
    return host.empty() ? receiver.instance : host;
}

// Refreshes the advertised names in the background. Browsing takes a moment and
// DHCP and address fallback need fresh observations; the first pass runs
// immediately so the menu has names shortly after launch.
void RunDiscovery(TrayContext* ctx)
{
    constexpr int kBrowseMs = 800;
    constexpr int kPauseMs = 5'000;

    while (ctx->discoveryRunning) {
        const std::vector<MdnsReceiver> answers = BrowseReceivers(kBrowseMs);
        std::vector<std::string> targets;
        {
            std::lock_guard<std::mutex> lock(ctx->discoveredMutex);
            targets = ctx->neighborTargets;
        }
        // The TCP connection itself normally populates the cache. Refresh only
        // known active/discovered endpoints, never enumerate an IP subnet.
        for (const auto& answer : answers)
            if (!answer.address.empty()) targets.push_back(answer.address);
        const auto neighbors = ReadNeighborSnapshot(targets, true);
        {
            std::lock_guard<std::mutex> lock(ctx->discoveredMutex);
            ctx->discoveryCache.Merge(answers);
            ctx->discoveryCache.Expire();
            ctx->discovered.clear();
            ctx->neighbors = neighbors;
            ctx->discoveryCompleted = true;
            for (const DiscoveryRecord& receiver : ctx->discoveryCache.Records())
                if (receiver.online && !receiver.address.empty())
                    ctx->discovered[receiver.address] = ReceiverLabel(receiver);
        }

        for (int waited = 0; waited < kPauseMs && ctx->discoveryRunning; waited += 250)
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

bool AnyStreaming(const TrayContext* ctx)
{
    if (ctx->usbApp && ctx->usbApp->GetState() == SenderApp::State::Streaming)
        return true;
    for (const auto& app : ctx->apps)
        if (app->GetState() == SenderApp::State::Streaming)
            return true;
    return false;
}

bool AnyRunning(const TrayContext* ctx)
{
    if (ctx->usbApp && ctx->usbApp->IsRunning())
        return true;
    for (const auto& app : ctx->apps)
        if (app->IsRunning())
            return true;
    return false;
}

// Builds the tray icon at runtime (no .ico asset to ship): a little monitor
// glyph with a coloured status dot in the top-left corner. `dotColor` is the
// point — it signals connection state at a glance.
HICON MakeStatusIcon(COLORREF dotColor)
{
    constexpr int S = 32;
    HDC screen = GetDC(nullptr);
    HDC colorDC = CreateCompatibleDC(screen);
    HDC maskDC = CreateCompatibleDC(screen);
    HBITMAP colorBmp = CreateCompatibleBitmap(screen, S, S);
    HBITMAP maskBmp = CreateBitmap(S, S, 1, 1, nullptr); // monochrome AND mask
    ReleaseDC(nullptr, screen);

    HBITMAP oldColor = static_cast<HBITMAP>(SelectObject(colorDC, colorBmp));
    HBITMAP oldMask = static_cast<HBITMAP>(SelectObject(maskDC, maskBmp));

    RECT rc{0, 0, S, S};
    FillRect(colorDC, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));   // colour: black where transparent
    FillRect(maskDC, &rc, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));    // mask: 1 = transparent everywhere

    // Draws a shape on both DCs: real colour on colorDC, opaque (black=0) on
    // the mask so that region shows through.
    auto opaqueMask = [&] {
        SelectObject(maskDC, GetStockObject(BLACK_BRUSH));
        SelectObject(maskDC, GetStockObject(BLACK_PEN));
    };

    // Monitor body + stand.
    HBRUSH bodyBrush = CreateSolidBrush(RGB(55, 65, 90));
    HPEN edgePen = CreatePen(PS_SOLID, 1, RGB(20, 24, 34));
    SelectObject(colorDC, bodyBrush);
    SelectObject(colorDC, edgePen);
    RoundRect(colorDC, 2, 5, 30, 23, 5, 5);
    Rectangle(colorDC, 13, 22, 19, 27);
    Rectangle(colorDC, 8, 27, 24, 30);
    opaqueMask();
    RoundRect(maskDC, 2, 5, 30, 23, 5, 5);
    Rectangle(maskDC, 13, 22, 19, 27);
    Rectangle(maskDC, 8, 27, 24, 30);

    // Inner "screen" highlight.
    HBRUSH innerBrush = CreateSolidBrush(RGB(120, 150, 195));
    SelectObject(colorDC, innerBrush);
    SelectObject(colorDC, static_cast<HPEN>(GetStockObject(NULL_PEN)));
    RoundRect(colorDC, 5, 8, 27, 20, 3, 3);

    // Status dot, top-left corner, with a light outline for contrast.
    HBRUSH dotBrush = CreateSolidBrush(dotColor);
    HPEN dotPen = CreatePen(PS_SOLID, 1, RGB(245, 245, 245));
    SelectObject(colorDC, dotBrush);
    SelectObject(colorDC, dotPen);
    Ellipse(colorDC, 0, 0, 14, 14);
    opaqueMask();
    Ellipse(maskDC, 0, 0, 14, 14);

    SelectObject(colorDC, oldColor);
    SelectObject(maskDC, oldMask);
    DeleteDC(colorDC);
    DeleteDC(maskDC);
    DeleteObject(bodyBrush);
    DeleteObject(edgePen);
    DeleteObject(innerBrush);
    DeleteObject(dotBrush);
    DeleteObject(dotPen);

    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = colorBmp;
    ii.hbmMask = maskBmp;
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(colorBmp);
    DeleteObject(maskBmp);
    return icon;
}

HICON PickIcon(TrayContext* ctx)
{
    if (ctx->cfg.devices.empty() && !ctx->usbApp)
        return ctx->iconGrey;
    if (AnyStreaming(ctx))
        return ctx->iconGreen;
    return ctx->iconRed;
}

std::wstring Widen(const std::string& s)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// A configured device is "<address>[ <name>]": the iPad only ever calls itself
// "iPad" on the wire (iOS hands out the model, not the device name) and neither
// DNS nor mDNS resolves these addresses, so a readable label can only come from
// the user. Everything up to the first space is the address, the rest is the
// label; without one the address is the label.
struct DeviceEntry {
    std::string address;
    std::string label;
};

DeviceEntry SplitDevice(const DeviceConfig& device)
{
    const std::string address = !device.lastIpv4.empty() ? device.lastIpv4 : device.bonjourHost;
    return {address, device.name.empty() ? address : device.name};
}

std::string ResolveDeviceTarget(const TrayContext* ctx, size_t index)
{
    if (ctx == nullptr || index >= ctx->cfg.devices.size()) return {};
    std::lock_guard<std::mutex> lock(ctx->discoveredMutex);
    const auto candidates = BuildDeviceCandidates(ctx->cfg.devices[index], ctx->discoveryCache.Records(), ctx->neighbors.records);
    return candidates.empty() ? std::string{} : candidates.front();
}

StreamSettings MakeDeviceStreamSettings(const TrayContext* ctx, size_t index)
{
    StreamSettings settings = MakeStreamSettings(ctx->cfg);
    if (index >= ctx->cfg.devices.size()) return settings;
    std::lock_guard<std::mutex> lock(ctx->discoveredMutex);
    settings.candidateTargets = BuildDeviceCandidates(ctx->cfg.devices[index], ctx->discoveryCache.Records(), ctx->neighbors.records);
    return settings;
}

void RefreshConnectionCandidates(TrayContext* ctx)
{
    if (ctx->preview) return;
    std::vector<std::string> targets;
    for (size_t i = 0; i < ctx->apps.size() && i < ctx->cfg.devices.size(); ++i) {
        ctx->apps[i]->UpdateConnectionCandidates(MakeDeviceStreamSettings(ctx, i).candidateTargets);
        const auto live = ctx->apps[i]->Snapshot();
        if (!live.connectedAddress.empty()) targets.push_back(live.connectedAddress);
    }
    std::lock_guard<std::mutex> lock(ctx->discoveredMutex);
    ctx->neighborTargets = std::move(targets);
}

std::chrono::milliseconds StartupAttemptBudget(const TrayContext* ctx)
{
    if (ctx->startupConnection.transport == ConnectionTransport::Wifi &&
        ctx->startupConnection.activeDeviceIndex < ctx->apps.size()) {
        const auto count = std::clamp(ctx->apps[ctx->startupConnection.activeDeviceIndex]->Snapshot().candidateCount, 1u, 8u);
        return std::chrono::milliseconds(8000 * count + 4000);
    }
    return kStartupConnectionAttemptTimeout;
}

uint16_t DevicePort(const TrayContext* ctx, size_t index)
{
    if (!ctx->cfg.devices[index].id.empty()) {
        std::lock_guard<std::mutex> lock(ctx->discoveredMutex);
        const auto record = ctx->discoveryCache.FindById(ctx->cfg.devices[index].id);
        if (record && record->online && record->port != 0) return record->port;
    }
    const uint16_t configured = ctx->cfg.devices[index].port;
    return configured == 0 ? ctx->cfg.port : configured;
}

std::string Narrow(const std::wstring& w)
{
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool IsElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION el{};
    DWORD size = sizeof(el);
    bool ok = GetTokenInformation(token, TokenElevation, &el, sizeof(el), &size) != 0;
    CloseHandle(token);
    return ok && el.TokenIsElevated;
}

// One line per iPad: "iPad-Pro 2732x2048" / "connecting..." / "off".
// Also the menu entry's text, so both say the same thing.
// What to call this iPad: the name the user typed in Settings wins, then the
// one it advertises over Bonjour, and the address is the fallback.
std::wstring DeviceLabel(const TrayContext* ctx, size_t index)
{
    const DeviceConfig& configured = ctx->cfg.devices[index];
    DeviceEntry entry = SplitDevice(configured);
    if (entry.label != entry.address)
        return Widen(entry.label);

    std::lock_guard<std::mutex> lock(ctx->discoveredMutex);
    if (!configured.id.empty()) {
        const auto record = ctx->discoveryCache.FindById(configured.id);
        if (record && !ReceiverLabel(*record).empty()) return Widen(ReceiverLabel(*record));
    }
    auto discovered = ctx->discovered.find(entry.address);
    return Widen(discovered != ctx->discovered.end() ? discovered->second : entry.address);
}

std::wstring DeviceStatusText(const TrayContext* ctx, size_t index)
{
    // Configured TCP targets are the LAN/Wi-Fi transport. Marking this in the
    // menu makes it unambiguous beside the separately enumerated USB device.
    std::wstring text = DeviceLabel(ctx, index) + L"（WiFi）";
    const SenderApp& app = *ctx->apps[index];
    switch (app.GetState()) {
        case SenderApp::State::Streaming:
            return text + L"  " + std::to_wstring(app.Width()) + L"x" + std::to_wstring(app.Height());
        case SenderApp::State::Connecting:
            return text + L"  正在连接...";
        case SenderApp::State::Blocked:
            // Only one panel size can be on the air (see AcquirePanel): name
            // the size that holds it, otherwise "waiting" looks like a hang.
            return text + L"  等待 " + std::to_wstring(app.BlockedByWidth()) + L"x" +
                   std::to_wstring(app.BlockedByHeight());
        case SenderApp::State::UnsafeNetwork:
            return text + L"  已阻止：网络不是专用网络";
        default:
            return text + L"  已断开";
    }
}

std::wstring UsbStatusText(const TrayContext* ctx)
{
    if (!ctx->usbApp)
        return L"iPad（USB）  已断开";
    const SenderApp& app = *ctx->usbApp;
    const std::wstring text = L"iPad（USB）";
    switch (app.GetState()) {
        case SenderApp::State::Streaming:
            return text + L"  " + std::to_wstring(app.Width()) + L"x" + std::to_wstring(app.Height());
        case SenderApp::State::Connecting:
            return text + L"  正在连接...";
        case SenderApp::State::Blocked:
            // Only one panel size can be on the air (see AcquirePanel): name
            // the size that holds it, otherwise "waiting" looks like a hang.
            return text + L"  等待 " + std::to_wstring(app.BlockedByWidth()) + L"x" +
                   std::to_wstring(app.BlockedByHeight());
        case SenderApp::State::UnsafeNetwork:
            return text + L"  已阻止：网络不是专用网络";
        default:
            return text + L"  已断开";
    }
}

void StopUsbSession(TrayContext* ctx)
{
    if (!ctx->usbApp)
        return;
    ctx->usbApp->Stop();
    ctx->usbApp.reset();
    ctx->activeUsbTarget.clear();
}

void StopConfiguredSenders(TrayContext* ctx)
{
    for (auto& app : ctx->apps)
        app->Stop();
}

void RequestStopConfiguredSenders(TrayContext* ctx)
{
    for (auto& app : ctx->apps) app->RequestStop();
}

void StartUsbSession(TrayContext* ctx, std::string target)
{
    // The receiver accepts only one desktop stream. Until a future protocol
    // identity map can associate a USB UDID with a specific Bonjour receiver,
    // switching to cable deliberately stops configured Wi-Fi sessions first.
    StopConfiguredSenders(ctx);
    StopUsbSession(ctx);
    ctx->activeUsbTarget = std::move(target);
    ctx->usbApp = std::make_unique<SenderApp>();
    ctx->usbApp->Start(ctx->activeUsbTarget, ctx->cfg.port, MakeStreamSettings(ctx->cfg, true));
}

bool IsWifiStreaming(const TrayContext* ctx)
{
    for (const auto& app : ctx->apps)
        if (app->GetState() == SenderApp::State::Streaming)
            return true;
    return false;
}

bool IsTransportStreaming(const TrayContext* ctx, ConnectionTransport transport)
{
    switch (transport) {
        case ConnectionTransport::Usb:
            return ctx->usbApp && ctx->usbApp->GetState() == SenderApp::State::Streaming;
        case ConnectionTransport::Wifi:
            return IsWifiStreaming(ctx);
        default:
            return false;
    }
}

std::optional<std::string> PreferredUsbTarget(const TrayContext* ctx)
{
    if (ctx == nullptr || ctx->usbDevices.empty())
        return std::nullopt;
    for (const UsbMuxDevice& device : ctx->usbDevices) {
        const std::string target = MakeUsbMuxTarget(device.udid);
        if (!ctx->cfg.lastUsbTarget.empty() && target == ctx->cfg.lastUsbTarget)
            return target;
    }
    // With no remembered cable identity, only a single inserted iPad is
    // unambiguous.  Several cables require the user to choose in the tray.
    if (ctx->usbDevices.size() == 1)
        return MakeUsbMuxTarget(ctx->usbDevices.front().udid);
    return std::nullopt;
}

bool StartWifiSession(TrayContext* ctx)
{
    if (ctx == nullptr || ctx->apps.empty())
        return false;
    StopUsbSession(ctx);
    const auto first = ctx->deviceFlow.Begin(ctx->cfg.devices, ctx->cfg.preferredDeviceId);
    const std::vector<size_t> order = ctx->deviceFlow.Order();
    if (order.empty()) return false;
    ctx->startupConnection.wifiOrder = order;
    ctx->startupConnection.wifiPosition = 0;
    ctx->startupConnection.activeDeviceIndex = order.front();
    const size_t index = *first;
    const std::string target = ResolveDeviceTarget(ctx, index);
    if (target.empty()) return false;
    ctx->apps[index]->Start(target, DevicePort(ctx, index), MakeDeviceStreamSettings(ctx, index), ctx->cfg.devices[index].id);
    return true;
}

bool StartWifiCandidate(TrayContext* ctx, size_t orderPosition)
{
    if (ctx == nullptr || orderPosition >= ctx->startupConnection.wifiOrder.size()) return false;
    const size_t index = ctx->startupConnection.wifiOrder[orderPosition];
    if (index >= ctx->apps.size()) return false;
    const std::string target = ResolveDeviceTarget(ctx, index);
    if (target.empty()) return false;
    ctx->startupConnection.wifiPosition = orderPosition;
    ctx->startupConnection.activeDeviceIndex = index;
    ctx->startupConnection.waitingNext = false;
    ctx->startupConnection.started = std::chrono::steady_clock::now();
    ctx->apps[index]->Start(target, DevicePort(ctx, index), MakeDeviceStreamSettings(ctx, index), ctx->cfg.devices[index].id);
    Logf("tray", "auto-connect: trying WiFi device priority %zu\n", orderPosition + 1);
    return true;
}

bool StartTransportAttempt(TrayContext* ctx, ConnectionTransport transport)
{
    if (ctx == nullptr)
        return false;
    if (transport == ConnectionTransport::Usb) {
        std::optional<std::string> target = PreferredUsbTarget(ctx);
        if (!target)
            return false;
        StartUsbSession(ctx, std::move(*target));
        return true;
    }
    if (transport == ConnectionTransport::Wifi)
        return StartWifiSession(ctx);
    return false;
}

void StopTransportAttempt(TrayContext* ctx, ConnectionTransport transport)
{
    if (transport == ConnectionTransport::Usb)
        StopUsbSession(ctx);
    else if (transport == ConnectionTransport::Wifi)
        StopConfiguredSenders(ctx);
}

void RequestStopTransportAttempt(TrayContext* ctx, ConnectionTransport transport)
{
    if (transport == ConnectionTransport::Usb) {
        if (ctx->usbApp) ctx->usbApp->RequestStop();
    } else if (transport == ConnectionTransport::Wifi) {
        RequestStopConfiguredSenders(ctx);
    }
}

ConnectionTransport OtherTransport(ConnectionTransport transport)
{
    return transport == ConnectionTransport::Usb ? ConnectionTransport::Wifi :
           transport == ConnectionTransport::Wifi ? ConnectionTransport::Usb : ConnectionTransport::None;
}

void RememberSuccessfulTransport(TrayContext* ctx)
{
    if (ctx == nullptr)
        return;
    if (ctx->usbApp && ctx->usbApp->GetState() == SenderApp::State::Streaming) {
        if (ctx->cfg.lastConnectionTransport != "usb" || ctx->cfg.lastUsbTarget != ctx->activeUsbTarget) {
            ctx->cfg.lastConnectionTransport = "usb";
            ctx->cfg.lastUsbTarget = ctx->activeUsbTarget;
            ctx->cfg.Save();
        }
        return;
    }
    for (size_t index = 0; index < ctx->apps.size() && index < ctx->cfg.devices.size(); ++index) {
        if (ctx->apps[index]->GetState() != SenderApp::State::Streaming)
            continue;
        const ConnectionSnapshot snapshot = ctx->apps[index]->Snapshot();
        if (snapshot.phase != ConnectionPhase::Streaming) continue;
        const std::string address = snapshot.connectedAddress;
        if (address.empty()) continue;
        bool changed = ctx->cfg.lastConnectionTransport != "wifi" || ctx->cfg.lastWifiAddress != address;
        if (ctx->cfg.devices[index].id.empty() && !snapshot.deviceId.empty()) {
            ctx->cfg.devices[index].id = snapshot.deviceId;
            changed = true;
        }
        const auto seen = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::system_clock::now().time_since_epoch()).count();
        if (ctx->cfg.devices[index].lastSeen == 0 || seen - ctx->cfg.devices[index].lastSeen >= 3600) {
            ctx->cfg.devices[index].lastSeen = seen;
            changed = true;
        }
        if (!ctx->cfg.devices[index].id.empty()) {
            std::lock_guard<std::mutex> lock(ctx->discoveredMutex);
            changed = LearnVerifiedMacBinding(ctx->cfg.devices[index], address, snapshot.deviceId, ctx->neighbors.records) || changed;
            const auto record = ctx->discoveryCache.FindById(ctx->cfg.devices[index].id);
            if (record && !record->host.empty() && record->host != ctx->cfg.devices[index].bonjourHost) {
                ctx->cfg.devices[index].bonjourHost = record->host;
                changed = true;
            }
        }
        if (changed) {
            ctx->cfg.lastConnectionTransport = "wifi";
            ctx->cfg.lastWifiAddress = address;
            ctx->cfg.devices[index].lastIpv4 = address;
            ctx->cfg.Save();
        }
        return;
    }
}

void BeginStartupAutoConnect(TrayContext* ctx)
{
    if (ctx == nullptr)
        return;
    RefreshUsbDevices(ctx, true);
    ctx->autoConnectMessage.clear();
    ctx->startupConnection = {};

    ConnectionTransport preferred = ConnectionTransport::None;
    if (ctx->cfg.lastConnectionTransport == "usb")
        preferred = ConnectionTransport::Usb;
    else if (ctx->cfg.lastConnectionTransport == "wifi")
        preferred = ConnectionTransport::Wifi;
    else
        preferred = PreferredUsbTarget(ctx) ? ConnectionTransport::Usb : ConnectionTransport::Wifi;

    if (StartTransportAttempt(ctx, preferred)) {
        ctx->startupConnection.transport = preferred;
        ctx->startupConnection.fallbackUsed = false;
        ctx->startupConnection.started = std::chrono::steady_clock::now();
        Logf("tray", "startup connection: trying %s first\n", preferred == ConnectionTransport::Usb ? "USB" : "WiFi");
        return;
    }

    const ConnectionTransport fallback = OtherTransport(preferred);
    if (StartTransportAttempt(ctx, fallback)) {
        ctx->startupConnection.transport = fallback;
        ctx->startupConnection.fallbackUsed = true;
        ctx->startupConnection.started = std::chrono::steady_clock::now();
        Logf("tray", "startup connection: preferred path unavailable; trying %s\n",
             fallback == ConnectionTransport::Usb ? "USB" : "WiFi");
    }
}

void AdvanceStartupAutoConnect(TrayContext* ctx)
{
    if (ctx == nullptr || ctx->startupConnection.transport == ConnectionTransport::None)
        return;
    if (IsTransportStreaming(ctx, ctx->startupConnection.transport)) {
        ctx->autoConnectMessage.clear();
        if (ctx->startupConnection.transport == ConnectionTransport::Wifi &&
            ctx->startupConnection.activeDeviceIndex < ctx->cfg.devices.size())
            ctx->deviceFlow.ReportStreaming(ctx->cfg.devices[ctx->startupConnection.activeDeviceIndex].id);
        RememberSuccessfulTransport(ctx);
        ctx->startupConnection.transport = ConnectionTransport::None;
        return;
    }
    if (ctx->startupConnection.waitingTransportStop) {
        if (ctx->startupConnection.transport == ConnectionTransport::Usb) {
            if (ctx->usbApp && ctx->usbApp->IsRunning()) return;
        } else if (std::any_of(ctx->apps.begin(), ctx->apps.end(), [](const auto& app) { return app->IsRunning(); })) {
            return;
        }
        const ConnectionTransport failed = ctx->startupConnection.transport;
        StopTransportAttempt(ctx, failed); // worker is already down; join is immediate
        ctx->startupConnection.waitingTransportStop = false;
        if (ctx->startupConnection.fallbackUsed) {
            ctx->startupConnection.transport = ConnectionTransport::None;
            ctx->autoConnectMessage = L"所有候选设备和传输方式均连接失败；可重试、选择设备，或检查网络/OpenDisplay";
            SetPanelNotice(ctx, ctx->autoConnectMessage, 10000);
            return;
        }
        const ConnectionTransport fallback = OtherTransport(failed);
        if (StartTransportAttempt(ctx, fallback)) {
            ctx->startupConnection.transport = fallback;
            ctx->startupConnection.fallbackUsed = true;
            ctx->startupConnection.started = std::chrono::steady_clock::now();
            return;
        }
        ctx->startupConnection.transport = ConnectionTransport::None;
        ctx->autoConnectMessage = L"没有可用的后备连接；请选择设备或检查 USB/Wi-Fi";
        return;
    }

    if (ctx->startupConnection.transport == ConnectionTransport::Wifi &&
        ctx->startupConnection.activeDeviceIndex < ctx->apps.size()) {
        SenderApp& active = *ctx->apps[ctx->startupConnection.activeDeviceIndex];
        const ConnectionSnapshot snapshot = active.Snapshot();
        const bool globalFailure = snapshot.phase == ConnectionPhase::UnsafeNetwork ||
            snapshot.failure == FailureReason::DriverUnavailable || snapshot.failure == FailureReason::ResolutionUnavailable ||
            snapshot.failure == FailureReason::CaptureUnavailable || snapshot.failure == FailureReason::EncoderUnavailable ||
            snapshot.failure == FailureReason::AnotherSender;
        if (globalFailure) {
            ctx->deviceFlow.ReportFailure(DeviceFailureClass::Global);
            active.RequestStop();
            ctx->startupConnection.transport = ConnectionTransport::None;
            ctx->autoConnectMessage = L"自动连接已停止：请先处理可信网络、驱动或编码器问题，再点击重试";
            SetPanelNotice(ctx, ctx->autoConnectMessage);
            return;
        }
        if (ctx->startupConnection.waitingNext) {
            if (active.IsRunning()) return;
            const size_t next = ctx->startupConnection.wifiPosition + 1;
            if (StartWifiCandidate(ctx, next)) return;
            ctx->startupConnection.waitingNext = false;
            // No more WiFi devices: the transport fallback below decides
            // whether a remembered USB device is available.
        } else if (snapshot.phase == ConnectionPhase::Failed ||
                   std::chrono::steady_clock::now() - ctx->startupConnection.started >= StartupAttemptBudget(ctx)) {
            const size_t next = ctx->startupConnection.wifiPosition + 1;
            if (next < ctx->startupConnection.wifiOrder.size()) {
                ctx->deviceFlow.ReportFailure(DeviceFailureClass::Device);
                active.RequestStop();
                ctx->startupConnection.waitingNext = true;
                SetPanelNotice(ctx, L"首选设备未连接，正在尝试下一台设备");
                return;
            }
            ctx->deviceFlow.ReportFailure(DeviceFailureClass::Device);
            active.RequestStop();
            if (active.IsRunning()) return;
        }
    }
    if (std::chrono::steady_clock::now() - ctx->startupConnection.started < StartupAttemptBudget(ctx))
        return;

    RequestStopTransportAttempt(ctx, ctx->startupConnection.transport);
    ctx->startupConnection.waitingTransportStop = true;
}

std::set<std::string> RunningAddresses(const TrayContext* ctx)
{
    std::set<std::string> running;
    size_t count = std::min(ctx->apps.size(), ctx->cfg.devices.size());
    for (size_t i = 0; i < count; ++i)
        if (ctx->apps[i]->IsRunning())
            running.insert(SplitDevice(ctx->cfg.devices[i]).address);
    return running;
}

void StartAddresses(TrayContext* ctx, const std::set<std::string>& addresses)
{
    for (size_t i = 0; i < ctx->apps.size(); ++i) {
        std::string address = SplitDevice(ctx->cfg.devices[i]).address;
        if (addresses.contains(address))
            ctx->apps[i]->Start(std::move(address), DevicePort(ctx, i), MakeDeviceStreamSettings(ctx, i),
                                ctx->cfg.devices[i].id);
    }
}

void QueueDeviceCommand(TrayContext* ctx, TrayContext::CommandType type, size_t index = static_cast<size_t>(-1))
{
    if (ctx == nullptr) return;
    if ((type == TrayContext::CommandType::Start || type == TrayContext::CommandType::Retry ||
         type == TrayContext::CommandType::Switch) && index >= ctx->cfg.devices.size()) return;
    if (type == TrayContext::CommandType::Switch && ctx->switchingTo && *ctx->switchingTo == index) return;
    if (type == TrayContext::CommandType::Switch && !ctx->deviceFlow.RequestSwitch(index)) return;
    const bool duplicate = std::any_of(ctx->commands.begin(), ctx->commands.end(), [&](const auto& command) {
        return command.type == type && command.index == index;
    });
    if (!duplicate) ctx->commands.push_back({type, index});
}

void ProcessDeviceCommands(TrayContext* ctx)
{
    if (ctx == nullptr) return;
    ctx->retiredApps.erase(std::remove_if(ctx->retiredApps.begin(), ctx->retiredApps.end(), [](auto& app) {
        if (app->IsRunning()) return false;
        app->Stop();
        return true;
    }), ctx->retiredApps.end());
    if (ctx->switching) {
        if (!ctx->restartAfterStop.empty() || !ctx->restartUsbTarget.empty()) {
            for (size_t index : ctx->restartAfterStop)
                if (index < ctx->apps.size() && ctx->apps[index]->IsRunning()) return;
            if (ctx->usbApp && ctx->usbApp->IsRunning()) return;
            for (size_t index : ctx->restartAfterStop) if (index < ctx->apps.size()) {
                ctx->apps[index]->Start(ResolveDeviceTarget(ctx, index), DevicePort(ctx, index),
                                        MakeDeviceStreamSettings(ctx, index), ctx->cfg.devices[index].id);
            }
            if (!ctx->restartUsbTarget.empty()) {
                if (ctx->usbApp) { ctx->usbApp->Stop(); ctx->usbApp.reset(); }
                ctx->activeUsbTarget = ctx->restartUsbTarget;
                ctx->usbApp = std::make_unique<SenderApp>();
                ctx->usbApp->Start(ctx->activeUsbTarget, ctx->cfg.port, MakeStreamSettings(ctx->cfg, true));
            }
            ctx->restartAfterStop.clear();
            ctx->restartUsbTarget.clear();
            ctx->switching = false;
            SetPanelNotice(ctx, L"设置已应用，原有连接正在恢复");
            return;
        }
        if (AnyRunning(ctx)) return;
        if (ctx->usbApp) { ctx->usbApp->Stop(); ctx->usbApp.reset(); ctx->activeUsbTarget.clear(); }
        if (ctx->switchingTo && *ctx->switchingTo < ctx->apps.size()) {
            const size_t index = *ctx->switchingTo;
            const std::string target = ResolveDeviceTarget(ctx, index);
            if (!target.empty()) {
                ctx->apps[index]->Start(target, DevicePort(ctx, index), MakeDeviceStreamSettings(ctx, index),
                                        ctx->cfg.devices[index].id);
                SetPanelNotice(ctx, L"已启动所选设备，正在等待 OpenDisplay");
            } else {
                SetPanelNotice(ctx, L"所选设备没有可用地址；请刷新 mDNS 或填写手动地址");
            }
        } else {
            SetPanelNotice(ctx, L"连接已停止");
        }
        ctx->switching = false;
        ctx->switchingTo.reset();
        ctx->deviceFlow.CompleteSwitch(false);
        return;
    }
    if (ctx->commands.empty()) return;
    const TrayContext::DeviceCommand command = ctx->commands.front();
    ctx->commands.pop_front();
    ctx->startupConnection.transport = ConnectionTransport::None;
    switch (command.type) {
        case TrayContext::CommandType::Stop:
            ctx->deviceFlow.Reset();
            if (ctx->usbApp) ctx->usbApp->RequestStop();
            RequestStopConfiguredSenders(ctx);
            ctx->switching = true;
            ctx->switchingTo.reset();
            break;
        case TrayContext::CommandType::Start:
        case TrayContext::CommandType::Retry:
        case TrayContext::CommandType::Switch:
            if (ctx->usbApp) ctx->usbApp->RequestStop();
            RequestStopConfiguredSenders(ctx);
            ctx->switching = true;
            ctx->switchingTo = command.index;
            SetPanelNotice(ctx, command.type == TrayContext::CommandType::Switch ? L"正在切换设备…" : L"正在连接设备…");
            break;
    }
}

bool AddTrayIcon(TrayContext* ctx)
{
    ctx->nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    ctx->trayAdded = Shell_NotifyIconW(NIM_ADD, &ctx->nid) != FALSE;
    return ctx->trayAdded;
}

void UpdateStatus(TrayContext* ctx)
{
    std::wstring tip = L"iPad互联";
    bool hasEntry = false;
    if (ctx->usbApp) {
        tip += L" - " + UsbStatusText(ctx);
        hasEntry = true;
    }
    if (!ctx->cfg.devices.empty()) {
        // The tooltip is capped at 128 characters, so with several iPads the
        // tail may be cut — the menu carries the full list.
        for (size_t i = 0; i < ctx->cfg.devices.size(); ++i) {
            tip += (hasEntry ? L" | " : L" - ") + DeviceStatusText(ctx, i);
            hasEntry = true;
        }
    }
    if (!hasEntry) {
        tip += L" - 尚未配置 iPad";
    }

    ctx->nid.hIcon = PickIcon(ctx);
    wcsncpy_s(ctx->nid.szTip, tip.c_str(), _TRUNCATE);
    ctx->nid.uFlags = NIF_ICON | NIF_TIP;
    if (!ctx->trayAdded && !AddTrayIcon(ctx))
        return; // the one-second status timer retries while Explorer starts
    if (!Shell_NotifyIconW(NIM_MODIFY, &ctx->nid)) {
        ctx->trayAdded = false;
        AddTrayIcon(ctx);
    }
}

void ApplyAndRestart(TrayContext* ctx, const std::vector<DeviceConfig>& oldDevices,
                     const std::set<std::string>& reconnect, bool restartActive)
{
    // Screen contents are sensitive and connection is explicit by default.
    // Preserve exactly the targets that were already active; changing one
    // quality field must never start every configured iPad.
    std::vector<std::unique_ptr<SenderApp>> oldApps = std::move(ctx->apps);
    std::vector<bool> used(oldApps.size(), false);
    ctx->apps.reserve(ctx->cfg.devices.size());
    for (size_t next = 0; next < ctx->cfg.devices.size(); ++next) {
        size_t match = oldApps.size();
        for (size_t old = 0; old < oldDevices.size() && old < oldApps.size(); ++old) {
            if (used[old]) continue;
            const bool sameId = !ctx->cfg.devices[next].id.empty() && ctx->cfg.devices[next].id == oldDevices[old].id;
            const bool sameEndpoint = SplitDevice(ctx->cfg.devices[next]).address == SplitDevice(oldDevices[old]).address;
            if (sameId || sameEndpoint) { match = old; break; }
        }
        if (match < oldApps.size()) {
            used[match] = true;
            const bool endpointChanged = SplitDevice(ctx->cfg.devices[next]).address != SplitDevice(oldDevices[match]).address;
            if (oldApps[match]->IsRunning() && (restartActive || endpointChanged)) {
                oldApps[match]->RequestStop();
                ctx->restartAfterStop.push_back(next);
            }
            ctx->apps.push_back(std::move(oldApps[match]));
        } else {
            ctx->apps.push_back(std::make_unique<SenderApp>());
            if (next < oldDevices.size() && reconnect.contains(SplitDevice(oldDevices[next]).address))
                ctx->restartAfterStop.push_back(next);
        }
    }
    for (size_t i = 0; i < oldApps.size(); ++i) if (oldApps[i]) {
        oldApps[i]->RequestStop();
        ctx->retiredApps.push_back(std::move(oldApps[i]));
    }
    if (restartActive && ctx->usbApp && ctx->usbApp->IsRunning()) {
        ctx->restartUsbTarget = ctx->activeUsbTarget;
        ctx->usbApp->RequestStop();
    }
    if (!ctx->restartAfterStop.empty() || !ctx->restartUsbTarget.empty()) {
        std::sort(ctx->restartAfterStop.begin(), ctx->restartAfterStop.end());
        ctx->restartAfterStop.erase(std::unique(ctx->restartAfterStop.begin(), ctx->restartAfterStop.end()),
                                    ctx->restartAfterStop.end());
        ctx->switching = true;
        ctx->switchingTo.reset();
    }
    UpdateStatus(ctx);
}

constexpr wchar_t kControlPanelClass[] = L"IpadConnectControlPanel";

struct PanelPalette {
    COLORREF canvas;
    COLORREF surface;
    COLORREF surfaceSoft;
    COLORREF stroke;
    COLORREF text;
    COLORREF muted;
    COLORREF primary;
    COLORREF primarySoft;
    COLORREF success;
};

PanelPalette ControlPanelPalette(bool dark)
{
    if (dark) {
        return {RGB(20, 27, 34), RGB(29, 38, 47), RGB(38, 49, 59), RGB(58, 73, 85), RGB(239, 245, 248),
                RGB(162, 180, 190), RGB(74, 194, 160), RGB(35, 80, 71), RGB(98, 222, 181)};
    }
    return {RGB(246, 249, 248), RGB(255, 255, 255), RGB(240, 247, 245), RGB(220, 230, 226), RGB(30, 48, 55),
            RGB(104, 125, 132), RGB(43, 126, 105), RGB(226, 243, 237), RGB(72, 172, 137)};
}

unsigned PanelDpi(HWND window)
{
    const auto* ctx = window == nullptr ? nullptr : reinterpret_cast<TrayContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (ctx != nullptr && ctx->preview && ctx->previewDpi != 0) return ctx->previewDpi;
    const UINT dpi = window != nullptr ? GetDpiForWindow(window) : GetDpiForSystem();
    return dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi;
}

int PanelScale(HWND window, int logicalPixels)
{
    return MulDiv(logicalPixels, static_cast<int>(PanelDpi(window)), USER_DEFAULT_SCREEN_DPI);
}

HFONT CreatePanelFont(HWND window, int points, int weight)
{
    return CreateFontW(-MulDiv(points, static_cast<int>(PanelDpi(window)), 72), 0, 0, 0, weight, FALSE, FALSE,
                       FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

void FillRoundedRect(HDC dc, const RECT& rect, int radius, COLORREF color)
{
    HRGN region = CreateRoundRectRgn(rect.left, rect.top, rect.right + 1, rect.bottom + 1, radius, radius);
    HBRUSH brush = CreateSolidBrush(color);
    FillRgn(dc, region, brush);
    DeleteObject(brush);
    DeleteObject(region);
}

void StrokeRoundedRect(HDC dc, const RECT& rect, int radius, COLORREF color)
{
    HBRUSH brush = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
}

void DrawPanelText(HDC dc, const RECT& rect, const std::wstring& text, HFONT font, COLORREF color, UINT format)
{
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.c_str(), -1, const_cast<RECT*>(&rect), format);
    SelectObject(dc, oldFont);
}

PanelLayout BuildPanelLayout(HWND window, const RECT& client)
{
    const auto* ctx = reinterpret_cast<TrayContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return CalculatePanelLayout(static_cast<int>(client.right - client.left), PanelDpi(window),
                                ctx != nullptr && ctx->panelAdvancedSettings);
}

void UpdatePanelScroll(HWND window, TrayContext* ctx)
{
    if (ctx == nullptr) return;
    RECT client{}; GetClientRect(window, &client);
    const int contentHeight = BuildPanelLayout(window, client).contentHeight;
    const int page = (std::max)(1L, client.bottom - client.top);
    ctx->panelScrollY = ClampPanelScroll(ctx->panelScrollY, contentHeight, page);
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
    info.nMin = 0; info.nMax = (std::max)(0, contentHeight - 1); info.nPage = static_cast<UINT>(page); info.nPos = ctx->panelScrollY;
    SetScrollInfo(window, SB_VERT, &info, TRUE);
}

void SetPanelEditText(HWND control, const std::wstring& value)
{
    if (control != nullptr)
        SetWindowTextW(control, value.c_str());
}

std::wstring GetPanelEditText(HWND control)
{
    if (control == nullptr)
        return {};
    const int length = GetWindowTextLengthW(control);
    std::vector<wchar_t> buffer(static_cast<size_t>(std::max(length, 0)) + 1, L'\0');
    GetWindowTextW(control, buffer.data(), static_cast<int>(buffer.size()));
    return buffer.data();
}

void RefreshPanelEditBrush(TrayContext* ctx)
{
    if (ctx == nullptr)
        return;
    if (ctx->panelEditBrush != nullptr)
        DeleteObject(ctx->panelEditBrush);
    ctx->panelEditBrush = CreateSolidBrush(ControlPanelPalette(ctx->cfg.darkTheme).surfaceSoft);
}

void SyncPanelSettingsFromConfig(TrayContext* ctx)
{
    if (ctx == nullptr)
        return;
    std::wstring devices;
    for (const DeviceConfig& device : ctx->cfg.devices) {
        if (!devices.empty())
            devices += L"\r\n";
        devices += DeviceEntryText(device);
    }
    SetPanelEditText(ctx->panelDevices, devices);
    ctx->panelDeviceBaseline = ctx->cfg.devices;
    ctx->panelDeviceTextBaseline = devices;
    SetPanelEditText(ctx->panelPort, std::to_wstring(ctx->cfg.port));
    SetPanelEditText(ctx->panelFps, std::to_wstring(ctx->cfg.fps));
    SetPanelEditText(ctx->panelBitrate, std::to_wstring(ctx->cfg.bitrateMbps));
    ctx->panelAutoReconnect = ctx->cfg.autoReconnect;
    ctx->panelRequirePrivateNetwork = ctx->cfg.requirePrivateNetwork;
    ctx->panelStreamProfile = ctx->cfg.streamProfile;
}

void SetPanelNotice(TrayContext* ctx, std::wstring text, int durationMs)
{
    if (ctx == nullptr)
        return;
    ctx->panelNotice = std::move(text);
    ctx->panelNoticeUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(durationMs);
}

void SetPanelTextIfChanged(HWND control, const std::wstring& value)
{
    if (control != nullptr && GetPanelEditText(control) != value)
        SetWindowTextW(control, value.c_str());
}

std::string SelectedPanelDeviceAddress(TrayContext* ctx)
{
    const int selected = static_cast<int>(SendMessageW(ctx->panelDeviceSelector, CB_GETCURSEL, 0, 0));
    if (selected < 0 || static_cast<size_t>(selected) >= ctx->cfg.devices.size()) return {};
    if (static_cast<size_t>(selected) < ctx->apps.size()) {
        const auto snapshot = ctx->apps[static_cast<size_t>(selected)]->Snapshot();
        if (snapshot.phase == ConnectionPhase::Streaming && !snapshot.connectedAddress.empty())
            return snapshot.connectedAddress;
    }
    const auto& device = ctx->cfg.devices[static_cast<size_t>(selected)];
    if (!device.id.empty()) {
        std::lock_guard<std::mutex> lock(ctx->discoveredMutex);
        const auto record = ctx->discoveryCache.FindById(device.id);
        if (record && record->online && !record->address.empty()) return record->address;
    }
    return ResolveDeviceTarget(ctx, static_cast<size_t>(selected));
}

void RefreshPanelAddressDetails(TrayContext* ctx)
{
    if (ctx == nullptr) return;
    const std::string address = SelectedPanelDeviceAddress(ctx);
    SetPanelTextIfChanged(ctx->panelDeviceAddress, address.empty() ? L"尚未选择设备" : Widen(address));
    std::wstring macText = L"当前网络不可见；仍可通过 IP 连接";
    if (ctx->preview) macText = ctx->previewEmpty ? L"尚未选择设备" : L"02:00:00:12:34:56（示例）";
    else {
        std::lock_guard<std::mutex> lock(ctx->discoveredMutex);
        if (!ctx->discoveryCompleted) macText = L"正在识别当前网络设备…";
        for (const auto& neighbor : ctx->neighbors.records) {
            if (neighbor.address == address) {
                macText = Widen(neighbor.mac) + (neighbor.reachable ? L"  ·  当前可达" : L"  ·  缓存记录");
                break;
            }
        }
    }
    SetPanelTextIfChanged(ctx->panelDeviceMac, macText);
    const int selected = static_cast<int>(SendMessageW(ctx->panelDiscovery, CB_GETCURSEL, 0, 0));
    const bool valid = selected >= 0 && static_cast<size_t>(selected) < ctx->panelDiscoveryAddresses.size();
    SetPanelTextIfChanged(ctx->panelDiscoveryAddress, valid ? Widen(ctx->panelDiscoveryAddresses[static_cast<size_t>(selected)]) : L"尚未发现设备；请让 OpenDisplay 保持前台");
}

void SyncPanelDiscovery(TrayContext* ctx)
{
    if (ctx == nullptr || ctx->panelDiscovery == nullptr) return;
    struct Entry { std::string key, address; std::wstring label; };
    std::vector<Entry> entries;
    std::wstring emptyLabel = L"暂未发现设备";
    {
        std::lock_guard<std::mutex> lock(ctx->discoveredMutex);
        if (!ctx->preview && !ctx->discoveryCompleted) emptyLabel = L"正在发现局域网设备…";
        const auto records = ctx->discoveryCache.Records();
        for (const auto& [address, name] : ctx->discovered) {
            std::string key = "address:" + address;
            for (const auto& record : records) {
                if (record.address != address) continue;
                if (!record.id.empty()) key = "id:" + record.id;
                else if (!record.host.empty()) key = "host:" + record.host;
                break;
            }
            if (std::none_of(entries.begin(), entries.end(), [&](const Entry& entry) { return entry.key == key; }))
                entries.push_back({key, address, Widen(name.empty() ? "iPad" : name)});
        }
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right) { return left.key < right.key; });
    std::vector<std::string> keys, addresses;
    std::vector<std::wstring> rows;
    for (const auto& entry : entries) { keys.push_back(entry.key); addresses.push_back(entry.address); rows.push_back(entry.label); }
    if (entries.empty()) rows.push_back(emptyLabel);
    const int oldSelection = static_cast<int>(SendMessageW(ctx->panelDiscovery, CB_GETCURSEL, 0, 0));
    const std::string oldKey = oldSelection >= 0 && static_cast<size_t>(oldSelection) < ctx->panelDiscoveryKeys.size()
        ? ctx->panelDiscoveryKeys[static_cast<size_t>(oldSelection)] : std::string{};
    if (!ctx->panelDiscoveryInitialized || keys != ctx->panelDiscoveryKeys || rows != ctx->panelDiscoveryRows || addresses != ctx->panelDiscoveryAddresses) {
        SendMessageW(ctx->panelDiscovery, CB_RESETCONTENT, 0, 0);
        ctx->panelDiscoveryInitialized = true;
        ctx->panelDiscoveryKeys = std::move(keys);
        ctx->panelDiscoveryRows = std::move(rows);
        ctx->panelDiscoveryAddresses = std::move(addresses);
        if (entries.empty()) SendMessageW(ctx->panelDiscovery, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(emptyLabel.c_str()));
        else for (const auto& row : ctx->panelDiscoveryRows) SendMessageW(ctx->panelDiscovery, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.c_str()));
        const auto found = std::find(ctx->panelDiscoveryKeys.begin(), ctx->panelDiscoveryKeys.end(), oldKey);
        const int selected = found == ctx->panelDiscoveryKeys.end() ? 0 : static_cast<int>(found - ctx->panelDiscoveryKeys.begin());
        SendMessageW(ctx->panelDiscovery, CB_SETCURSEL, selected, 0);
        EnableWindow(ctx->panelDiscovery, !entries.empty());
    }
    RefreshPanelAddressDetails(ctx);
}

void SyncPanelDeviceSelector(TrayContext* ctx)
{
    if (ctx == nullptr || ctx->panelDeviceSelector == nullptr) return;
    const int oldSelection = static_cast<int>(SendMessageW(ctx->panelDeviceSelector, CB_GETCURSEL, 0, 0));
    const std::string oldKey = oldSelection >= 0 && static_cast<size_t>(oldSelection) < ctx->panelDeviceKeys.size()
        ? ctx->panelDeviceKeys[static_cast<size_t>(oldSelection)] : std::string{};
    std::vector<std::string> keys;
    std::vector<std::wstring> rows;
    for (size_t i = 0; i < ctx->cfg.devices.size(); ++i) {
        const auto& device = ctx->cfg.devices[i];
        keys.push_back(!device.id.empty() ? "id:" + device.id : !device.bonjourHost.empty() ? "host:" + device.bonjourHost : "address:" + SplitDevice(device).address);
        std::wstring row = DeviceLabel(ctx, i);
        if (!device.id.empty() && device.id == ctx->cfg.preferredDeviceId) row += L"  ·  默认";
        row += device.id.empty() ? L"  ·  身份待识别" : L"  ·  已识别";
        {
            std::lock_guard<std::mutex> lock(ctx->discoveredMutex);
            const auto record = ctx->discoveryCache.FindById(device.id);
            row += record && record->online ? L"  ·  在线" : L"  ·  未发现";
        }
        rows.push_back(std::move(row));
    }
    if (rows != ctx->panelDeviceRows || keys != ctx->panelDeviceKeys || SendMessageW(ctx->panelDeviceSelector, CB_GETCOUNT, 0, 0) == 0) {
        SendMessageW(ctx->panelDeviceSelector, CB_RESETCONTENT, 0, 0);
        ctx->panelDeviceRows = std::move(rows);
        ctx->panelDeviceKeys = std::move(keys);
        if (ctx->panelDeviceRows.empty()) SendMessageW(ctx->panelDeviceSelector, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"尚未添加设备"));
        else for (const auto& row : ctx->panelDeviceRows) SendMessageW(ctx->panelDeviceSelector, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.c_str()));
        auto found = std::find(ctx->panelDeviceKeys.begin(), ctx->panelDeviceKeys.end(), oldKey);
        if (found == ctx->panelDeviceKeys.end() && !ctx->cfg.preferredDeviceId.empty())
            found = std::find(ctx->panelDeviceKeys.begin(), ctx->panelDeviceKeys.end(), "id:" + ctx->cfg.preferredDeviceId);
        SendMessageW(ctx->panelDeviceSelector, CB_SETCURSEL, found == ctx->panelDeviceKeys.end() ? 0 : found - ctx->panelDeviceKeys.begin(), 0);
    }
    const int selected = static_cast<int>(SendMessageW(ctx->panelDeviceSelector, CB_GETCURSEL, 0, 0));
    const BOOL enabled = !ctx->cfg.devices.empty();
    EnableWindow(ctx->panelDeviceSelector, !ctx->cfg.devices.empty());
    EnableWindow(ctx->panelDeviceConnect, enabled && !ctx->switching);
    EnableWindow(ctx->panelDeviceDefault, enabled);
    EnableWindow(ctx->panelDeviceUp, enabled && selected > 0);
    EnableWindow(ctx->panelDeviceRemove, enabled);
    SetPanelTextIfChanged(ctx->panelDeviceConnect, ctx->switching ? L"正在切换" : L"连接所选");
    RefreshPanelAddressDetails(ctx);
}

const RECT& PanelButtonRect(const PanelLayout& p, int id)
{
    switch (id) {
        case IDC_PANEL_THEME: return p.themeButton;
        case IDC_PANEL_PRIMARY: return p.primaryConnectionButton;
        case IDC_PANEL_SECONDARY: return p.secondaryConnectionButton;
        case IDC_PANEL_DEVICE_CONNECT: return p.deviceConnectButton;
        case IDC_PANEL_DEVICE_DEFAULT: return p.deviceDefaultButton;
        case IDC_PANEL_DEVICE_UP: return p.deviceUpButton;
        case IDC_PANEL_DEVICE_REMOVE: return p.deviceRemoveButton;
        case IDC_PANEL_DISPLAY_SETTINGS: return p.displaySettingsButton;
        case IDC_PANEL_PLACE_RIGHT: return p.placeRightButton;
        case IDC_PANEL_DIAGNOSTICS: return p.diagnosticsButton;
        case IDC_PANEL_ADVANCED: return p.advancedSettingsButton;
        case IDC_PANEL_ADD_DISCOVERED: return p.addDiscoveredButton;
        case IDC_PANEL_AUTO_RECONNECT: return p.autoReconnectToggle;
        case IDC_PANEL_PRIVATE_NETWORK: return p.privateNetworkToggle;
        case IDC_PANEL_SAVE: return p.saveSettingsButton;
        case IDC_PANEL_PROFILE_BALANCED: return p.profileCards[0];
        case IDC_PANEL_PROFILE_LOW: return p.profileCards[1];
        default: return p.profileCards[2];
    }
}

void UpdatePanelFonts(HWND window, TrayContext* ctx)
{
    const HFONT previous = ctx->panelFont;
    ctx->panelFont = CreatePanelFont(window, 10, FW_NORMAL);
    EnumChildWindows(window, [](HWND control, LPARAM parameter) -> BOOL {
        SendMessageW(control, WM_SETFONT, static_cast<WPARAM>(parameter), TRUE);
        return TRUE;
    }, reinterpret_cast<LPARAM>(ctx->panelFont));
    for (HWND combo : {ctx->panelDiscovery, ctx->panelDeviceSelector}) {
        SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), PanelScale(window, 30));
        SendMessageW(combo, CB_SETITEMHEIGHT, 0, PanelScale(window, 30));
        SendMessageW(combo, CB_SETDROPPEDWIDTH, PanelScale(window, 480), 0);
    }
    if (previous != nullptr) DeleteObject(previous);
}

void CreatePanelSettingsControls(HWND window, TrayContext* ctx)
{
    const auto createEdit = [&](int id, DWORD extraStyle) {
        HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | extraStyle,
                                    0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), ctx->hInstance, nullptr);
        if (edit != nullptr && (extraStyle & ES_READONLY) == 0)
            SendMessageW(edit, EM_SETLIMITTEXT, id == IDC_PANEL_DEVICES ? 8192 : 5, 0);
        return edit;
    };
    ctx->panelDevices = createEdit(IDC_PANEL_DEVICES, ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | WS_VSCROLL);
    ctx->panelPort = createEdit(IDC_PANEL_PORT, ES_AUTOHSCROLL | ES_NUMBER);
    ctx->panelFps = createEdit(IDC_PANEL_FPS, ES_AUTOHSCROLL | ES_NUMBER);
    ctx->panelBitrate = createEdit(IDC_PANEL_BITRATE, ES_AUTOHSCROLL | ES_NUMBER);
    ctx->panelDeviceAddress = createEdit(IDC_PANEL_DEVICE_ADDRESS, ES_AUTOHSCROLL | ES_READONLY);
    ctx->panelDeviceMac = createEdit(IDC_PANEL_DEVICE_MAC, ES_AUTOHSCROLL | ES_READONLY);
    ctx->panelDiscoveryAddress = createEdit(IDC_PANEL_DISCOVERY_ADDRESS, ES_AUTOHSCROLL | ES_READONLY);
    const auto createCombo = [&](int id) {
        return CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                              0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), ctx->hInstance, nullptr);
    };
    ctx->panelDeviceSelector = createCombo(IDC_PANEL_DEVICE_SELECT);
    ctx->panelDiscovery = createCombo(IDC_PANEL_DISCOVERY);
    const std::pair<int, const wchar_t*> buttons[] = {
        {IDC_PANEL_THEME, L"深色"}, {IDC_PANEL_DEVICE_CONNECT, L"连接所选"}, {IDC_PANEL_PRIMARY, L"自动连接"},
        {IDC_PANEL_SECONDARY, L"重试 / 切换"}, {IDC_PANEL_DEVICE_DEFAULT, L"设为默认"}, {IDC_PANEL_DEVICE_UP, L"优先上移"},
        {IDC_PANEL_DEVICE_REMOVE, L"移除设备"}, {IDC_PANEL_ADVANCED, L"高级设置"}, {IDC_PANEL_ADD_DISCOVERED, L"添加设备"},
        {IDC_PANEL_AUTO_RECONNECT, L"启动时自动连接"}, {IDC_PANEL_PRIVATE_NETWORK, L"仅允许专用 / 域网络"},
        {IDC_PANEL_SAVE, L"保存连接设置"}, {IDC_PANEL_PROFILE_BALANCED, L"均衡 · 30 Mbps"},
        {IDC_PANEL_PROFILE_LOW, L"省带宽 · 15 Mbps"}, {IDC_PANEL_PROFILE_SHARP, L"清晰 · 45 Mbps"},
        {IDC_PANEL_DISPLAY_SETTINGS, L"Windows 显示设置"}, {IDC_PANEL_PLACE_RIGHT, L"副屏置于右侧"},
        {IDC_PANEL_DIAGNOSTICS, L"导出诊断"},
    };
    for (const auto& [id, label] : buttons) {
        HWND button = CreateWindowExW(0, L"BUTTON", label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                     0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), ctx->hInstance, nullptr);
        ctx->panelButtons.emplace_back(id, button);
        if (id == IDC_PANEL_DEVICE_CONNECT) ctx->panelDeviceConnect = button;
        if (id == IDC_PANEL_DEVICE_DEFAULT) ctx->panelDeviceDefault = button;
        if (id == IDC_PANEL_DEVICE_UP) ctx->panelDeviceUp = button;
        if (id == IDC_PANEL_DEVICE_REMOVE) ctx->panelDeviceRemove = button;
    }
    UpdatePanelFonts(window, ctx);
    RefreshPanelEditBrush(ctx);
    SyncPanelSettingsFromConfig(ctx);
    SyncPanelDiscovery(ctx);
    SyncPanelDeviceSelector(ctx);
}

void LayoutPanelSettingsControls(HWND window, TrayContext* ctx)
{
    if (ctx == nullptr) return;
    RECT client{};
    GetClientRect(window, &client);
    const unsigned dpi = PanelDpi(window);
    const bool structureChanged = ctx->panelLayoutWidth != client.right || ctx->panelLayoutDpi != dpi ||
                                  ctx->panelLayoutAdvanced != ctx->panelAdvancedSettings;
    SetPanelTextIfChanged(GetDlgItem(window, IDC_PANEL_ADVANCED), ctx->panelAdvancedSettings ? L"收起高级设置" : L"高级设置");
    SetPanelTextIfChanged(GetDlgItem(window, IDC_PANEL_THEME), ctx->cfg.darkTheme ? L"浅色" : L"深色");
    if (!structureChanged && ctx->panelLayoutScrollY == ctx->panelScrollY) return;
    const PanelLayout layout = BuildPanelLayout(window, client);
    struct Placement { HWND control; RECT bounds; UINT flags; };
    std::vector<Placement> placements;
    const auto place = [&](HWND control, const RECT& source, int extraHeight = 0, bool visible = true) {
        if (control == nullptr) return;
        RECT bounds = source;
        OffsetRect(&bounds, 0, -ctx->panelScrollY);
        bounds.bottom += extraHeight;
        UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOCOPYBITS;
        const bool wasVisible = (GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) != 0;
        if (visible != wasVisible) flags |= visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW;
        placements.push_back({control, bounds, flags});
    };
    place(ctx->panelDevices, layout.devicesInput, 0, ctx->panelAdvancedSettings);
    place(ctx->panelPort, layout.portInput, 0, ctx->panelAdvancedSettings);
    place(ctx->panelFps, layout.fpsInput, 0, ctx->panelAdvancedSettings);
    place(ctx->panelBitrate, layout.bitrateInput, 0, ctx->panelAdvancedSettings);
    place(ctx->panelDeviceAddress, layout.deviceAddress);
    place(ctx->panelDeviceMac, layout.deviceMac);
    place(ctx->panelDiscoveryAddress, layout.discoveryAddress);
    place(ctx->panelDeviceSelector, layout.deviceSelector, PanelScale(window, 200));
    place(ctx->panelDiscovery, layout.discoveryInput, PanelScale(window, 200));
    for (const auto& [id, button] : ctx->panelButtons) place(button, PanelButtonRect(layout, id));
    // Move the complete child tree before repainting any part of it. Otherwise
    // native edits and owner-drawn buttons expose different scroll positions.
    HDWP batch = BeginDeferWindowPos(static_cast<int>(placements.size()));
    for (const auto& item : placements) {
        if (!batch) break;
        const auto& r = item.bounds;
        batch = DeferWindowPos(batch, item.control, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, item.flags);
    }
    const bool positioned = batch != nullptr && EndDeferWindowPos(batch) != FALSE;
    if (!positioned) {
        // Resource-failure fallback still suppresses intermediate paints.
        for (const auto& item : placements) {
            const auto& r = item.bounds;
            SetWindowPos(item.control, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, item.flags);
        }
    }
    // Dialog navigation follows child Z order. Keep Tab aligned with the
    // current visual layout, including controls shown by advanced settings.
    if (structureChanged) {
        std::vector<std::pair<RECT, HWND>> tabOrder;
        for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            const LONG_PTR style = GetWindowLongPtrW(child, GWL_STYLE);
            if ((style & (WS_VISIBLE | WS_TABSTOP)) != (WS_VISIBLE | WS_TABSTOP)) continue;
            RECT bounds{};
            GetWindowRect(child, &bounds);
            tabOrder.emplace_back(bounds, child);
        }
        std::sort(tabOrder.begin(), tabOrder.end(), [](const auto& a, const auto& b) {
            return a.first.top != b.first.top ? a.first.top < b.first.top : a.first.left < b.first.left;
        });
        HWND previous = HWND_TOP;
        for (const auto& item : tabOrder) {
            SetWindowPos(item.second, previous, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
            previous = item.second;
        }
    }
    ctx->panelLayoutWidth = client.right;
    ctx->panelLayoutDpi = dpi;
    ctx->panelLayoutAdvanced = ctx->panelAdvancedSettings;
    ctx->panelLayoutScrollY = ctx->panelScrollY;
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME);
}

void ScrollPanelTo(HWND window, TrayContext* ctx, long long requested)
{
    if (ctx == nullptr) return;
    RECT client{};
    GetClientRect(window, &client);
    const int next = ClampPanelScroll(requested, BuildPanelLayout(window, client).contentHeight, client.bottom);
    if (next == ctx->panelScrollY) return;
    ctx->panelScrollY = next;
    SetScrollPos(window, SB_VERT, next, TRUE);
    LayoutPanelSettingsControls(window, ctx);
}

void ApplyPanelStreamProfile(TrayContext* ctx, StreamProfile profile)
{
    if (ctx == nullptr)
        return;
    ctx->panelStreamProfile = profile;
    UINT bitrate = 30;
    if (profile == StreamProfile::LowBandwidth)
        bitrate = 15;
    else if (profile == StreamProfile::Sharp)
        bitrate = 45;
    SetPanelEditText(ctx->panelFps, L"60");
    SetPanelEditText(ctx->panelBitrate, std::to_wstring(bitrate));
}

bool SavePanelSettings(HWND window, TrayContext* ctx)
{
    if (ctx == nullptr)
        return false;
    BOOL validPort = FALSE;
    BOOL validFps = FALSE;
    BOOL validBitrate = FALSE;
    const UINT port = GetDlgItemInt(window, IDC_PANEL_PORT, &validPort, FALSE);
    const UINT fps = GetDlgItemInt(window, IDC_PANEL_FPS, &validFps, FALSE);
    const UINT bitrate = GetDlgItemInt(window, IDC_PANEL_BITRATE, &validBitrate, FALSE);
    if (!validPort || port == 0 || port > 65535 || !validFps || fps < 30 || fps > 120 || !validBitrate || bitrate < 5 ||
        bitrate > 100) {
        MessageBoxW(window, L"请填写有效的连接参数：端口 1–65535、帧率 30–120、码率 5–100 Mbps。", L"iPad互联",
                    MB_OK | MB_ICONWARNING);
        return false;
    }

    const std::vector<DeviceConfig> oldDevices = ctx->cfg.devices;
    const std::set<std::string> runningBeforeSettings = RunningAddresses(ctx);
    const uint16_t oldPort = ctx->cfg.port;
    const uint32_t oldFps = ctx->cfg.fps;
    const uint32_t oldBitrate = ctx->cfg.bitrateMbps;
    const bool oldNetworkPolicy = ctx->cfg.requirePrivateNetwork;
    const bool oldAutoReconnect = ctx->cfg.autoReconnect;
    const StreamProfile oldStreamProfile = ctx->cfg.streamProfile;

    auto editedDevices = MergeEditedDeviceEntries(oldDevices, ctx->panelDeviceBaseline,
        ctx->panelDeviceTextBaseline, GetPanelEditText(ctx->panelDevices), static_cast<uint16_t>(port));
    if (!editedDevices) {
        SyncPanelSettingsFromConfig(ctx);
        SetPanelNotice(ctx, L"设备地址已在后台变化，已刷新设备列表，请检查后重新保存");
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        return false;
    }
    ctx->cfg.devices = std::move(*editedDevices);
    ctx->cfg.port = static_cast<uint16_t>(port);
    ctx->cfg.fps = fps;
    ctx->cfg.bitrateMbps = bitrate;
    ctx->cfg.streamProfile = InferStreamProfile(fps, bitrate);
    ctx->panelStreamProfile = ctx->cfg.streamProfile;
    ctx->cfg.autoReconnect = ctx->panelAutoReconnect;
    ctx->cfg.requirePrivateNetwork = ctx->panelRequirePrivateNetwork;
    if (!ctx->cfg.Save()) {
        ctx->cfg.devices = oldDevices;
        ctx->cfg.port = oldPort;
        ctx->cfg.fps = oldFps;
        ctx->cfg.bitrateMbps = oldBitrate;
        ctx->cfg.requirePrivateNetwork = oldNetworkPolicy;
        ctx->cfg.autoReconnect = oldAutoReconnect;
        ctx->cfg.streamProfile = oldStreamProfile;
        SyncPanelSettingsFromConfig(ctx);
        MessageBoxW(window, L"配置保存失败，原配置保持不变。请检查 AppData 目录权限或磁盘空间。", L"iPad互联",
                    MB_OK | MB_ICONERROR);
        return false;
    }

    if (ctx->cfg.devices != oldDevices || ctx->cfg.port != oldPort || ctx->cfg.fps != oldFps ||
        ctx->cfg.bitrateMbps != oldBitrate || ctx->cfg.requirePrivateNetwork != oldNetworkPolicy)
        ApplyAndRestart(ctx, oldDevices, runningBeforeSettings,
                        ctx->cfg.port != oldPort || ctx->cfg.fps != oldFps ||
                        ctx->cfg.bitrateMbps != oldBitrate || ctx->cfg.requirePrivateNetwork != oldNetworkPolicy);
    SyncPanelSettingsFromConfig(ctx);
    SetPanelNotice(ctx, L"连接设置已保存");
    InvalidateRect(window, nullptr, FALSE);
    return true;
}

struct PanelLiveStatus {
    bool connected = false;
    bool running = false;
    std::wstring phase = L"未连接";
    std::wstring device = L"尚未选择 iPad";
    std::wstring transport = L"等待连接";
    std::wstring resolution = L"等待 iPad";
    std::wstring detail = L"先在 iPad 前台打开 OpenDisplay，再从下方选择设备";
    std::wstring health;
    std::wstring displayName;
};

PanelLiveStatus MakeLiveStatus(const std::wstring& device, const std::wstring& transport,
                               const ConnectionSnapshot& snapshot, bool running)
{
    PanelLiveStatus live;
    live.connected = snapshot.phase == ConnectionPhase::Streaming;
    live.running = running;
    live.phase = ConnectionPhaseTitle(snapshot.phase);
    live.device = device;
    live.transport = transport;
    if (snapshot.failure != FailureReason::None || !snapshot.detail.empty())
        live.detail = FailureHelpText(snapshot.failure, snapshot.detail);
    else {
        switch (snapshot.phase) {
            case ConnectionPhase::Connecting: live.detail = L"正在打开所选连接通道"; break;
            case ConnectionPhase::WaitingHello: live.detail = L"请让 OpenDisplay 保持前台"; break;
            case ConnectionPhase::PreparingDisplay: live.detail = L"正在创建虚拟显示器并启动 H.264 编码"; break;
            case ConnectionPhase::Reconnecting: live.detail = L"连接已中断，正在重连原通道"; break;
            default: live.detail = L"点击连接开始使用副屏"; break;
        }
    }
    live.displayName = snapshot.displayName;
    if (snapshot.width != 0 && snapshot.height != 0)
        live.resolution = std::to_wstring(snapshot.width) + L" × " + std::to_wstring(snapshot.height);
    if (live.connected) {
        const bool fluctuating = snapshot.networkDrops >= 3 || snapshot.receiverStalls > 0 ||
                                 snapshot.receiverRttMs >= 80.0;
        live.detail = fluctuating ? L"连接有波动，可尝试省带宽画质或切换 USB"
                                  : L"连接稳定，可直接在副屏工作";
        if (snapshot.receiverRttMs >= 0.0 || snapshot.actualFps >= 0.0 || snapshot.receiverMbps >= 0.0) {
            std::wostringstream health;
            if (snapshot.receiverRttMs >= 0.0)
                health << std::fixed << std::setprecision(0) << snapshot.receiverRttMs << L" ms";
            if (snapshot.receiverMbps >= 0.0)
                health << (health.tellp() > 0 ? L"  ·  " : L"") << std::setprecision(1) << snapshot.receiverMbps << L" Mbps";
            if (snapshot.actualFps >= 0.0)
                health << (health.tellp() > 0 ? L"  ·  " : L"") << std::setprecision(1) << snapshot.actualFps << L" fps";
            if (snapshot.networkDrops > 0)
                health << L"  ·  丢帧 " << snapshot.networkDrops;
            if (snapshot.receiverStalls > 0)
                health << L"  ·  卡顿 " << snapshot.receiverStalls;
            if (snapshot.captureMs >= 0.0)
                health << L"  ·  捕获 " << std::setprecision(1) << snapshot.captureMs << L" ms";
            if (snapshot.encodeMs >= 0.0)
                health << L"  ·  编码 " << std::setprecision(1) << snapshot.encodeMs << L" ms";
            live.health = health.str();
        }
    } else if (snapshot.retryInMs > 0) {
        live.detail += L"（" + std::to_wstring((snapshot.retryInMs + 999) / 1000) + L" 秒后重试）";
    }
    return live;
}

PanelLiveStatus GetPanelLiveStatus(const TrayContext* ctx)
{
    if (ctx->preview) {
        PanelLiveStatus live;
        live.phase = ctx->previewEmpty ? L"尚未添加设备" : L"可连接 · 预览示例";
        live.device = ctx->previewEmpty ? L"下方会显示局域网发现结果" : ctx->previewLong ? L"我的 iPad Pro · 这是用于验证长中文设备名称不会遮挡地址和操作按钮的预览设备" : L"我的 iPad Pro";
        live.transport = L"Wi-Fi";
        live.resolution = ctx->previewEmpty ? L"等待设备" : L"2420 × 1668";
        live.detail = L"界面预览使用示例数据，未建立连接";
        live.health = ctx->previewEmpty ? L"连接后显示实时传输状态" : L"示例 · 60 fps  /  30 Mbps";
        return live;
    }
    if (ctx->switching) {
        PanelLiveStatus live;
        live.running = true;
        live.phase = L"正在切换";
        live.device = ctx->switchingTo && *ctx->switchingTo < ctx->cfg.devices.size()
                          ? DeviceLabel(ctx, *ctx->switchingTo) : L"正在停止当前设备";
        live.transport = L"异步设备切换";
        live.detail = L"正在等待旧会话退出；请勿重复点击";
        return live;
    }
    if (ctx->usbApp) {
        const ConnectionSnapshot snapshot = ctx->usbApp->Snapshot();
        if (snapshot.phase == ConnectionPhase::Streaming || ctx->usbApp->IsRunning()) {
            PanelLiveStatus live = MakeLiveStatus(L"iPad", L"USB 有线连接", snapshot, ctx->usbApp->IsRunning());
            if (!live.connected && ctx->startupConnection.transport == ConnectionTransport::Usb &&
                !ctx->startupConnection.fallbackUsed) {
                const auto elapsed = std::chrono::steady_clock::now() - ctx->startupConnection.started;
                const auto remaining = std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::seconds>(
                                                                StartupAttemptBudget(ctx) - elapsed)
                                                                .count());
                live.detail += L"；" + std::to_wstring(remaining) + L" 秒后尝试 Wi-Fi";
            }
            return live;
        }
    }
    for (size_t i = 0; i < ctx->apps.size(); ++i) {
        const ConnectionSnapshot snapshot = ctx->apps[i]->Snapshot();
        if (snapshot.phase == ConnectionPhase::Streaming || ctx->apps[i]->IsRunning()) {
            PanelLiveStatus live = MakeLiveStatus(DeviceLabel(ctx, i), L"Wi-Fi 无线连接", snapshot,
                                                  ctx->apps[i]->IsRunning());
            if (!live.connected && ctx->startupConnection.transport == ConnectionTransport::Wifi &&
                !ctx->startupConnection.fallbackUsed) {
                const auto elapsed = std::chrono::steady_clock::now() - ctx->startupConnection.started;
                const auto remaining = std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::seconds>(
                                                                StartupAttemptBudget(ctx) - elapsed)
                                                                .count());
                live.detail += L"；" + std::to_wstring(remaining) + L" 秒后尝试 USB";
            }
            return live;
        }
    }
    if (!ctx->cfg.devices.empty()) {
        PanelLiveStatus live;
        live.device = DeviceLabel(ctx, 0);
        live.detail = ctx->autoConnectMessage.empty() ? L"请在 iPad 前台打开 OpenDisplay，然后点击“连接”"
                                                      : ctx->autoConnectMessage;
        return live;
    }
    if (!ctx->usbDevices.empty()) {
        PanelLiveStatus live;
        live.device = L"已发现 USB iPad";
        live.transport = L"USB 可用";
        live.detail = L"请解锁 iPad、信任此电脑，并在前台打开 OpenDisplay";
        return live;
    }
    {
        std::lock_guard<std::mutex> lock(ctx->discoveredMutex);
        if (!ctx->discovered.empty()) {
            PanelLiveStatus live;
            live.device = Widen(ctx->discovered.begin()->second);
            live.transport = L"局域网已发现";
            live.detail = L"在下方选择该设备并添加，然后点击“连接”";
            return live;
        }
    }
    return {};
}

void DrawPanelCombo(HWND window, TrayContext* ctx, const DRAWITEMSTRUCT& item)
{
    const int savedDc = SaveDC(item.hDC);
    const PanelPalette colors = ControlPanelPalette(ctx->cfg.darkTheme);
    const bool disabled = (item.itemState & ODS_DISABLED) != 0 || !IsWindowEnabled(item.hwndItem);
    const bool selected = (item.itemState & ODS_SELECTED) != 0 && !disabled;
    HBRUSH background = CreateSolidBrush(selected ? colors.primarySoft : colors.surfaceSoft);
    FillRect(item.hDC, &item.rcItem, background);
    DeleteObject(background);
    std::wstring label;
    if (item.itemID != static_cast<UINT>(-1)) {
        const LRESULT length = SendMessageW(item.hwndItem, CB_GETLBTEXTLEN, item.itemID, 0);
        if (length >= 0 && length <= 65536) {
            std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1, L'\0');
            if (SendMessageW(item.hwndItem, CB_GETLBTEXT, item.itemID, reinterpret_cast<LPARAM>(buffer.data())) != CB_ERR)
                label.assign(buffer.data());
        }
    }
    RECT text = item.rcItem;
    text.left += PanelScale(window, 10);
    text.right -= PanelScale(window, 10);
    const HFONT font = ctx->panelFont != nullptr ? ctx->panelFont : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    DrawPanelText(item.hDC, text, label, font, disabled ? colors.muted : colors.text,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if ((item.itemState & ODS_FOCUS) != 0 && (item.itemState & ODS_NOFOCUSRECT) == 0) {
        RECT focus = item.rcItem;
        InflateRect(&focus, -PanelScale(window, 2), -PanelScale(window, 2));
        DrawFocusRect(item.hDC, &focus);
    }
    RestoreDC(item.hDC, savedDc);
}

void DrawPanelButton(HWND window, TrayContext* ctx, const DRAWITEMSTRUCT& item)
{
    const PanelPalette colors = ControlPanelPalette(ctx->cfg.darkTheme);
    const int id = static_cast<int>(item.CtlID);
    const bool toggle = id == IDC_PANEL_AUTO_RECONNECT || id == IDC_PANEL_PRIVATE_NETWORK;
    const bool checked = (id == IDC_PANEL_AUTO_RECONNECT && ctx->panelAutoReconnect) ||
                         (id == IDC_PANEL_PRIVATE_NETWORK && ctx->panelRequirePrivateNetwork);
    const bool profile = id >= IDC_PANEL_PROFILE_BALANCED && id <= IDC_PANEL_PROFILE_SHARP;
    const StreamProfile selectedProfile = id == IDC_PANEL_PROFILE_LOW ? StreamProfile::LowBandwidth :
                                          id == IDC_PANEL_PROFILE_SHARP ? StreamProfile::Sharp : StreamProfile::Balanced;
    const bool selected = profile && ctx->panelStreamProfile == selectedProfile;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool primary = !disabled && (id == IDC_PANEL_DEVICE_CONNECT || id == IDC_PANEL_SAVE);
    const COLORREF fill = primary ? colors.primary : selected ? colors.primarySoft : colors.surfaceSoft;
    HBRUSH background = CreateSolidBrush(colors.surface);
    FillRect(item.hDC, &item.rcItem, background);
    DeleteObject(background);
    RECT bounds = item.rcItem;
    bounds.right -= 1; bounds.bottom -= 1;
    if (!toggle) {
        FillRoundedRect(item.hDC, bounds, PanelScale(window, 10), fill);
        StrokeRoundedRect(item.hDC, bounds, PanelScale(window, 10), selected || primary ? colors.primary : colors.stroke);
    }
    RECT text = bounds;
    text.left += PanelScale(window, 8); text.right -= PanelScale(window, 8);
    if (toggle) {
        RECT box{bounds.left, bounds.top + PanelScale(window, 4), bounds.left + PanelScale(window, 20), bounds.top + PanelScale(window, 24)};
        FillRoundedRect(item.hDC, box, PanelScale(window, 5), checked ? colors.primary : colors.surfaceSoft);
        StrokeRoundedRect(item.hDC, box, PanelScale(window, 5), checked ? colors.primary : colors.stroke);
        if (checked) DrawPanelText(item.hDC, box, L"✓", ctx->panelFont, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        text.left = box.right + PanelScale(window, 9);
    }
    const auto label = GetPanelEditText(item.hwndItem);
    DrawPanelText(item.hDC, text, label, ctx->panelFont, disabled ? colors.muted : primary ? RGB(255, 255, 255) : colors.text,
                  (toggle ? DT_LEFT : DT_CENTER) | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if ((item.itemState & ODS_FOCUS) != 0 && (item.itemState & ODS_NOFOCUSRECT) == 0) {
        RECT focus = bounds;
        InflateRect(&focus, -PanelScale(window, 3), -PanelScale(window, 3));
        DrawFocusRect(item.hDC, &focus);
    }
}

void DrawControlPanel(HWND window, TrayContext* ctx)
{
    PAINTSTRUCT paint{};
    HDC screenDc = BeginPaint(window, &paint);
    RECT client{}; GetClientRect(window, &client);
    const int width = (std::max)(1L, client.right), height = (std::max)(1L, client.bottom);
    HDC dc = CreateCompatibleDC(screenDc);
    HBITMAP bitmap = CreateCompatibleBitmap(screenDc, width, height);
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    const PanelPalette colors = ControlPanelPalette(ctx->cfg.darkTheme);
    const PanelLayout p = BuildPanelLayout(window, client);
    HBRUSH background = CreateSolidBrush(colors.canvas);
    FillRect(dc, &client, background); DeleteObject(background);
    SetViewportOrgEx(dc, 0, -ctx->panelScrollY, nullptr);
    HFONT titleFont = CreatePanelFont(window, 21, FW_SEMIBOLD);
    HFONT headingFont = CreatePanelFont(window, 11, FW_SEMIBOLD);
    HFONT valueFont = CreatePanelFont(window, 17, FW_SEMIBOLD);
    const UINT single = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
    const auto text = [&](const RECT& rect, const std::wstring& value, HFONT font, COLORREF color) {
        DrawPanelText(dc, rect, value, font, color, single);
    };
    const auto card = [&](const RECT& rect) {
        FillRoundedRect(dc, rect, PanelScale(window, 14), colors.surface);
        StrokeRoundedRect(dc, rect, PanelScale(window, 14), colors.stroke);
    };
    text(p.title, ctx->preview ? L"iPad互联 · 界面预览" : L"iPad互联", titleFont, colors.text);
    text(p.subtitle, L"让 iPad 成为 Windows 扩展屏", ctx->panelFont, colors.muted);
    card(p.statusCard); card(p.deviceCard); card(p.settingsCard); card(p.qualityCard);
    const PanelLiveStatus live = GetPanelLiveStatus(ctx);
    text(p.statusTitle, L"副屏状态", headingFont, colors.muted);
    text(p.statusValue, live.phase, valueFont, live.connected ? colors.primary : colors.text);
    text(p.statusDevice, live.device, ctx->panelFont, colors.text);
    text(p.statusResolution, live.transport + L"  ·  " + live.resolution, ctx->panelFont, colors.muted);
    text(p.statusDetail, live.detail, ctx->panelFont, colors.muted);
    text(p.deviceTitle, L"我的设备", headingFont, colors.text);
    text(p.deviceAddressLabel, L"当前 IP 地址（可复制）", ctx->panelFont, colors.muted);
    text(p.deviceMacLabel, L"MAC 地址（辅助识别）", ctx->panelFont, colors.muted);
    text(p.settingsTitle, L"连接设置", headingFont, colors.text);
    text(p.discoveryLabel, L"局域网发现 · 请让 OpenDisplay 保持前台", ctx->panelFont, colors.muted);
    text(p.discoveryAddressLabel, L"所选发现设备的 IP 地址（可复制）", ctx->panelFont, colors.muted);
    if (ctx->panelAdvancedSettings) {
        text(p.devicesLabel, L"手动地址（每行：IP 地址 + 可选名称）", ctx->panelFont, colors.muted);
        text(p.portLabel, L"端口", ctx->panelFont, colors.muted);
        text(p.fpsLabel, L"帧率", ctx->panelFont, colors.muted);
        text(p.bitrateLabel, L"码率 Mbps", ctx->panelFont, colors.muted);
    }
    text(p.qualityTitle, L"画质与显示", headingFont, colors.text);
    text(p.qualityHint, L"画质预设 · 60 fps", ctx->panelFont, colors.muted);
    text(p.health, live.health.empty() ? L"连接后显示实时传输状态" : live.health, ctx->panelFont, colors.muted);
    DrawPanelText(dc, p.displayHint, L"桌面图标、任务栏和窗口由 Windows 原生桌面显示。", ctx->panelFont, colors.muted, DT_LEFT | DT_WORDBREAK);
    const bool showNotice = !ctx->panelNotice.empty() && std::chrono::steady_clock::now() < ctx->panelNoticeUntil;
    DrawPanelText(dc, p.notice, showNotice ? ctx->panelNotice : ctx->preview ? L"这是隔离的界面预览。示例设备不会连接，设置不会保存。" : L"设备与地址分开显示，可在普通窗口中查看和复制。", ctx->panelFont, colors.muted, DT_LEFT | DT_WORDBREAK);
    text(p.footer, L"iPad互联 · 本地直连", ctx->panelFont, colors.muted);
    DeleteObject(titleFont); DeleteObject(headingFont); DeleteObject(valueFont);
    SetViewportOrgEx(dc, 0, 0, nullptr);
    BitBlt(screenDc, 0, 0, width, height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBitmap); DeleteObject(bitmap); DeleteDC(dc); EndPaint(window, &paint);
}

std::string RedactDiagnostics(std::string text)
{
    wchar_t* profileRaw = nullptr;
    size_t profileLength = 0;
    if (_wdupenv_s(&profileRaw, &profileLength, L"USERPROFILE") == 0 && profileRaw != nullptr) {
        const std::string profile = Narrow(profileRaw);
        free(profileRaw);
        for (size_t pos = text.find(profile); !profile.empty() && pos != std::string::npos; pos = text.find(profile, pos + 8))
            text.replace(pos, profile.size(), "<profile>");
    }

    return RedactDiagnosticsText(std::move(text));
}

std::wstring ExportDiagnostics(TrayContext* ctx)
{
    if (ctx == nullptr)
        return {};
    std::error_code error;
    const std::filesystem::path appDir = std::filesystem::path(Config::FilePath()).parent_path();
    const std::filesystem::path diagnosticsDir = appDir / L"diagnostics";
    std::filesystem::create_directories(diagnosticsDir, error);
    if (error)
        return {};

    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::wostringstream filename;
    filename << L"iPad互联-diagnostics-" << std::setfill(L'0') << std::setw(4) << now.wYear << std::setw(2) << now.wMonth
             << std::setw(2) << now.wDay << L'-' << std::setw(2) << now.wHour << std::setw(2) << now.wMinute
             << std::setw(2) << now.wSecond << L".txt";
    const std::filesystem::path outputPath = diagnosticsDir / filename.str();

    std::ostringstream report;
    report << "iPad Connect diagnostics\nversion=0.1.2\nprotocol=OpenDisplay v3 (unchanged)\n";
    report << "autoReconnect=" << (ctx->cfg.autoReconnect ? "true" : "false")
           << " requirePrivateNetwork=" << (ctx->cfg.requirePrivateNetwork ? "true" : "false")
           << " fps=" << ctx->cfg.fps << " bitrateMbps=" << ctx->cfg.bitrateMbps << "\n";
    size_t discoveredCount = 0;
    {
        std::lock_guard<std::mutex> lock(ctx->discoveredMutex);
        discoveredCount = ctx->discovered.size();
    }
    report << "configuredDevices=" << ctx->cfg.devices.size() << " discoveredDevices=" << discoveredCount
           << " usbDevices=" << ctx->usbDevices.size() << "\n\n";

    size_t senderIndex = 0;
    auto appendSnapshot = [&](const char* transport, const ConnectionSnapshot& snapshot, bool running) {
        report << "sender[" << senderIndex++ << "] transport=" << transport << " running=" << (running ? "true" : "false")
               << " phase=" << Narrow(ConnectionPhaseTitle(snapshot.phase)) << " failure=" << static_cast<int>(snapshot.failure)
               << " resolution=" << snapshot.width << 'x' << snapshot.height << " retryMs=" << snapshot.retryInMs
               << " drops=" << snapshot.networkDrops << " rttMs=" << snapshot.receiverRttMs
               << " mbps=" << snapshot.receiverMbps << " stalls=" << snapshot.receiverStalls
               << " captureMs=" << snapshot.captureMs << " encodeMs=" << snapshot.encodeMs << "\n";
        const auto& metrics = snapshot.pipeline;
        report << "  encoder=" << Narrow(metrics.encoderName) << " hardware=" << metrics.hardware << " color709=" << metrics.colorVerified
               << " waitMs=" << metrics.waitMs << " readbackMs=" << metrics.readbackMs << " cursorMs=" << metrics.cursorMs
               << " conversionMs=" << metrics.conversionMs << " copyMs=" << metrics.bufferCopyMs << " outputDelayMs=" << metrics.outputDelayMs
               << " sendMs=" << metrics.sendMs << " outputFps=" << metrics.outputFps << " outputFrames=" << metrics.outputFrames
               << " sentFrames=" << metrics.sentFrames << "\n";
        if (!snapshot.detail.empty())
            report << "  detail=" << snapshot.detail << "\n";
    };
    for (const auto& app : ctx->apps)
        appendSnapshot("wifi", app->Snapshot(), app->IsRunning());
    if (ctx->usbApp)
        appendSnapshot("usb", ctx->usbApp->Snapshot(), ctx->usbApp->IsRunning());

    report << "\nattachedDisplays:\n";
    for (const DisplayInfo& display : EnumerateAttachedDisplays()) {
        report << "  " << Narrow(display.deviceName) << ' ' << display.bounds.left << ',' << display.bounds.top << ' '
               << display.bounds.right - display.bounds.left << 'x' << display.bounds.bottom - display.bounds.top
               << (display.primary ? " primary" : "") << "\n";
    }
    report << "\nnetworkPolicy:\n";
    for (size_t i = 0; i < ctx->cfg.devices.size(); ++i) {
        const std::string address = ResolveDeviceTarget(ctx, i);
        const NetworkSafetyResult safety = CheckTrustedLan(address, ctx->cfg.requirePrivateNetwork);
        report << "  device[" << i << "] allowed=" << (safety.allowed ? "true" : "false")
               << " reason=" << safety.reason << "\n";
    }

    const std::filesystem::path logPath = appDir / L"log.txt";
    std::ifstream log(logPath, std::ios::binary);
    if (log) {
        std::stringstream buffer;
        buffer << log.rdbuf();
        std::string logText = buffer.str();
        constexpr size_t kMaxLogBytes = 64 * 1024;
        if (logText.size() > kMaxLogBytes)
            logText.erase(0, logText.size() - kMaxLogBytes);
        report << "\nrecentLog(redacted):\n" << logText;
    }

    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output)
        return {};
    output << RedactDiagnostics(report.str());
    output.close();
    return outputPath.wstring();
}

bool AddSelectedDiscoveredDevice(TrayContext* ctx)
{
    if (ctx == nullptr || ctx->panelDiscovery == nullptr || ctx->panelDiscoveryAddresses.empty())
        return false;
    const int selected = static_cast<int>(SendMessageW(ctx->panelDiscovery, CB_GETCURSEL, 0, 0));
    if (selected < 0 || selected >= static_cast<int>(ctx->panelDiscoveryAddresses.size()))
        return false;
    const std::string address = ctx->panelDiscoveryAddresses[static_cast<size_t>(selected)];
    const bool exists = std::any_of(ctx->cfg.devices.begin(), ctx->cfg.devices.end(), [&](const DeviceConfig& entry) {
        return SplitDevice(entry).address == address;
    });
    if (exists) {
        SetPanelNotice(ctx, L"这台 iPad 已在设备列表中");
        return true;
    }
    std::string name;
    std::string id;
    std::string host;
    uint16_t discoveredPort = ctx->cfg.port;
    {
        std::lock_guard<std::mutex> lock(ctx->discoveredMutex);
        auto found = ctx->discovered.find(address);
        if (found != ctx->discovered.end())
            name = found->second;
        for (const DiscoveryRecord& record : ctx->discoveryCache.Records()) {
            if (record.address == address) {
                id = record.id;
                host = record.host;
                if (record.port != 0) discoveredPort = record.port;
                break;
            }
        }
    }
    if (!id.empty()) {
        auto stable = std::find_if(ctx->cfg.devices.begin(), ctx->cfg.devices.end(), [&](const DeviceConfig& device) {
            return device.id == id;
        });
        if (stable != ctx->cfg.devices.end()) {
            stable->lastIpv4 = address;
            stable->bonjourHost = host;
            stable->port = discoveredPort;
            if (!name.empty()) stable->name = name;
            ctx->cfg.Save();
            SyncPanelSettingsFromConfig(ctx);
            SetPanelNotice(ctx, L"已刷新该设备的当前地址");
            return true;
        }
    }
    DeviceConfig device;
    device.id = std::move(id);
    device.name = name.empty() ? address : std::move(name);
    device.lastIpv4 = address;
    device.bonjourHost = std::move(host);
    device.port = discoveredPort;
    device.priority = static_cast<uint32_t>(ctx->cfg.devices.size());
    ctx->cfg.devices.push_back(std::move(device));
    ctx->cfg.Save();
    ctx->apps.push_back(std::make_unique<SenderApp>());
    SyncPanelSettingsFromConfig(ctx);
    SetPanelNotice(ctx, L"已添加局域网 iPad，点击“连接”即可使用");
    return true;
}

std::wstring ActiveDisplayName(const TrayContext* ctx)
{
    if (ctx->usbApp) {
        const auto snapshot = ctx->usbApp->Snapshot();
        if (snapshot.phase == ConnectionPhase::Streaming)
            return snapshot.displayName;
    }
    for (const auto& app : ctx->apps) {
        const auto snapshot = app->Snapshot();
        if (snapshot.phase == ConnectionPhase::Streaming)
            return snapshot.displayName;
    }
    return {};
}

bool PlaceActiveDisplayRightOfPrimary(HWND owner, TrayContext* ctx)
{
    const std::wstring displayName = ActiveDisplayName(ctx);
    if (displayName.empty()) {
        MessageBoxW(owner, L"请先连接 iPad，成功创建副屏后再调整位置。", L"iPad互联", MB_OK | MB_ICONINFORMATION);
        return false;
    }
    std::optional<DisplayInfo> primary;
    for (const DisplayInfo& display : EnumerateAttachedDisplays())
        if (display.primary)
            primary = display;
    if (!primary)
        return false;

    DEVMODEW original{};
    original.dmSize = sizeof(original);
    if (!EnumDisplaySettingsExW(displayName.c_str(), ENUM_CURRENT_SETTINGS, &original, 0))
        return false;
    const LONG targetX = primary->bounds.right;
    const LONG targetY = primary->bounds.top;
    std::wostringstream prompt;
    prompt << L"将 " << displayName << L" 从 (" << original.dmPosition.x << L", " << original.dmPosition.y << L") 移到主屏右侧 ("
           << targetX << L", " << targetY << L")。\n\n是否继续？";
    if (MessageBoxW(owner, prompt.str().c_str(), L"调整副屏位置", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return false;

    DEVMODEW proposed = original;
    proposed.dmFields = DM_POSITION;
    proposed.dmPosition.x = targetX;
    proposed.dmPosition.y = targetY;
    const LONG stage = ChangeDisplaySettingsExW(displayName.c_str(), &proposed, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
    const LONG applied = stage == DISP_CHANGE_SUCCESSFUL ? ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr) : stage;
    if (applied != DISP_CHANGE_SUCCESSFUL) {
        original.dmFields = DM_POSITION;
        ChangeDisplaySettingsExW(displayName.c_str(), &original, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
        ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
        MessageBoxW(owner, L"Windows 未能应用新位置，已尝试恢复原布局。", L"iPad互联", MB_OK | MB_ICONWARNING);
        return false;
    }
    return true;
}

void TogglePanelConnection(TrayContext* ctx)
{
    if (AnyRunning(ctx)) {
        QueueDeviceCommand(ctx, TrayContext::CommandType::Stop);
        SetPanelNotice(ctx, L"正在异步断开副屏连接…");
    } else {
        BeginStartupAutoConnect(ctx);
        if (ctx->startupConnection.transport == ConnectionTransport::None)
            SetPanelNotice(ctx, L"未找到可连接设备；请先添加局域网 iPad 或连接 USB");
        else
            SetPanelNotice(ctx, L"正在按上次成功通道连接");
    }
    UpdateStatus(ctx);
}

void RetryOrSwitchPanelConnection(TrayContext* ctx)
{
    if (ctx->cfg.devices.size() > 1 && ctx->panelDeviceSelector != nullptr) {
        const int selected = static_cast<int>(SendMessageW(ctx->panelDeviceSelector, CB_GETCURSEL, 0, 0));
        if (selected >= 0) {
            QueueDeviceCommand(ctx, TrayContext::CommandType::Switch, static_cast<size_t>(selected));
            SetPanelNotice(ctx, L"正在切换到所选设备…");
            return;
        }
    }
    const bool usbActive = ctx->usbApp && ctx->usbApp->IsRunning();
    const bool wifiActive = std::any_of(ctx->apps.begin(), ctx->apps.end(), [](const auto& app) { return app->IsRunning(); });
    ctx->startupConnection.transport = ConnectionTransport::None;
    if (usbActive && !ctx->cfg.devices.empty()) {
        StartWifiSession(ctx);
        SetPanelNotice(ctx, L"正在切换到 Wi-Fi");
    } else if (wifiActive) {
        RefreshUsbDevices(ctx, true);
        if (auto target = PreferredUsbTarget(ctx)) {
            StartUsbSession(ctx, std::move(*target));
            SetPanelNotice(ctx, L"正在切换到 USB");
        } else {
            size_t activeIndex = 0;
            for (size_t i = 0; i < ctx->apps.size(); ++i) if (ctx->apps[i]->IsRunning()) { activeIndex = i; break; }
            QueueDeviceCommand(ctx, TrayContext::CommandType::Retry, activeIndex);
            SetPanelNotice(ctx, L"正在重新连接 Wi-Fi");
        }
    } else if (usbActive) {
        const std::string target = ctx->activeUsbTarget;
        StopUsbSession(ctx);
        StartUsbSession(ctx, target);
        SetPanelNotice(ctx, L"正在重新连接 USB");
    } else {
        BeginStartupAutoConnect(ctx);
        SetPanelNotice(ctx, L"正在重新尝试连接");
    }
    UpdateStatus(ctx);
}

void ApplyControlPanelTheme(HWND window, bool dark)
{
    BOOL enabled = dark ? TRUE : FALSE;
    constexpr DWORD kUseImmersiveDarkMode = 20;
    DwmSetWindowAttribute(window, kUseImmersiveDarkMode, &enabled, sizeof(enabled));
}

void RefreshControlPanel(TrayContext* ctx)
{
    if (ctx == nullptr || ctx->controlPanel == nullptr || !IsWindow(ctx->controlPanel)) return;
    RECT client{}; GetClientRect(ctx->controlPanel, &client);
    const auto p = BuildPanelLayout(ctx->controlPanel, client);
    for (RECT area : {p.statusCard, p.health, p.notice}) {
        OffsetRect(&area, 0, -ctx->panelScrollY);
        InvalidateRect(ctx->controlPanel, &area, FALSE);
    }
    SetPanelTextIfChanged(GetDlgItem(ctx->controlPanel, IDC_PANEL_PRIMARY), AnyRunning(ctx) ? L"断开连接" : L"自动连接");
}

bool HandlePanelAction(HWND window, TrayContext* ctx, int id)
{
    if (ctx->preview && id != IDC_PANEL_THEME && id != IDC_PANEL_ADVANCED &&
        id != IDC_PANEL_AUTO_RECONNECT && id != IDC_PANEL_PRIVATE_NETWORK &&
        !(id >= IDC_PANEL_PROFILE_BALANCED && id <= IDC_PANEL_PROFILE_SHARP)) {
        SetPanelNotice(ctx, L"界面预览：此操作不会连接设备或保存设置");
        return true;
    }
    switch (id) {
        case IDC_PANEL_THEME:
            ctx->cfg.darkTheme = !ctx->cfg.darkTheme;
            if (!ctx->preview) ctx->cfg.Save();
            ApplyControlPanelTheme(window, ctx->cfg.darkTheme);
            RefreshPanelEditBrush(ctx);
            LayoutPanelSettingsControls(window, ctx);
            break;
        case IDC_PANEL_PRIMARY: TogglePanelConnection(ctx); break;
        case IDC_PANEL_SECONDARY: RetryOrSwitchPanelConnection(ctx); break;
        case IDC_PANEL_DISPLAY_SETTINGS:
            ShellExecuteW(window, L"open", L"ms-settings:display", nullptr, nullptr, SW_SHOWNORMAL);
            SetPanelNotice(ctx, L"已打开 Windows 显示设置");
            break;
        case IDC_PANEL_PLACE_RIGHT:
            if (PlaceActiveDisplayRightOfPrimary(window, ctx)) SetPanelNotice(ctx, L"已将 iPad 副屏放到主屏右侧");
            break;
        case IDC_PANEL_DIAGNOSTICS: {
            const std::wstring path = ExportDiagnostics(ctx);
            if (path.empty()) MessageBoxW(window, L"无法写入诊断报告。", L"iPad互联", MB_OK | MB_ICONWARNING);
            else {
                SetPanelNotice(ctx, L"诊断报告已导出（敏感信息已遮蔽）");
                const std::filesystem::path folder = std::filesystem::path(path).parent_path();
                ShellExecuteW(window, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            break;
        }
        case IDC_PANEL_ADVANCED:
            ctx->panelAdvancedSettings = !ctx->panelAdvancedSettings;
            UpdatePanelScroll(window, ctx);
            LayoutPanelSettingsControls(window, ctx);
            break;
        case IDC_PANEL_ADD_DISCOVERED:
            if (!AddSelectedDiscoveredDevice(ctx)) SetPanelNotice(ctx, L"暂未发现可添加的 iPad；可在高级设置中手动填写 IP");
            SyncPanelDeviceSelector(ctx);
            break;
        case IDC_PANEL_PROFILE_BALANCED: ApplyPanelStreamProfile(ctx, StreamProfile::Balanced); break;
        case IDC_PANEL_PROFILE_LOW: ApplyPanelStreamProfile(ctx, StreamProfile::LowBandwidth); break;
        case IDC_PANEL_PROFILE_SHARP: ApplyPanelStreamProfile(ctx, StreamProfile::Sharp); break;
        case IDC_PANEL_AUTO_RECONNECT: ctx->panelAutoReconnect = !ctx->panelAutoReconnect; break;
        case IDC_PANEL_PRIVATE_NETWORK: ctx->panelRequirePrivateNetwork = !ctx->panelRequirePrivateNetwork; break;
        case IDC_PANEL_SAVE: SavePanelSettings(window, ctx); break;
        default: return false;
    }
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    return true;
}

void EnsurePanelFocusVisible(HWND window, TrayContext* ctx)
{
    HWND focus = GetFocus();
    if (focus == nullptr || !IsChild(window, focus)) return;
    RECT bounds{}, client{};
    GetWindowRect(focus, &bounds);
    MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&bounds), 2);
    GetClientRect(window, &client);
    const int padding = PanelScale(window, 12);
    int delta = 0;
    if (bounds.top < padding) delta = bounds.top - padding;
    else if (bounds.bottom > client.bottom - padding) delta = bounds.bottom - client.bottom + padding;
    if (delta == 0) return;
    ScrollPanelTo(window, ctx, static_cast<long long>(ctx->panelScrollY) + delta);
}

LRESULT CALLBACK ControlPanelWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* ctx = reinterpret_cast<TrayContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        ctx = reinterpret_cast<TrayContext*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        if (ctx != nullptr)
            ctx->controlPanel = window;
    }

    switch (message) {
        case WM_CREATE:
            if (ctx != nullptr) {
                ApplyControlPanelTheme(window, ctx->cfg.darkTheme);
                CreatePanelSettingsControls(window, ctx);
                UpdatePanelScroll(window, ctx);
                LayoutPanelSettingsControls(window, ctx);
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            if (ctx != nullptr)
                DrawControlPanel(window, ctx);
            return 0;
        case WM_MEASUREITEM:
            if (ctx != nullptr && lParam != 0) {
                auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
                if (item->CtlType == ODT_COMBOBOX &&
                    (item->CtlID == IDC_PANEL_DEVICE_SELECT || item->CtlID == IDC_PANEL_DISCOVERY)) {
                    item->itemHeight = static_cast<UINT>(PanelScale(window, 30));
                    return TRUE;
                }
            }
            break;
        case WM_DRAWITEM:
            if (ctx != nullptr && lParam != 0) {
                const auto& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
                if (item.CtlType == ODT_COMBOBOX) {
                    DrawPanelCombo(window, ctx, item);
                    return TRUE;
                }
                if (item.CtlType == ODT_BUTTON) {
                    DrawPanelButton(window, ctx, item);
                    return TRUE;
                }
            }
            break;
        case WM_COMMAND:
            if (ctx == nullptr) break;
            if (LOWORD(wParam) == IDOK) {
                HWND focus = GetFocus();
                wchar_t className[32]{};
                if (focus != nullptr && IsChild(window, focus) && GetClassNameW(focus, className, 32) != 0 && wcscmp(className, L"Button") == 0)
                    SendMessageW(focus, BM_CLICK, 0, 0);
                return 0;
            }
            if (HIWORD(wParam) == CBN_SELCHANGE && (LOWORD(wParam) == IDC_PANEL_DISCOVERY || LOWORD(wParam) == IDC_PANEL_DEVICE_SELECT)) {
                RefreshPanelAddressDetails(ctx);
                if (LOWORD(wParam) == IDC_PANEL_DEVICE_SELECT) SyncPanelDeviceSelector(ctx);
                return 0;
            }
            if (ctx != nullptr && LOWORD(wParam) == IDCANCEL) {
                SyncPanelSettingsFromConfig(ctx);
                RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
                SetPanelNotice(ctx, L"未应用的设置已取消");
                return 0;
            }
            if (HIWORD(wParam) == BN_CLICKED && HandlePanelAction(window, ctx, LOWORD(wParam))) return 0;
            if (ctx != nullptr && HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == IDC_PANEL_DEVICE_CONNECT) {
                const int selected = static_cast<int>(SendMessageW(ctx->panelDeviceSelector, CB_GETCURSEL, 0, 0));
                if (selected >= 0) QueueDeviceCommand(ctx, TrayContext::CommandType::Switch, static_cast<size_t>(selected));
                return 0;
            }
            if (ctx != nullptr && HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == IDC_PANEL_DEVICE_DEFAULT) {
                const int selected = static_cast<int>(SendMessageW(ctx->panelDeviceSelector, CB_GETCURSEL, 0, 0));
                if (selected >= 0 && static_cast<size_t>(selected) < ctx->cfg.devices.size()) {
                    const DeviceConfig& device = ctx->cfg.devices[static_cast<size_t>(selected)];
                    if (device.id.empty()) SetPanelNotice(ctx, L"请先通过 mDNS 添加或连接一次，以取得稳定设备 ID");
                    else {
                        ctx->cfg.preferredDeviceId = device.id;
                        ctx->cfg.Save();
                        SetPanelNotice(ctx, L"已记住为默认设备");
                    }
                }
                return 0;
            }
            if (ctx != nullptr && HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == IDC_PANEL_DEVICE_UP) {
                const int selected = static_cast<int>(SendMessageW(ctx->panelDeviceSelector, CB_GETCURSEL, 0, 0));
                if (selected > 0 && static_cast<size_t>(selected) < ctx->cfg.devices.size()) {
                    std::swap(ctx->cfg.devices[static_cast<size_t>(selected)], ctx->cfg.devices[static_cast<size_t>(selected - 1)]);
                    std::swap(ctx->apps[static_cast<size_t>(selected)], ctx->apps[static_cast<size_t>(selected - 1)]);
                    for (size_t i = 0; i < ctx->cfg.devices.size(); ++i) ctx->cfg.devices[i].priority = static_cast<uint32_t>(i);
                    ctx->cfg.Save();
                    SyncPanelSettingsFromConfig(ctx);
                    SyncPanelDeviceSelector(ctx);
                    SendMessageW(ctx->panelDeviceSelector, CB_SETCURSEL, selected - 1, 0);
                    SetPanelNotice(ctx, L"设备优先级已上移");
                }
                return 0;
            }
            if (ctx != nullptr && HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == IDC_PANEL_DEVICE_REMOVE) {
                const int selected = static_cast<int>(SendMessageW(ctx->panelDeviceSelector, CB_GETCURSEL, 0, 0));
                if (selected >= 0 && static_cast<size_t>(selected) < ctx->cfg.devices.size() &&
                    MessageBoxW(window, L"从配置中移除所选设备？正在连接时会异步断开该设备。", L"iPad互联",
                                MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    const size_t index = static_cast<size_t>(selected);
                    const std::string removedId = ctx->cfg.devices[index].id;
                    ctx->apps[index]->RequestStop();
                    ctx->retiredApps.push_back(std::move(ctx->apps[index]));
                    ctx->apps.erase(ctx->apps.begin() + static_cast<std::ptrdiff_t>(index));
                    ctx->cfg.devices.erase(ctx->cfg.devices.begin() + static_cast<std::ptrdiff_t>(index));
                    for (size_t i = 0; i < ctx->cfg.devices.size(); ++i) ctx->cfg.devices[i].priority = static_cast<uint32_t>(i);
                    if (!removedId.empty() && ctx->cfg.preferredDeviceId == removedId) ctx->cfg.preferredDeviceId.clear();
                    ctx->cfg.Save();
                    SyncPanelSettingsFromConfig(ctx);
                    SyncPanelDeviceSelector(ctx);
                    SetPanelNotice(ctx, L"设备已移除");
                }
                return 0;
            }
            break;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE && ctx != nullptr) {
                SyncPanelSettingsFromConfig(ctx);
                RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
                SetPanelNotice(ctx, L"未应用的设置已取消");
                return 0;
            }
            break;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = PanelScale(window, 620);
            info->ptMinTrackSize.y = PanelScale(window, 480);
            MONITORINFO monitor{sizeof(monitor)};
            if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) {
                info->ptMinTrackSize.x = std::min(info->ptMinTrackSize.x, monitor.rcWork.right - monitor.rcWork.left);
                info->ptMinTrackSize.y = std::min(info->ptMinTrackSize.y, monitor.rcWork.bottom - monitor.rcWork.top);
            }
            return 0;
        }
        case WM_SIZE:
            UpdatePanelScroll(window, ctx);
            LayoutPanelSettingsControls(window, ctx);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_VSCROLL:
            if (ctx != nullptr) {
                SCROLLINFO info{sizeof(info), SIF_ALL};
                GetScrollInfo(window, SB_VERT, &info);
                long long position = ctx->panelScrollY;
                switch (LOWORD(wParam)) {
                    case SB_LINEUP: position -= PanelScale(window, 32); break;
                    case SB_LINEDOWN: position += PanelScale(window, 32); break;
                    case SB_PAGEUP: position -= static_cast<int>(info.nPage); break;
                    case SB_PAGEDOWN: position += static_cast<int>(info.nPage); break;
                    case SB_THUMBTRACK:
                    case SB_THUMBPOSITION: position = info.nTrackPos; break;
                    case SB_TOP: position = 0; break;
                    case SB_BOTTOM: position = info.nMax; break;
                    default: return 0;
                }
                ScrollPanelTo(window, ctx, position);
            }
            return 0;
        case WM_DPICHANGED: {
            if (ctx != nullptr) UpdatePanelFonts(window, ctx);
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                          suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            UpdatePanelScroll(window, ctx);
            LayoutPanelSettingsControls(window, ctx);
            return 0;
        }
        case WM_MOUSEWHEEL:
            if (ctx != nullptr) {
                UINT lines = 3;
                SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
                RECT client{};
                GetClientRect(window, &client);
                const int next = PanelWheelScroll(ctx->panelScrollY, GET_WHEEL_DELTA_WPARAM(wParam), lines,
                    PanelScale(window, 32), client.bottom, BuildPanelLayout(window, client).contentHeight, ctx->panelWheelRemainder);
                ScrollPanelTo(window, ctx, next);
            }
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLOREDIT:
            if (ctx != nullptr && ctx->panelEditBrush != nullptr) {
                const PanelPalette colors = ControlPanelPalette(ctx->cfg.darkTheme);
                HDC dc = reinterpret_cast<HDC>(wParam);
                SetTextColor(dc, colors.text);
                SetBkColor(dc, colors.surfaceSoft);
                return reinterpret_cast<LRESULT>(ctx->panelEditBrush);
            }
            break;
        case WM_CLOSE:
            if (ctx != nullptr && ctx->exitWhenControlPanelCloses && ctx->trayWindow != nullptr)
                DestroyWindow(ctx->trayWindow);
            else
                DestroyWindow(window);
            return 0;
        case WM_NCDESTROY:
            if (ctx != nullptr) {
                if (ctx->panelEditBrush != nullptr) {
                    DeleteObject(ctx->panelEditBrush);
                    ctx->panelEditBrush = nullptr;
                }
                ctx->panelDevices = nullptr;
                ctx->panelPort = nullptr;
                ctx->panelFps = nullptr;
                ctx->panelBitrate = nullptr;
                ctx->panelDiscovery = nullptr;
                ctx->panelDeviceSelector = nullptr;
                ctx->panelDeviceConnect = nullptr;
                ctx->panelDeviceDefault = nullptr;
                ctx->panelDeviceUp = nullptr;
                ctx->panelDeviceRemove = nullptr;
                if (ctx->panelFont != nullptr) DeleteObject(ctx->panelFont);
                ctx->panelFont = nullptr;
                ctx->panelDeviceAddress = nullptr;
                ctx->panelDeviceMac = nullptr;
                ctx->panelDiscoveryAddress = nullptr;
                ctx->panelButtons.clear();
                ctx->panelDiscoveryKeys.clear();
                ctx->panelDiscoveryRows.clear();
                ctx->panelDeviceKeys.clear();
                ctx->panelDiscoveryInitialized = false;
                ctx->panelDiscoveryAddresses.clear();
                ctx->panelDeviceRows.clear();
                ctx->panelLayoutWidth = -1;
                if (ctx->controlPanel == window)
                    ctx->controlPanel = nullptr;
            }
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void EnsureControlPanelClass(HINSTANCE instance)
{
    static ATOM registered = 0;
    if (registered != 0)
        return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpfnWndProc = ControlPanelWindowProc;
    wc.lpszClassName = kControlPanelClass;
    registered = RegisterClassExW(&wc);
}

int PreviewNumber(const wchar_t* name, int fallback, int minimum, int maximum)
{
    wchar_t value[32]{};
    if (GetEnvironmentVariableW(name, value, 32) == 0) return fallback;
    wchar_t* end = nullptr;
    const long parsed = wcstol(value, &end, 10);
    return end != value && *end == L'\0' && parsed >= minimum && parsed <= maximum ? static_cast<int>(parsed) : fallback;
}

void ShowControlPanel(TrayContext* ctx)
{
    if (ctx == nullptr)
        return;
    if (ctx->controlPanel != nullptr && IsWindow(ctx->controlPanel)) {
        ShowWindow(ctx->controlPanel, SW_RESTORE);
        SetForegroundWindow(ctx->controlPanel);
        RefreshControlPanel(ctx);
        return;
    }
    EnsureControlPanelClass(ctx->hInstance);
    POINT cursor{}; GetCursorPos(&cursor);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor);
    const unsigned dpi = ctx->preview && ctx->previewDpi != 0 ? ctx->previewDpi : PanelDpi(nullptr);
    const int desiredWidth = ctx->preview ? PreviewNumber(L"IPAD_UI_PREVIEW_WIDTH", 960, 590, 2560) : 960;
    const int desiredHeight = ctx->preview ? PreviewNumber(L"IPAD_UI_PREVIEW_HEIGHT", 760, 360, 1600) : 760;
    RECT desired{0, 0, MulDiv(desiredWidth, static_cast<int>(dpi), 96), MulDiv(desiredHeight, static_cast<int>(dpi), 96)};
    AdjustWindowRectExForDpi(&desired, WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_CLIPCHILDREN, FALSE,
                            WS_EX_APPWINDOW | WS_EX_CONTROLPARENT | WS_EX_COMPOSITED, GetDpiForSystem());
    const int width = (std::min)(static_cast<int>(desired.right - desired.left), static_cast<int>(monitor.rcWork.right - monitor.rcWork.left) - 32);
    const int height = (std::min)(static_cast<int>(desired.bottom - desired.top), static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top) - 32);
    const int left = monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2;
    const int top = monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2;
    HWND panel = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_CONTROLPARENT | WS_EX_COMPOSITED, kControlPanelClass,
                                 ctx->preview ? L"iPad互联 · 界面预览" : L"iPad互联",
                                 WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN,
                                 left, top, width, height, nullptr, nullptr, ctx->hInstance, ctx);
    if (panel != nullptr) {
        // WM_GETMINMAXINFO can run before WM_NCCREATE installs the preview
        // context. Reapply its intended size with the synthetic DPI available.
        if (ctx->preview && ctx->previewDpi != 0)
            SetWindowPos(panel, nullptr, left, top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(panel, SW_SHOWNORMAL);
        UpdateWindow(panel);
        SetForegroundWindow(panel);
    }
}

void ShowContextMenu(HWND hwnd, TrayContext* ctx)
{
    RefreshUsbDevices(ctx, true);
    HMENU menu = CreatePopupMenu();

    // One checkable entry per iPad: click toggles just that one, so the others
    // keep streaming.
    bool several = ctx->cfg.devices.size() > 1;
    for (size_t i = 0; i < ctx->cfg.devices.size(); ++i) {
        // Checked follows the *state*, not IsRunning(): a sender that backed off
        // because another instance already holds this iPad keeps its thread
        // object but sits Idle, and showing that as connected would lie.
        UINT flags = MF_STRING | (ctx->apps[i]->GetState() != SenderApp::State::Idle ? MF_CHECKED : 0);
        AppendMenuW(menu, flags, IDM_DEVICE_FIRST + i, DeviceStatusText(ctx, i).c_str());
    }
    if (!ctx->cfg.devices.empty())
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // A physical cable-connected iPad needs no address entry or Bonjour. It
    // only starts when clicked; merely detecting a device never streams the
    // desktop. The session is transient so a later automatic Wi-Fi reconnect
    // cannot race this same iPad.
    bool usbEntryAdded = false;
    bool activeUsbListed = false;
    for (size_t i = 0; i < ctx->usbDevices.size(); ++i) {
        std::string target = MakeUsbMuxTarget(ctx->usbDevices[i].udid);
        bool active = ctx->usbApp && target == ctx->activeUsbTarget;
        UINT flags = MF_STRING | (active && ctx->usbApp->GetState() != SenderApp::State::Idle ? MF_CHECKED : 0);
        AppendMenuW(menu, flags, IDM_USB_DEVICE_FIRST + i,
                    active ? UsbStatusText(ctx).c_str() : L"连接已插入的 iPad（USB）");
        usbEntryAdded = true;
        activeUsbListed = activeUsbListed || active;
    }
    if (ctx->usbApp && !activeUsbListed)
        AppendMenuW(menu, MF_STRING | MF_CHECKED, IDM_DISCONNECT_USB, UsbStatusText(ctx).c_str());
    if (usbEntryAdded)
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    if (AnyRunning(ctx)) {
        AppendMenuW(menu, MF_STRING, IDM_DISCONNECT, several ? L"全部断开" : L"断开连接");
    } else {
        AppendMenuW(menu, MF_STRING | (ctx->cfg.devices.empty() ? MF_GRAYED : 0), IDM_CONNECT,
                    several ? L"连接默认 iPad" : L"连接 iPad");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_OPEN_CONTROL_PANEL, L"打开 iPad互联");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"连接与画质设置...");
    if (!IsElevated())
        AppendMenuW(menu, MF_STRING, IDM_RUNASADMIN, L"以管理员身份重新启动");
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd); // so the menu dismisses when clicking elsewhere
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* ctx = reinterpret_cast<TrayContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (ctx != nullptr && !ctx->preview && gShowControlPanel != 0 && msg == gShowControlPanel) {
        ShowControlPanel(ctx);
        return 0;
    }
    if (ctx != nullptr && !ctx->preview && gTaskbarCreated != 0 && msg == gTaskbarCreated) {
        // Explorer was restarted: its notification area forgot every icon.
        // Re-add ours, while the timer remains a retry if Explorer is not yet
        // ready to accept NIM_ADD.
        ctx->trayAdded = false;
        UpdateStatus(ctx);
        return 0;
    }

    switch (msg) {
        case WM_APP_TRAY:
            if (ctx == nullptr || ctx->preview) return 0;
            if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU)
                ShowContextMenu(hwnd, ctx);
            else if (LOWORD(lParam) == WM_LBUTTONDBLCLK)
                PostMessageW(hwnd, WM_COMMAND, IDM_OPEN_CONTROL_PANEL, 0);
            return 0;

        case WM_COMMAND:
            if (ctx == nullptr || ctx->preview) return 0;
            if (LOWORD(wParam) >= IDM_USB_DEVICE_FIRST && LOWORD(wParam) < IDM_USB_DEVICE_FIRST + ctx->usbDevices.size()) {
                ctx->startupConnection.transport = ConnectionTransport::None;
                size_t usbIndex = LOWORD(wParam) - IDM_USB_DEVICE_FIRST;
                std::string target = MakeUsbMuxTarget(ctx->usbDevices[usbIndex].udid);
                if (ctx->usbApp && target == ctx->activeUsbTarget)
                    StopUsbSession(ctx);
                else
                    StartUsbSession(ctx, std::move(target));
                UpdateStatus(ctx);
                return 0;
            }
            if (LOWORD(wParam) == IDM_DISCONNECT_USB) {
                ctx->startupConnection.transport = ConnectionTransport::None;
                StopUsbSession(ctx);
                UpdateStatus(ctx);
                return 0;
            }

            // One iPad's entry: toggle that sender alone.
            if (LOWORD(wParam) >= IDM_DEVICE_FIRST && LOWORD(wParam) < IDM_DEVICE_FIRST + ctx->apps.size()) {
                size_t index = LOWORD(wParam) - IDM_DEVICE_FIRST;
                SenderApp& app = *ctx->apps[index];
                if (app.IsRunning())
                    QueueDeviceCommand(ctx, TrayContext::CommandType::Stop);
                else
                    QueueDeviceCommand(ctx, TrayContext::CommandType::Switch, index);
                UpdateStatus(ctx);
                return 0;
            }

            switch (LOWORD(wParam)) {
                case IDM_OPEN_CONTROL_PANEL:
                    ShowControlPanel(ctx);
                    return 0;
                case IDM_CONNECT: {
                    const auto preferred = std::find_if(ctx->cfg.devices.begin(), ctx->cfg.devices.end(), [&](const DeviceConfig& device) {
                        return !ctx->cfg.preferredDeviceId.empty() && device.id == ctx->cfg.preferredDeviceId;
                    });
                    const size_t index = preferred == ctx->cfg.devices.end() ? 0 : static_cast<size_t>(preferred - ctx->cfg.devices.begin());
                    QueueDeviceCommand(ctx, TrayContext::CommandType::Start, index);
                    UpdateStatus(ctx);
                    return 0;
                }
                case IDM_DISCONNECT:
                    QueueDeviceCommand(ctx, TrayContext::CommandType::Stop);
                    UpdateStatus(ctx);
                    return 0;
                case IDM_SETTINGS: {
                    ShowControlPanel(ctx);
                    return 0;
                }
                case IDM_RUNASADMIN: {
                    // Relaunch elevated, then exit this instance. Stop first so
                    // the per-iPad locks are released before the elevated copy
                    // grabs them. Elevated lets touch reach elevated windows and
                    // register new resolutions without a separate prompt.
                    std::set<std::string> resumeOnFailure = RunningAddresses(ctx);
                    StopUsbSession(ctx);
                    StopConfiguredSenders(ctx);
                    wchar_t exe[MAX_PATH];
                    DWORD exeLength = GetModuleFileNameW(nullptr, exe, MAX_PATH);
                    if (exeLength == 0 || exeLength >= MAX_PATH - 1) {
                        StartAddresses(ctx, resumeOnFailure);
                        UpdateStatus(ctx);
                        return 0;
                    }
                    FILETIME parentCreated{}, parentExited{}, parentKernel{}, parentUser{};
                    if (!GetProcessTimes(GetCurrentProcess(), &parentCreated, &parentExited, &parentKernel, &parentUser)) {
                        StartAddresses(ctx, resumeOnFailure);
                        UpdateStatus(ctx);
                        return 0;
                    }
                    const uint64_t parentCreation = (static_cast<uint64_t>(parentCreated.dwHighDateTime) << 32) | parentCreated.dwLowDateTime;
                    std::wstring resumeParameters = L"--resume-after " + std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(parentCreation);
                    for (const std::string& address : resumeOnFailure)
                        resumeParameters += L" " + Widen(address); // running targets already passed numeric-IP policy
                    SHELLEXECUTEINFOW sei{};
                    sei.cbSize = sizeof(sei);
                    sei.lpVerb = L"runas";
                    sei.lpFile = exe;
                    sei.lpParameters = resumeParameters.c_str();
                    sei.nShow = SW_NORMAL;
                    if (ShellExecuteExW(&sei))
                        DestroyWindow(hwnd); // elevated copy is up; tear this one down
                    else if (!resumeOnFailure.empty()) {
                        // UAC was declined: the current process remains alive,
                        // so resume the senders that were deliberately stopped
                        // above instead of leaving the tray unexpectedly idle.
                        StartAddresses(ctx, resumeOnFailure);
                        UpdateStatus(ctx);
                    }
                    return 0;
                }
                case IDM_EXIT:
                    DestroyWindow(hwnd);
                    return 0;
            }
            return 0;

        case WM_TIMER:
            if (wParam == kStatusTimerId && ctx != nullptr) {
                if (ctx->preview) {
                    RefreshControlPanel(ctx);
                    return 0;
                }
                RefreshUsbDevices(ctx);
                RefreshConnectionCandidates(ctx);
                ProcessDeviceCommands(ctx);
                AdvanceStartupAutoConnect(ctx);
                RememberSuccessfulTransport(ctx);
                UpdateStatus(ctx);
                SyncPanelDiscovery(ctx);
                SyncPanelDeviceSelector(ctx);
                RefreshControlPanel(ctx);
            }
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, kStatusTimerId);
            if (ctx->trayAdded)
                Shell_NotifyIconW(NIM_DELETE, &ctx->nid);
            ctx->trayAdded = false;
            StopUsbSession(ctx);
            StopConfiguredSenders(ctx);
            if (ctx->controlPanel != nullptr && IsWindow(ctx->controlPanel))
                DestroyWindow(ctx->controlPanel);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int RunTray(HINSTANCE hInstance, std::optional<std::vector<std::string>> resumeAddresses, bool showControlPanelAtStartup,
            bool exitWhenControlPanelCloses)
{
    TrayContext ctx;
    ctx.hInstance = hInstance;
    ctx.preview = exitWhenControlPanelCloses;
    ctx.cfg = ctx.preview ? Config{} : Config::Load();
    ctx.exitWhenControlPanelCloses = exitWhenControlPanelCloses;
    if (ctx.preview) {
        wchar_t scene[64]{};
        GetEnvironmentVariableW(L"IPAD_UI_PREVIEW_SCENE", scene, 64);
        ctx.previewDpi = static_cast<unsigned>(PreviewNumber(L"IPAD_UI_PREVIEW_DPI", 0, 96, 240));
        ctx.previewEmpty = std::wstring(scene) == L"empty";
        ctx.previewLong = std::wstring(scene) == L"long";
        ctx.cfg.autoReconnect = false;
        ctx.cfg.darkTheme = false;
        if (!ctx.previewEmpty) {
            for (int i = 0; i < 2; ++i) {
                DeviceConfig device;
                device.id = "preview-device-" + std::to_string(i);
                device.name = ctx.previewLong ? "我的 iPad Pro - 用于验证超长中文设备名称不会覆盖地址或按钮的测试设备 " + std::to_string(i + 1) : i == 0 ? "我的 iPad Pro" : "书桌上的 iPad";
                device.lastIpv4 = "192.0.2." + std::to_string(20 + i);
                device.bonjourHost = "preview-ipad-" + std::to_string(i) + ".local";
                device.port = 9000;
                ctx.cfg.devices.push_back(device);
                ctx.discovered.emplace(device.lastIpv4, device.name);
                DiscoveryRecord record;
                record.id = device.id; record.address = device.lastIpv4; record.name = device.name;
                record.host = device.bonjourHost; record.port = device.port;
                ctx.discoveryCache.Merge({record});
            }
            ctx.cfg.preferredDeviceId = ctx.cfg.devices.front().id;
        }
    }
    // USB entries written by the early implementation would auto-start beside
    // their Wi-Fi counterpart after a restart. Migrate them out once; cable
    // sessions now live only in TrayContext and are explicitly selected.
    const size_t configuredBeforeUsbMigration = ctx.cfg.devices.size();
    ctx.cfg.devices.erase(std::remove_if(ctx.cfg.devices.begin(), ctx.cfg.devices.end(), [](const DeviceConfig& entry) {
                              return IsUsbMuxTarget(SplitDevice(entry).address);
                          }),
                          ctx.cfg.devices.end());
    if (!ctx.preview && ctx.cfg.devices.size() != configuredBeforeUsbMigration)
        ctx.cfg.Save();
    if (!ctx.preview) RebuildSenders(&ctx);
    gTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    gShowControlPanel = RegisterWindowMessageW(L"IpadConnect.ShowControlPanel.v1");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = ctx.preview ? kPreviewWndClass : kWndClass;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"iPad互联", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInstance,
                                nullptr);
    if (hwnd == nullptr)
        return 1;
    ctx.trayWindow = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&ctx));
    if (!ctx.preview && gShowControlPanel != 0)
        ChangeWindowMessageFilterEx(hwnd, gShowControlPanel, MSGFLT_ALLOW, nullptr);

    ctx.iconGreen = MakeStatusIcon(RGB(40, 200, 80));
    ctx.iconRed = MakeStatusIcon(RGB(225, 65, 55));
    ctx.iconGrey = MakeStatusIcon(RGB(150, 150, 150));

    ctx.nid.cbSize = sizeof(ctx.nid);
    ctx.nid.hWnd = hwnd;
    ctx.nid.uID = 1;
    ctx.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    ctx.nid.uCallbackMessage = WM_APP_TRAY;
    ctx.nid.hIcon = PickIcon(&ctx);
    wcsncpy_s(ctx.nid.szTip, L"iPad互联", _TRUNCATE);
    if (!ctx.preview) AddTrayIcon(&ctx);

    SetTimer(hwnd, kStatusTimerId, kStatusTimerMs, nullptr);

    if (!ctx.preview) {
        ctx.discoveryRunning = true;
        ctx.discovery = std::thread([&ctx] { RunDiscovery(&ctx); });
    }

    // A normal desktop launch opens the panel and connects automatically only
    // when the user previously opted in.  It first uses the transport that
    // last reached Streaming, then makes one bounded attempt over the other
    // available path instead of leaving the user on a dead connection loop.
    if (!ctx.preview && resumeAddresses) {
        // Explicit resume wins even when it is an empty set: an elevated
        // relaunch must preserve manually stopped targets rather than letting
        // autoReconnect start everything.
        StartAddresses(&ctx, std::set<std::string>(resumeAddresses->begin(), resumeAddresses->end()));
    } else if (!ctx.preview && ctx.cfg.autoReconnect)
        BeginStartupAutoConnect(&ctx);
    if (!ctx.preview) UpdateStatus(&ctx);
    if (showControlPanelAtStartup)
        ShowControlPanel(&ctx);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        const HWND focusBefore = GetFocus();
        if (ctx.controlPanel != nullptr && IsWindow(ctx.controlPanel) && IsDialogMessageW(ctx.controlPanel, &msg)) {
            if (ShouldRevealPanelFocus(msg.message, focusBefore != GetFocus()))
                EnsurePanelFocusVisible(ctx.controlPanel, &ctx);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ctx.discoveryRunning = false;
    if (ctx.discovery.joinable())
        ctx.discovery.join();

    DestroyIcon(ctx.iconGreen);
    DestroyIcon(ctx.iconRed);
    DestroyIcon(ctx.iconGrey);
    return 0;
}

} // namespace od
