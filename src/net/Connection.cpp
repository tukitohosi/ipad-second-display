#include "net/Connection.h"
#include "net/UsbMux.h"

#include <algorithm>
#include <chrono>
#include <limits>

#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

namespace od {

namespace {

// Bounds a single connect attempt so an unreachable iPad (SYN black-hole ~20s)
// fails fast — keeps reconnect snappy and Stop()/Disconnect from hanging.
constexpr int kConnectTimeoutMs = 3000;
constexpr int kInterruptPollMs = 25;

bool PrepareConnectedSocket(SOCKET socket)
{
    BOOL noDelay = TRUE;
    setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    // Poll readiness is advisory. Nonblocking I/O prevents a recv/send from
    // getting stuck if readiness changes just before that call, including a
    // concurrent Interrupt. Do this only after the usbmux handshake is done.
    u_long nonBlocking = 1;
    return ioctlsocket(socket, FIONBIO, &nonBlocking) == 0;
}

bool ConnectWithTimeout(SOCKET s, const sockaddr* addr, int addrlen, int timeoutMs)
{
    u_long nonBlocking = 1;
    if (ioctlsocket(s, FIONBIO, &nonBlocking) != 0)
        return false;

    bool ok = false;
    int r = ::connect(s, addr, addrlen);
    if (r == 0) {
        ok = true;
    } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
        WSAPOLLFD pfd{};
        pfd.fd = s;
        pfd.events = POLLWRNORM;
        if (WSAPoll(&pfd, 1, timeoutMs) > 0 && (pfd.revents & POLLWRNORM) != 0) {
            int soErr = 0;
            int len = sizeof(soErr);
            if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &len) == 0 && soErr == 0)
                ok = true;
        }
    }

    u_long blocking = 0;
    ioctlsocket(s, FIONBIO, &blocking); // final transport setup selects nonblocking I/O
    return ok;
}

} // namespace

Connection::~Connection()
{
    Close();
}

Connection::Connection(Connection&& other) noexcept : socket_(other.socket_.exchange(INVALID_SOCKET)),
    interrupted_(other.interrupted_.exchange(true))
{
}

Connection& Connection::operator=(Connection&& other) noexcept
{
    if (this != &other) {
        Close();
        socket_ = other.socket_.exchange(INVALID_SOCKET);
        interrupted_ = other.interrupted_.exchange(true);
    }
    return *this;
}

std::optional<Connection> Connection::Connect(const std::string& ip, uint16_t port)
{
    if (IsUsbMuxTarget(ip)) {
        auto socket = ConnectUsbMuxTarget(ip, port);
        if (!socket)
            return std::nullopt;
        if (!PrepareConnectedSocket(*socket)) {
            closesocket(*socket);
            return std::nullopt;
        }
        return Connection(*socket);
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_NUMERICHOST;

    addrinfo* result = nullptr;
    if (getaddrinfo(ip.c_str(), std::to_string(port).c_str(), &hints, &result) != 0)
        return std::nullopt;

    SOCKET s = INVALID_SOCKET;
    for (addrinfo* p = result; p != nullptr; p = p->ai_next) {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET)
            continue;

        if (ConnectWithTimeout(s, p->ai_addr, static_cast<int>(p->ai_addrlen), kConnectTimeoutMs))
            break;

        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(result);

    if (s == INVALID_SOCKET)
        return std::nullopt;

    if (!PrepareConnectedSocket(s)) {
        closesocket(s);
        return std::nullopt;
    }

    return Connection(s);
}

bool Connection::WaitWritable(int timeoutMs)
{
    if (timeoutMs != 0) {
        const auto deadline = timeoutMs < 0 ? std::chrono::steady_clock::time_point::max() :
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        return WaitForSocket(POLLWRNORM, deadline);
    }
    SOCKET socket = socket_.load();
    if (socket == INVALID_SOCKET || interrupted_.load())
        return false;

    WSAPOLLFD pfd{};
    pfd.fd = socket;
    pfd.events = POLLWRNORM;
    int r = WSAPoll(&pfd, 1, 0);
    return !interrupted_.load() && r > 0 && (pfd.revents & POLLWRNORM) != 0 &&
           (pfd.revents & (POLLERR | POLLNVAL | POLLHUP)) == 0;
}

bool Connection::WaitForSocket(short events, std::chrono::steady_clock::time_point deadline)
{
    for (;;) {
        const SOCKET socket = socket_.load();
        if (socket == INVALID_SOCKET || interrupted_.load())
            return false;
        int waitMs = kInterruptPollMs;
        if (deadline != std::chrono::steady_clock::time_point::max()) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return false;
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            waitMs = static_cast<int>(std::clamp<int64_t>(remaining, 1, kInterruptPollMs));
        }
        WSAPOLLFD poll{socket, events, 0};
        const int result = WSAPoll(&poll, 1, waitMs);
        if (interrupted_.load() || result < 0 || (poll.revents & (POLLERR | POLLNVAL)) != 0)
            return false;
        if (result > 0 && (poll.revents & events) != 0)
            return true;
        if ((poll.revents & POLLHUP) != 0)
            return false;
        // A slice timeout is not the frame deadline. Recheck cancellation and
        // remaining total time instead of restarting a prefix/payload budget.
    }
}

bool Connection::ReadExact(uint8_t* buffer, size_t size, std::chrono::steady_clock::time_point deadline)
{
    size_t total = 0;
    while (total < size) {
        if (!WaitForSocket(POLLRDNORM, deadline))
            return false;
        const SOCKET socket = socket_.load();
        if (socket == INVALID_SOCKET || interrupted_.load())
            return false;
        const size_t chunk = std::min<size_t>(size - total, std::numeric_limits<int>::max());
        int n = recv(socket, reinterpret_cast<char*>(buffer + total), static_cast<int>(chunk), 0);
        if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
            continue;
        if (n <= 0)
            return false;
        total += static_cast<size_t>(n);
    }
    return true;
}

bool Connection::WriteExactUntil(const uint8_t* buffer, size_t size,
                                 std::chrono::steady_clock::time_point deadline)
{
    size_t total = 0;
    while (total < size) {
        if (!WaitForSocket(POLLWRNORM, deadline))
            return false;
        const SOCKET socket = socket_.load();
        if (socket == INVALID_SOCKET || interrupted_.load())
            return false;

        size_t chunk = std::min<size_t>(size - total, static_cast<size_t>(std::numeric_limits<int>::max()));
        int n = send(socket, reinterpret_cast<const char*>(buffer + total), static_cast<int>(chunk), 0);
        if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
            continue;
        if (n <= 0)
            return false;
        total += static_cast<size_t>(n);
    }
    return true;
}

std::optional<std::vector<uint8_t>> Connection::ReadFrame(int timeoutMs)
{
    if (!IsValid())
        return std::nullopt;

    uint8_t lenBytes[4];
    const auto deadline = timeoutMs < 0 ? std::chrono::steady_clock::time_point::max() :
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    if (!ReadExact(lenBytes, sizeof(lenBytes), deadline))
        return std::nullopt;

    uint32_t len = (static_cast<uint32_t>(lenBytes[0]) << 24) |
                    (static_cast<uint32_t>(lenBytes[1]) << 16) |
                    (static_cast<uint32_t>(lenBytes[2]) << 8) |
                    static_cast<uint32_t>(lenBytes[3]);

    // The iPad only ever sends us small control JSON (hello/touch/scroll/kf,
    // all < 32 KB per spec §3). A length larger than this cap means a corrupt
    // or desynced stream — refuse it rather than attempt a multi-GB allocation
    // from an attacker/garbage-controlled 32-bit length.
    constexpr uint32_t kMaxInboundFrame = 1u << 20; // 1 MiB, generous
    if (len > kMaxInboundFrame)
        return std::nullopt;

    std::vector<uint8_t> payload(len);
    if (len > 0 && !ReadExact(payload.data(), len, deadline))
        return std::nullopt;

    return payload;
}

bool Connection::SendFrame(const uint8_t* data, uint32_t size, int timeoutMs)
{
    // Receiver pings are handled on the reader thread while video is written
    // by the capture thread. Serialize the complete length+payload pair so a
    // pong can never splice itself into an H.264 frame on the TCP stream.
    std::lock_guard<std::mutex> lock(sendMutex_);
    if (!IsValid())
        return false;
    if (timeoutMs <= 0)
        return false;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    uint8_t lenBytes[4] = {
        static_cast<uint8_t>((size >> 24) & 0xFF),
        static_cast<uint8_t>((size >> 16) & 0xFF),
        static_cast<uint8_t>((size >> 8) & 0xFF),
        static_cast<uint8_t>(size & 0xFF),
    };

    if (!WriteExactUntil(lenBytes, sizeof(lenBytes), deadline)) {
        // Failure may follow a partial prefix. Poison the stream before
        // releasing sendMutex_ so the reader thread cannot append a pong to
        // already desynchronized framing.
        Interrupt();
        return false;
    }

    if (size > 0 && !WriteExactUntil(data, size, deadline)) {
        Interrupt();
        return false;
    }

    return true;
}

void Connection::Interrupt()
{
    interrupted_ = true;
    std::lock_guard<std::mutex> lock(closeMutex_);
    SOCKET socket = socket_.load();
    if (socket != INVALID_SOCKET)
        shutdown(socket, SD_BOTH);
}

void Connection::Close()
{
    interrupted_ = true;
    std::lock_guard<std::mutex> lock(closeMutex_);
    SOCKET socket = socket_.exchange(INVALID_SOCKET);
    if (socket != INVALID_SOCKET) {
        shutdown(socket, SD_BOTH);
        closesocket(socket);
    }
}

} // namespace od
