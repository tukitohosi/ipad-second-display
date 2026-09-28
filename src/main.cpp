#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cwchar>
#include <limits>
#include <string_view>
#include <fcntl.h>
#include <filesystem>
#include <iterator>
#include <io.h>
#include <string>
#include <utility>
#include <vector>

#include <winsock2.h>

#include <mfapi.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#include "app/SenderApp.h"
#include "app/Config.h"
#include "app/TrayApp.h"
#include "display/DisplayCatalog.h"
#include "display/VirtualDisplay.h"
#include "net/Mdns.h"
#include "net/UsbMux.h"

namespace {

// Route diagnostics to %APPDATA%\MouseLink\log.txt. Under the GUI
// subsystem there is no console to print to, and a file gives persistent logs
// regardless of how the app was launched.
// `name` is the log's file name: the tray owns log.txt, while a headless CLI
// sender gets its own log-<pid>.txt. Both open CREATE_ALWAYS, so without the
// split a second process would truncate the first one's log out from under it.
void RedirectLogToFile(const std::string& name, bool preview = false)
{
    std::wstring dir = std::filesystem::path(od::Config::FilePath()).parent_path().wstring();
    if (preview) {
        wchar_t temp[MAX_PATH]{};
        if (GetTempPathW(MAX_PATH, temp) == 0)
            return;
        dir = std::wstring(temp) + L"iPadConnect-preview";
    }

    CreateDirectoryW(dir.c_str(), nullptr); // no-op if it already exists
    std::wstring wideName(name.begin(), name.end()); // log names are fixed ASCII
    std::wstring logPath = dir + L"\\" + wideName;

    // Under the GUI subsystem there is no console, so stdout/stderr have no
    // valid fd to redirect. Bind them to NUL first to give them real fds, then
    // dup our log handle over those. (freopen straight to the log would work
    // too but opens exclusive — this keeps the log tailable while running.)
    FILE* f = nullptr;
    freopen_s(&f, "NUL", "w", stdout);
    freopen_s(&f, "NUL", "w", stderr);

    HANDLE h = CreateFileW(logPath.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_WRONLY | _O_TEXT);
    if (fd == -1) {
        CloseHandle(h);
        return;
    }
    _dup2(fd, _fileno(stdout));
    _dup2(fd, _fileno(stderr));
    _close(fd); // the dup'd copies on stdout/stderr keep the file open
    setvbuf(stdout, nullptr, _IONBF, 0); // unbuffered so a live stream shows immediately
    setvbuf(stderr, nullptr, _IONBF, 0);
}

// The one-off commands below are meant to be read in the terminal that started
// them, but this is a GUI-subsystem process and has no console of its own — so
// borrow the caller's. Returns false when there is none (double-clicked, or
// started by a process without a console), and the log file takes over.
bool AttachParentConsole()
{
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
        return false;
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    return true;
}

bool ParseSafeResolution(const char* widthText, const char* heightText, uint32_t& width, uint32_t& height)
{
    auto parseAxis = [](const char* text, uint32_t& value) {
        if (text == nullptr || *text == '\0')
            return false;
        errno = 0;
        char* end = nullptr;
        unsigned long parsed = std::strtoul(text, &end, 10);
        if (errno == ERANGE || end == text || *end != '\0' || parsed < 320 || parsed > 8192 ||
            (parsed & 1ul) != 0)
            return false;
        value = static_cast<uint32_t>(parsed);
        return true;
    };

    if (!parseAxis(widthText, width) || !parseAxis(heightText, height))
        return false;
    return static_cast<uint64_t>(width) * height <= 8192ull * 4096ull;
}

std::string MaskDeviceId(const std::string& udid)
{
    if (udid.size() <= 8)
        return "USB iPad";
    return "USB iPad " + udid.substr(0, 4) + "..." + udid.substr(udid.size() - 4);
}

// Last-resort diagnostics: on an unhandled crash, write the exception code and
// the faulting module + offset to the log before dying. Without this the tray
// process just vanishes with no WER entry, so a crash during a resolution/
// rotation change left nothing to point at the culprit.
LONG WINAPI CrashLogger(EXCEPTION_POINTERS* ep)
{
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    HMODULE mod = nullptr;
    char modName[MAX_PATH] = "?";
    uintptr_t off = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(addr), &mod)) {
        GetModuleFileNameA(mod, modName, MAX_PATH);
        off = reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod);
    }
    fprintf(stderr, "\n*** CRASH: code=0x%08lX addr=%p module=%s +0x%zX ***\n", code, addr, modName, off);
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER; // let the process terminate
}

// An elevated replacement waits for the exact launching process to finish.
// PID reuse is rejected using the creation time, and unrelated programs cannot
// be used as a handoff parent. The normal single-instance gate still runs next.
bool WaitForResumeParent(int argc, char** argv)
{
    if (argc < 4) return false;
    const auto parse = [](const char* text, uint64_t& value) {
        const std::string_view input(text);
        const auto result = std::from_chars(input.data(), input.data() + input.size(), value);
        return !input.empty() && result.ec == std::errc{} && result.ptr == input.data() + input.size();
    };
    uint64_t parentValue = 0, expectedCreation = 0;
    if (!parse(argv[2], parentValue) || !parse(argv[3], expectedCreation) || parentValue == 0 ||
        parentValue > (std::numeric_limits<DWORD>::max)() || expectedCreation == 0 || parentValue == GetCurrentProcessId())
        return false;
    HANDLE parent = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(parentValue));
    if (parent == nullptr) {
        // The parent can complete its shutdown before the elevated child runs.
        // Re-enter the ordinary mutex gate in that case; never bypass it.
        return GetLastError() == ERROR_INVALID_PARAMETER;
    }
    FILETIME creation{}, exit{}, kernel{}, user{};
    const bool gotTimes = GetProcessTimes(parent, &creation, &exit, &kernel, &user) != FALSE;
    const uint64_t actualCreation = (static_cast<uint64_t>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
    wchar_t parentPath[32768]{}, currentPath[32768]{};
    DWORD parentPathLength = static_cast<DWORD>(std::size(parentPath));
    const DWORD currentPathLength = GetModuleFileNameW(nullptr, currentPath, static_cast<DWORD>(std::size(currentPath)));
    const bool identityMatches = gotTimes && actualCreation == expectedCreation &&
        QueryFullProcessImageNameW(parent, 0, parentPath, &parentPathLength) != FALSE &&
        currentPathLength > 0 && currentPathLength < std::size(currentPath) && _wcsicmp(parentPath, currentPath) == 0;
    const DWORD result = identityMatches ? WaitForSingleObject(parent, 10000) : WAIT_FAILED;
    CloseHandle(parent);
    return result == WAIT_OBJECT_0;
}

} // namespace

int main(int argc, char** argv)
{
    // Run per-monitor DPI aware (V2): on a scaled desktop (e.g. 125%) a
    // DPI-unaware process sees monitor positions in *virtualized* coordinates
    // from GetMonitorInfo but sets them in *physical* coordinates via
    // ChangeDisplaySettingsEx, so a saved position wouldn't round-trip. Being
    // DPI-aware keeps every coordinate API in one physical space — needed for
    // stable position persistence and correct input mapping on scaled displays.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const std::string command = argc >= 2 ? argv[1] : "";
    const bool resumeAfter = command == "--resume-after";
    if (resumeAfter && !WaitForResumeParent(argc, argv)) return 1;
    const bool trayMode = argc < 2 || command == "--resume" || resumeAfter;
    struct TrayInstance {
        HANDLE handle = nullptr;
        ~TrayInstance() { if (handle) CloseHandle(handle); }
    } instance;
    if (trayMode) {
        // Claim before opening log/config: another launch must not truncate
        // the active sender's log or start a second discovery/connection loop.
        instance.handle = CreateMutexW(nullptr, FALSE, L"Local\\IpadConnect.Tray.v1");
        const DWORD error = GetLastError();
        HWND existing = FindWindowExW(HWND_MESSAGE, nullptr, L"MouseLinkTrayWindow", nullptr);
        // An elevated instance can deny access to its mutex; the window still
        // accepts our narrowly allowed activation message.
        if (!instance.handle && !existing) return 1;
        if (error == ERROR_ALREADY_EXISTS || existing) {
            for (int attempt = 0; !existing && attempt < 100; ++attempt) {
                Sleep(50);
                existing = FindWindowExW(HWND_MESSAGE, nullptr, L"MouseLinkTrayWindow", nullptr);
            }
            if (existing) {
                DWORD processId = 0;
                GetWindowThreadProcessId(existing, &processId);
                AllowSetForegroundWindow(processId);
                PostMessageW(existing, RegisterWindowMessageW(L"IpadConnect.ShowControlPanel.v1"), 0, 0);
                // The earlier release understands this menu command too.
                PostMessageW(existing, WM_COMMAND, 40009, 0);
            }
            return 0;
        }
    }

    // Keep COM (MTA) and Media Foundation alive for the whole process. The
    // per-connection H264Encoder does its own CoInitialize/MFStartup on its
    // worker thread and the matching CoUninitialize/MFShutdown on Stop(); with
    // this process-wide reference those only *decrement* the refcounts instead
    // of tearing the subsystems down while DXGI/MF background threads are still
    // attached — which crashed the app on Disconnect. Intentionally never
    // released (process-lifetime).
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);

    bool oneOff = command == "--register-resolution" || command == "--cleanup-monitors" ||
                  command == "--remove-display" || command == "--browse-mdns" || command == "--list-displays" ||
                  command == "--list-usb";

    // A one-off answers into the caller's terminal; a sender writes to the log,
    // and a headless one gets its own file so two of them don't truncate each
    // other's.
    if (!oneOff || !AttachParentConsole()) {
        RedirectLogToFile(trayMode ? "log.txt" : "log-" + std::to_string(GetCurrentProcessId()) + ".txt",
                          command == "--preview-ui");
    }

    SetUnhandledExceptionFilter(CrashLogger);

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    int rc = 0;
    if (command == "--register-resolution") {
        // Elevated one-off: register a custom resolution (+ its rotation) so the
        // virtual monitor can use it. Invoked by the self-elevate path or by hand.
        uint32_t w = 0, h = 0;
        if (argc != 4 || !ParseSafeResolution(argc > 2 ? argv[2] : nullptr, argc > 3 ? argv[3] : nullptr, w, h)) {
            printf("usage: --register-resolution <even width 320..8192> <even height 320..8192>\n");
            rc = 2;
        } else {
            bool ok = od::VirtualDisplay::RegisterResolutions(w, h);
            printf("register %ux%u: %s\n", w, h, ok ? "ok" : "failed");
            rc = ok ? 0 : 1;
        }
    } else if (argc >= 2 && std::string(argv[1]) == "--remove-display") {
        // Driver-global numeric indexes are not an ownership proof and can
        // unplug another application's live display. Product builds refuse
        // this upstream maintenance command; recovery is performed only by
        // the receipt-validated PowerShell tool shipped in tools/safety.
        printf("remove-display is disabled: use the receipt-validated recovery tool\n");
        rc = 2;
    } else if (argc >= 2 && std::string(argv[1]) == "--browse-mdns") {
        // Checks the response parser against malformed packets, then shows what
        // is actually advertising right now — the parser reads data from
        // whoever answers on the network, so it gets a probe of its own.
        bool ok = od::SelfCheck();
        for (const od::MdnsReceiver& receiver : od::BrowseReceivers(1500))
            printf("%s  %s  host=%s port=%u id=%s\n", receiver.address.c_str(), receiver.instance.c_str(),
                   receiver.host.c_str(), receiver.port, receiver.id.c_str());
        rc = ok ? 0 : 1;
    } else if (argc >= 2 && std::string(argv[1]) == "--cleanup-monitors") {
        // A broad ghost cleanup has no ownership receipt and can touch Parsec
        // nodes created by other software. Keep the argument recognizable for
        // upstream users, but refuse it in the hardened product build.
        printf("cleanup-monitors is disabled: use the receipt-validated recovery tool\n");
        rc = 2;
    } else if (argc >= 2 && std::string(argv[1]) == "--list-displays") {
        for (const od::DisplayInfo& display : od::EnumerateAttachedDisplays())
            wprintf(L"%ls  %ld,%ld %ldx%ld%s\n", display.deviceName.c_str(), display.bounds.left,
                    display.bounds.top, display.bounds.right - display.bounds.left,
                    display.bounds.bottom - display.bounds.top, display.primary ? L"  primary" : L"");
    } else if (argc >= 2 && std::string(argv[1]) == "--list-usb") {
        std::vector<od::UsbMuxDevice> devices = od::ListUsbMuxDevices();
        if (devices.empty()) {
            printf("No cable-connected iPad was available through Apple Mobile Device Service. Unlock the iPad and trust this PC.\n");
            rc = 1;
        } else {
            for (const od::UsbMuxDevice& device : devices)
                printf("%s\n", MaskDeviceId(device.udid).c_str());
        }
    } else if (command == "--capture-existing") {
        // Non-mutating stream gate: capture an already attached GDI output and
        // never add/remove/resize a virtual display. Useful with the loopback
        // mock receiver before any VDD is installed.
        if (argc != 4) {
            fprintf(stderr, "usage: --capture-existing \\\\.\\DISPLAYn <numeric-ipv4>\n");
            rc = 2;
        } else {
            od::StreamSettings settings;
            std::string displayName = argv[2];
            settings.existingDisplayName.assign(displayName.begin(), displayName.end());
            od::SenderApp app;
            app.RunBlocking(argv[3], 9000, std::move(settings));
        }
    } else if (command == "--resume" || resumeAfter) {
        std::vector<std::string> addresses;
        for (int i = resumeAfter ? 4 : 2; i < argc; ++i)
            addresses.emplace_back(argv[i]);
        rc = od::RunTray(GetModuleHandleW(nullptr), std::move(addresses));
    } else if (command == "--preview-ui") {
        // A local design preview deliberately resumes an empty connection set:
        // it shows the real control panel but never creates a virtual display,
        // starts a sender, or competes with an already-running iPad session.
        rc = od::RunTray(GetModuleHandleW(nullptr), std::vector<std::string>{}, true, true);
    } else if (argc >= 2) {
        // Headless CLI mode: stream to the given IP until killed (handy for
        // testing/scripting; logging goes to the inherited/redirected stdout).
        // The per-iPad connection guard lives in SenderApp.
        od::SenderApp app;
        app.RunBlocking(argv[1], 9000);
    } else {
        // A normal desktop launch opens the control panel immediately. The
        // tray keeps the connection alive after that window is closed.
        rc = od::RunTray(GetModuleHandleW(nullptr), std::nullopt, true);
    }

    WSACleanup();
    return rc;
}
