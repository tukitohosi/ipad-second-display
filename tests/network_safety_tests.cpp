#ifdef NDEBUG
#undef NDEBUG
#endif

#include "net/NetworkSafety.h"

#include <winsock2.h>
#include <windows.h>

#include <cassert>
#include <chrono>
#include <string>
#include <thread>

int main()
{
    using namespace std::chrono_literals;
    WSADATA winsock{};
    assert(WSAStartup(MAKEWORD(2, 2), &winsock) == 0);

    for (const std::string numeric : {"127.0.0.1", "127.0.0.2", "::1"}) {
        const auto result = od::CheckTrustedLan(numeric, true, nullptr, 0);
        assert(result.allowed && !result.resolutionFailed && !result.resolvedTarget.empty());
    }
    const auto localhost = od::CheckTrustedLan("localhost", true);
    assert(localhost.allowed && !localhost.resolutionFailed && !localhost.resolvedTarget.empty());
    const auto publicAddress = od::CheckTrustedLan("8.8.8.8", true);
    assert(!publicAddress.allowed && !publicAddress.resolutionFailed); // numeric policy check; no connection/query
    const auto empty = od::CheckTrustedLan("", true);
    assert(!empty.allowed && empty.resolutionFailed);

    std::atomic<bool> cancelled{true};
    const auto cancelledStart = std::chrono::steady_clock::now();
    const auto beforeStart = od::CheckTrustedLan("never-looked-up.invalid", true, &cancelled);
    assert(!beforeStart.allowed && beforeStart.resolutionFailed);
    assert(beforeStart.reason == "address lookup cancelled");
    assert(std::chrono::steady_clock::now() - cancelledStart < 500ms);
    const auto noBudget = od::CheckTrustedLan("never-looked-up.invalid", true, nullptr, 0);
    assert(!noBudget.allowed && noBudget.resolutionFailed && noBudget.reason == "address lookup timed out");

    // Exercise the native asynchronous API against a reserved nonexistent name.
    // This can complete before cancellation (e.g. cached NXDOMAIN), so accept
    // either lookup failure or cancellation, while always enforcing the bound.
    // There is no TCP connection, real receiver, or network configuration change.
    cancelled = false;
    const std::string absent = "ipad-candidate-test-" + std::to_string(GetCurrentProcessId()) + ".invalid";
    const auto started = std::chrono::steady_clock::now();
    std::thread cancelSoon([&] { std::this_thread::sleep_for(20ms); cancelled = true; });
    const auto pending = od::CheckTrustedLan(absent, true, &cancelled, 2000);
    cancelSoon.join();
    assert(!pending.allowed && pending.resolutionFailed);
    assert(std::chrono::steady_clock::now() - started < 2500ms);

    WSACleanup();
    return 0;
}
