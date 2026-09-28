#include "net/NetworkSafety.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <iphlpapi.h>
#include <netlistmgr.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <vector>

namespace od {

namespace {

using Microsoft::WRL::ComPtr;

bool IsLoopback(uint32_t hostOrder)
{
    return (hostOrder & 0xFF000000u) == 0x7F000000u;
}

bool IsPrivateOrLinkLocal(uint32_t hostOrder)
{
    return (hostOrder & 0xFF000000u) == 0x0A000000u ||
           (hostOrder & 0xFFF00000u) == 0xAC100000u ||
           (hostOrder & 0xFFFF0000u) == 0xC0A80000u ||
           (hostOrder & 0xFFFF0000u) == 0xA9FE0000u;
}

struct ResolvedAddress {
    sockaddr_storage storage{};
    int length = 0;
    std::string numeric;
};

template <typename AddressInfo>
void AppendResolvedAddresses(const AddressInfo* result, std::vector<ResolvedAddress>& addresses)
{
    for (const AddressInfo* item = result; item != nullptr; item = item->ai_next) {
        if (item->ai_family != AF_INET && item->ai_family != AF_INET6) continue;
        if (item->ai_addr == nullptr || item->ai_addrlen > sizeof(sockaddr_storage)) continue;
        ResolvedAddress resolved;
        resolved.length = static_cast<int>(item->ai_addrlen);
        std::memcpy(&resolved.storage, item->ai_addr, item->ai_addrlen);
        char host[NI_MAXHOST]{};
        if (getnameinfo(item->ai_addr, static_cast<int>(item->ai_addrlen), host, sizeof(host), nullptr, 0,
                        NI_NUMERICHOST) == 0) {
            resolved.numeric = host;
            addresses.push_back(std::move(resolved));
        }
    }
}

bool ResolveTarget(const std::string& target, std::vector<ResolvedAddress>& addresses,
                   const std::atomic<bool>* stopRequested, int timeoutMs, std::string& failure)
{
    auto stopped = [&] { return stopRequested && stopRequested->load(); };
    if (stopped()) { failure = "address lookup cancelled"; return false; }
    if (target.empty()) { failure = "target address is empty"; return false; }

    addrinfo numericHints{};
    numericHints.ai_family = AF_UNSPEC;
    numericHints.ai_socktype = SOCK_STREAM;
    numericHints.ai_protocol = IPPROTO_TCP;
    numericHints.ai_flags = AI_NUMERICHOST;
    addrinfo* numeric = nullptr;
    if (getaddrinfo(target.c_str(), nullptr, &numericHints, &numeric) == 0) {
        AppendResolvedAddresses(numeric, addresses);
        freeaddrinfo(numeric);
        if (stopped()) { failure = "address lookup cancelled"; addresses.clear(); return false; }
        return !addresses.empty();
    }
    if (numeric) freeaddrinfo(numeric);

    const int budget = (std::clamp)(timeoutMs, 0, 2000);
    if (budget == 0) { failure = "address lookup timed out"; return false; }
    const int wideLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, target.c_str(),
                                               static_cast<int>(target.size()), nullptr, 0);
    if (wideLength <= 0) { failure = "target address is not valid UTF-8"; return false; }
    std::wstring name(static_cast<size_t>(wideLength), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, target.c_str(), static_cast<int>(target.size()),
                        name.data(), wideLength);

    HANDLE completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!completed) { failure = "address lookup event could not be created"; return false; }
    OVERLAPPED operation{};
    operation.hEvent = completed;
    HANDLE cancellation = nullptr;
    ADDRINFOEXW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    PADDRINFOEXW result = nullptr;
    timeval queryTimeout{};
    queryTimeout.tv_sec = budget / 1000;
    queryTimeout.tv_usec = (budget % 1000) * 1000;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget);
    int error = GetAddrInfoExW(name.c_str(), nullptr, NS_DNS, nullptr, &hints, &result,
                               &queryTimeout, &operation, nullptr, &cancellation);
    if (error == WSA_IO_PENDING) {
        for (;;) {
            if (WaitForSingleObject(completed, 0) == WAIT_OBJECT_0) break;
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (stopped() || remaining <= 0) {
                failure = stopped() ? "address lookup cancelled" : "address lookup timed out";
                GetAddrInfoExCancel(&cancellation);
                // Windows signals the completion mechanism on cancellation.
                // Never release stack-backed OVERLAPPED/result storage before
                // that signal, even when completion raced with cancellation.
                WaitForSingleObject(completed, INFINITE);
                break;
            }
            const DWORD wait = WaitForSingleObject(completed, static_cast<DWORD>((std::min)(remaining, int64_t{50})));
            if (wait == WAIT_OBJECT_0) break;
            if (wait == WAIT_FAILED) {
                failure = "address lookup wait failed";
                GetAddrInfoExCancel(&cancellation);
                WaitForSingleObject(completed, INFINITE);
                break;
            }
        }
        error = GetAddrInfoExOverlappedResult(&operation);
    }
    if (stopped()) failure = "address lookup cancelled";
    if (error == NO_ERROR && failure.empty()) AppendResolvedAddresses(result, addresses);
    if (result) FreeAddrInfoExW(result);
    CloseHandle(completed);
    if (addresses.empty() && failure.empty()) failure = "target could not be resolved to IPv4 or IPv6";
    return !addresses.empty();
}

bool IsLoopback(const ResolvedAddress& address)
{
    if (address.storage.ss_family == AF_INET)
        return IsLoopback(ntohl(reinterpret_cast<const sockaddr_in*>(&address.storage)->sin_addr.s_addr));
    const auto& value = reinterpret_cast<const sockaddr_in6*>(&address.storage)->sin6_addr;
    return IN6_IS_ADDR_LOOPBACK(&value) != 0;
}

bool IsPrivateOrLinkLocal(const ResolvedAddress& address)
{
    if (address.storage.ss_family == AF_INET)
        return IsPrivateOrLinkLocal(ntohl(reinterpret_cast<const sockaddr_in*>(&address.storage)->sin_addr.s_addr));
    const auto& bytes = reinterpret_cast<const sockaddr_in6*>(&address.storage)->sin6_addr.u.Byte;
    return (bytes[0] & 0xFE) == 0xFC || (bytes[0] == 0xFE && (bytes[1] & 0xC0) == 0x80);
}

bool HasTrustedWindowsNetwork(const ResolvedAddress& target)
{
    sockaddr_storage destination = target.storage;
    DWORD interfaceIndex = 0;
    if (GetBestInterfaceEx(reinterpret_cast<sockaddr*>(&destination), &interfaceIndex) != NO_ERROR)
        return false;

    NET_LUID interfaceLuid{};
    GUID adapterId{};
    if (ConvertInterfaceIndexToLuid(interfaceIndex, &interfaceLuid) != NO_ERROR ||
        ConvertInterfaceLuidToGuid(&interfaceLuid, &adapterId) != NO_ERROR)
        return false;

    ComPtr<INetworkListManager> manager;
    if (FAILED(CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&manager))))
        return false;

    ComPtr<IEnumNetworkConnections> connections;
    if (FAILED(manager->GetNetworkConnections(&connections)))
        return false;

    bool foundConnectedRoute = false;
    for (;;) {
        ComPtr<INetworkConnection> connection;
        ULONG fetched = 0;
        HRESULT hr = connections->Next(1, connection.GetAddressOf(), &fetched);
        if (hr == S_FALSE && fetched == 0)
            break;
        if (hr != S_OK || fetched != 1)
            return false; // enumeration failure is not proof of a trusted route

        GUID candidateId{};
        if (FAILED(connection->GetAdapterId(&candidateId)))
            return false;
        if (!IsEqualGUID(candidateId, adapterId))
            continue;

        VARIANT_BOOL connected = VARIANT_FALSE;
        if (FAILED(connection->get_IsConnected(&connected)))
            return false;
        if (connected != VARIANT_TRUE)
            continue;
        foundConnectedRoute = true;

        ComPtr<INetwork> network;
        if (FAILED(connection->GetNetwork(&network)))
            return false;
        NLM_NETWORK_CATEGORY category = NLM_NETWORK_CATEGORY_PUBLIC;
        if (FAILED(network->GetCategory(&category)) ||
            (category != NLM_NETWORK_CATEGORY_PRIVATE && category != NLM_NETWORK_CATEGORY_DOMAIN_AUTHENTICATED))
            return false;
    }
    return foundConnectedRoute;
}

} // namespace

NetworkSafetyResult CheckTrustedLan(const std::string& target, bool requirePrivateNetwork,
                                    const std::atomic<bool>* stopRequested, int timeoutMs)
{
    std::vector<ResolvedAddress> addresses;
    std::string resolutionFailure;
    if (!ResolveTarget(target, addresses, stopRequested, timeoutMs, resolutionFailure))
        return {false, resolutionFailure.empty() ? "target could not be resolved to IPv4 or IPv6" : resolutionFailure, {}, true};
    if (!requirePrivateNetwork)
        return {true, "private-network enforcement disabled by explicit configuration", addresses.front().numeric};

    bool allLoopback = true;
    for (const ResolvedAddress& address : addresses) {
        bool loopback = IsLoopback(address);
        allLoopback = allLoopback && loopback;
        if (!loopback && !IsPrivateOrLinkLocal(address))
            return {false, "target resolves to a non-private address"};
    }
    if (allLoopback)
        return {true, "loopback self-test", addresses.front().numeric};

    // Validate the profile of the route that Windows would actually use for
    // every address returned by the name. Merely finding some unrelated
    // Private adapter is insufficient when the iPad route itself is Public.
    for (const ResolvedAddress& address : addresses) {
        if (!IsLoopback(address) && !HasTrustedWindowsNetwork(address))
            return {false, "the Windows network route to the target is Public or cannot be verified; set that trusted LAN to Private"};
    }
    return {true, "trusted private LAN", addresses.front().numeric};
}

} // namespace od
