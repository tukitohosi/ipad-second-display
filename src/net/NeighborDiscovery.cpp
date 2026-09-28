#include "net/NeighborDiscovery.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netlistmgr.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <iterator>
#include <map>
#include <set>

namespace od {
namespace {

using Microsoft::WRL::ComPtr;

std::string Lower(std::string value)
{
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

bool ParseNumeric(const std::string& value, SOCKADDR_INET& address)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST;
    addrinfo* answer = nullptr;
    if (getaddrinfo(value.c_str(), nullptr, &hints, &answer) != 0) return false;
    bool valid = false;
    if (answer && answer->ai_family == AF_INET) {
        address.Ipv4 = *reinterpret_cast<const sockaddr_in*>(answer->ai_addr);
        const uint32_t ip = ntohl(address.Ipv4.sin_addr.s_addr);
        valid = ip != 0 && (ip >> 24) != 0 && (ip >> 24) != 127 && (ip >> 24) < 224;
    } else if (answer && answer->ai_family == AF_INET6) {
        address.Ipv6 = *reinterpret_cast<const sockaddr_in6*>(answer->ai_addr);
        valid = !IN6_IS_ADDR_UNSPECIFIED(&address.Ipv6.sin6_addr) &&
                !IN6_IS_ADDR_LOOPBACK(&address.Ipv6.sin6_addr) &&
                !IN6_IS_ADDR_MULTICAST(&address.Ipv6.sin6_addr) &&
                !IN6_IS_ADDR_V4MAPPED(&address.Ipv6.sin6_addr);
    }
    if (answer) freeaddrinfo(answer);
    return valid;
}

std::string NumericText(const SOCKADDR_INET& address)
{
    char text[NI_MAXHOST]{};
    const int size = static_cast<int>(address.si_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6));
    if (getnameinfo(reinterpret_cast<const sockaddr*>(&address), size, text, sizeof(text),
                    nullptr, 0, NI_NUMERICHOST) != 0) return {};
    return Lower(text);
}

std::string EndpointKey(const std::string& endpoint)
{
    SOCKADDR_INET address{};
    if (ParseNumeric(endpoint, address)) return NumericText(address);
    std::string key = Lower(endpoint);
    if (!key.empty() && key.back() == '.') key.pop_back();
    return key;
}

std::string GuidText(const GUID& id)
{
    wchar_t text[40]{};
    if (StringFromGUID2(id, text, static_cast<int>(std::size(text))) == 0) return {};
    std::string result;
    for (const wchar_t* p = text; *p; ++p) result.push_back(static_cast<char>(*p));
    return Lower(result);
}

std::map<NET_IFINDEX, std::string> ConnectedScopes()
{
    std::map<NET_IFINDEX, std::string> scopes;
    std::set<NET_IFINDEX> ambiguous;
    ComPtr<INetworkListManager> manager;
    if (FAILED(CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&manager)))) return scopes;
    ComPtr<IEnumNetworkConnections> connections;
    if (FAILED(manager->GetNetworkConnections(&connections))) return scopes;
    for (;;) {
        ComPtr<INetworkConnection> connection;
        ULONG fetched = 0;
        if (connections->Next(1, connection.GetAddressOf(), &fetched) != S_OK || fetched != 1) break;
        VARIANT_BOOL connected = VARIANT_FALSE;
        GUID adapter{}, networkId{};
        NET_LUID luid{};
        NET_IFINDEX index = 0;
        ComPtr<INetwork> network;
        if (FAILED(connection->get_IsConnected(&connected)) || connected != VARIANT_TRUE ||
            FAILED(connection->GetAdapterId(&adapter)) ||
            ConvertInterfaceGuidToLuid(&adapter, &luid) != NO_ERROR ||
            ConvertInterfaceLuidToIndex(&luid, &index) != NO_ERROR ||
            FAILED(connection->GetNetwork(&network)) || FAILED(network->GetNetworkId(&networkId))) continue;
        const std::string scope = GuidText(networkId) + "/" + GuidText(adapter);
        if (scope.size() != 77 || ambiguous.contains(index)) continue;
        const auto it = scopes.find(index);
        if (it != scopes.end() && it->second != scope) {
            scopes.erase(it);
            ambiguous.insert(index);
        } else {
            scopes[index] = scope;
        }
    }
    return scopes;
}

bool OnLink(const SOCKADDR_INET& address, NET_IFINDEX requestedIndex, MIB_IPFORWARD_ROW2& route)
{
    SOCKADDR_INET source{};
    if (GetBestRoute2(nullptr, requestedIndex, nullptr, &address, 0, &route, &source) != NO_ERROR) return false;
    if (address.si_family == AF_INET) {
        if (route.NextHop.Ipv4.sin_addr.s_addr != 0) return false;
        // Windows can publish a /32 on-link route for a subnet broadcast.
        // Check actual interface prefixes as well as the selected route.
        PMIB_UNICASTIPADDRESS_TABLE locals = nullptr;
        if (GetUnicastIpAddressTable(AF_INET, &locals) != NO_ERROR || !locals) return false;
        bool reservedAddress = false;
        const uint32_t destination = ntohl(address.Ipv4.sin_addr.s_addr);
        for (ULONG i = 0; i < locals->NumEntries; ++i) {
            const auto& local = locals->Table[i];
            if (local.InterfaceIndex != route.InterfaceIndex) continue;
            const uint32_t localIp = ntohl(local.Address.Ipv4.sin_addr.s_addr);
            if (destination == localIp) { reservedAddress = true; break; }
            const unsigned length = local.OnLinkPrefixLength;
            if (length == 0 || length >= 31) continue;
            const uint32_t hostMask = UINT32_MAX >> length;
            if ((destination & ~hostMask) == (localIp & ~hostMask) &&
                ((destination & hostMask) == 0 || (destination & hostMask) == hostMask)) {
                reservedAddress = true;
                break;
            }
        }
        FreeMibTable(locals);
        if (reservedAddress) return false;
        const unsigned prefix = route.DestinationPrefix.PrefixLength;
        if (prefix < 31) {
            const uint32_t hostMask = prefix == 0 ? UINT32_MAX : UINT32_MAX >> prefix;
            const uint32_t host = ntohl(address.Ipv4.sin_addr.s_addr) & hostMask;
            if (host == 0 || host == hostMask) return false;
        }
        return true;
    }
    return IN6_IS_ADDR_UNSPECIFIED(&route.NextHop.Ipv6.sin6_addr) != 0;
}

std::vector<NeighborRecord> CollectRows(const std::map<NET_IFINDEX, std::string>& scopes)
{
    std::vector<NeighborRecord> result;
    PMIB_IPNET_TABLE2 table = nullptr;
    if (GetIpNetTable2(AF_UNSPEC, &table) != NO_ERROR || !table) return result;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const auto& row = table->Table[i];
        const auto scope = scopes.find(row.InterfaceIndex);
        if (scope == scopes.end() || row.PhysicalAddressLength != 6 || row.IsRouter ||
            row.State == NlnsUnreachable || row.State == NlnsIncomplete) continue;
        SOCKADDR_INET address = row.Address;
        if (address.si_family == AF_INET6 && IN6_IS_ADDR_LINKLOCAL(&address.Ipv6.sin6_addr))
            address.Ipv6.sin6_scope_id = row.InterfaceIndex;
        const std::string numeric = NumericText(address);
        SOCKADDR_INET parsed{};
        MIB_IPFORWARD_ROW2 route{};
        if (!ParseNumeric(numeric, parsed) || !OnLink(parsed, row.InterfaceIndex, route)) continue;
        static constexpr char hex[] = "0123456789ABCDEF";
        std::string mac;
        for (size_t j = 0; j < 6; ++j) {
            if (j) mac.push_back(':');
            mac.push_back(hex[row.PhysicalAddress[j] >> 4]);
            mac.push_back(hex[row.PhysicalAddress[j] & 15]);
        }
        mac = NormalizeMacAddress(mac);
        if (mac.empty()) continue;
        result.push_back({numeric, mac, scope->second, row.InterfaceIndex, row.State == NlnsReachable});
    }
    FreeMibTable(table);
    return result;
}

bool UsableNeighbor(const NeighborRecord& row)
{
    SOCKADDR_INET address{};
    return !row.networkScope.empty() && !NormalizeMacAddress(row.mac).empty() && ParseNumeric(row.address, address);
}

// A MAC can legitimately have an IPv4 and IPv6 address. Multiple addresses in
// either family are conservative ambiguity here; Bonjour still handles those.
bool ConflictingMac(const NeighborRecord& row, const std::vector<NeighborRecord>& neighbors)
{
    SOCKADDR_INET current{};
    if (!ParseNumeric(row.address, current)) return true;
    const std::string mac = NormalizeMacAddress(row.mac);
    for (const auto& other : neighbors) {
        if (!UsableNeighbor(other) || other.networkScope != row.networkScope) continue;
        const bool sameAddress = EndpointKey(other.address) == EndpointKey(row.address);
        const std::string otherMac = NormalizeMacAddress(other.mac);
        if (sameAddress && otherMac != mac) return true;
        SOCKADDR_INET candidate{};
        if (otherMac == mac && !sameAddress && ParseNumeric(other.address, candidate) &&
            candidate.si_family == current.si_family) return true;
    }
    return false;
}

} // namespace

std::string NormalizeMacAddress(std::string_view mac)
{
    if (mac.size() != 17 || (mac[2] != ':' && mac[2] != '-')) return {};
    const char separator = mac[2];
    std::array<unsigned char, 6> bytes{};
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    };
    for (size_t i = 0; i < 6; ++i) {
        const int high = digit(mac[i * 3]), low = digit(mac[i * 3 + 1]);
        if (high < 0 || low < 0 || (i < 5 && mac[i * 3 + 2] != separator)) return {};
        bytes[i] = static_cast<unsigned char>((high << 4) | low);
    }
    if ((bytes[0] & 1) || std::all_of(bytes.begin(), bytes.end(), [](unsigned char b) { return b == 0; })) return {};
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i) result.push_back(':');
        result.push_back(hex[bytes[i] >> 4]);
        result.push_back(hex[bytes[i] & 15]);
    }
    return result;
}

NeighborSnapshot ReadNeighborSnapshot(const std::vector<std::string>& knownTargets, bool resolveMissing)
{
    struct ComScope {
        HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
    } apartment;
    NeighborSnapshot snapshot;
    if (FAILED(apartment.result) && apartment.result != RPC_E_CHANGED_MODE) {
        snapshot.status = "network profile unavailable";
        return snapshot;
    }
    const auto scopes = ConnectedScopes();
    if (scopes.empty()) {
        snapshot.status = "network scope unavailable";
        return snapshot;
    }
    snapshot.records = CollectRows(scopes);
    if (resolveMissing) {
        std::set<std::string> seen;
        size_t refreshed = 0;
        for (const auto& target : knownTargets) {
            if (refreshed >= 4) break;
            SOCKADDR_INET address{};
            MIB_IPFORWARD_ROW2 route{};
            if (!ParseNumeric(target, address) || !seen.insert(NumericText(address)).second ||
                !OnLink(address, 0, route) || !scopes.contains(route.InterfaceIndex)) continue;
            const bool fresh = std::any_of(snapshot.records.begin(), snapshot.records.end(), [&](const auto& row) {
                return row.reachable && EndpointKey(row.address) == EndpointKey(target);
            });
            if (fresh) continue;
            MIB_IPNET_ROW2 row{};
            row.Address = address;
            row.InterfaceLuid = route.InterfaceLuid;
            row.InterfaceIndex = route.InterfaceIndex;
            ++refreshed;
            ResolveIpNetEntry2(&row, nullptr);
        }
        if (refreshed) snapshot.records = CollectRows(scopes);
    }
    snapshot.status = snapshot.records.empty() ? "no usable local neighbors" : "local neighbor cache";
    return snapshot;
}

std::vector<std::string> BuildDeviceCandidates(const DeviceConfig& device,
                                             const std::vector<DiscoveryRecord>& discovered,
                                             const std::vector<NeighborRecord>& neighbors)
{
    std::vector<std::string> result;
    std::set<std::string> seen;
    auto add = [&](const std::string& value) {
        if (!value.empty() && result.size() < 8 && seen.insert(EndpointKey(value)).second) result.push_back(value);
    };
    if (!device.id.empty()) {
        for (const auto& record : discovered) {
            if (!record.online || record.goodbye || record.id != device.id) continue;
            add(record.address);
            for (const auto& address : record.addresses) add(address);
        }
        if (device.macMatchingEnabled) for (const auto& binding : device.macBindings) {
            const std::string mac = NormalizeMacAddress(binding.mac);
            if (mac.empty() || binding.networkScope.empty()) continue;
            const bool conflictingBinding = std::any_of(device.macBindings.begin(), device.macBindings.end(), [&](const auto& other) {
                return other.networkScope == binding.networkScope && NormalizeMacAddress(other.mac) != mac;
            });
            if (conflictingBinding) continue;
            for (const auto& row : neighbors) {
                if (UsableNeighbor(row) && row.networkScope == binding.networkScope &&
                    NormalizeMacAddress(row.mac) == mac && !ConflictingMac(row, neighbors)) add(row.address);
            }
        }
        for (const auto& record : discovered)
            if (record.online && !record.goodbye && record.id == device.id) add(record.host);
    }
    add(device.bonjourHost);
    add(device.lastIpv4);
    return result;
}

bool LearnVerifiedMacBinding(DeviceConfig& device, const std::string& connectedAddress,
                             const std::string& helloId, const std::vector<NeighborRecord>& neighbors)
{
    if (!device.macMatchingEnabled || device.id.empty() || device.id != helloId) return false;
    const NeighborRecord* observation = nullptr;
    for (const auto& row : neighbors) {
        if (!row.reachable || !UsableNeighbor(row) || EndpointKey(row.address) != EndpointKey(connectedAddress)) continue;
        if (ConflictingMac(row, neighbors)) return false;
        if (observation && (observation->networkScope != row.networkScope ||
                            NormalizeMacAddress(observation->mac) != NormalizeMacAddress(row.mac))) return false;
        observation = &row;
    }
    if (!observation) return false;
    const MacBinding learned{NormalizeMacAddress(observation->mac), observation->networkScope};
    const auto found = std::find_if(device.macBindings.begin(), device.macBindings.end(), [&](const MacBinding& binding) {
        return binding.networkScope == learned.networkScope;
    });
    if (found != device.macBindings.end()) {
        const auto count = std::count_if(device.macBindings.begin(), device.macBindings.end(), [&](const auto& binding) {
            return binding.networkScope == learned.networkScope;
        });
        if (*found == learned && count == 1) return false;
        *found = learned;
        device.macBindings.erase(std::remove_if(found + 1, device.macBindings.end(), [&](const auto& binding) {
            return binding.networkScope == learned.networkScope;
        }), device.macBindings.end());
    } else {
        if (device.macBindings.size() >= 8) device.macBindings.erase(device.macBindings.begin());
        device.macBindings.push_back(learned);
    }
    return true;
}

} // namespace od
