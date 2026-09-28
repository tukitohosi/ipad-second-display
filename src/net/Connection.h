#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// Prevent windows.h from pulling in winsock.h before winsock2.h; caller must
// only ever include this header, never <windows.h> first, in TUs that use it.
#include <winsock2.h>

namespace od {

// One TCP connection to the iPad receiver at <ip>:9000, with the wire framing
// ([4-byte big-endian length][payload], identical in both directions) built in.
// We are the connecting side; the iPad listens (spec §1).
class Connection {
public:
    Connection() = default;
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;

    // Connects to ip:port, sets TCP_NODELAY. Returns nullopt on failure.
    static std::optional<Connection> Connect(const std::string& ip, uint16_t port);

    // Blocking read of the next framed message. Returns nullopt on
    // disconnect/socket error.
    // A nonnegative timeout bounds the complete prefix + payload (handshake).
    // Streaming retains the default indefinite wait, interrupted by Stop().
    // Internally all I/O is nonblocking with bounded, interruptible polling.
    std::optional<std::vector<uint8_t>> ReadFrame(int timeoutMs = -1);

    // Sends one length-prefixed frame within a whole-frame deadline. False can
    // mean the wire contains only a prefix/partial payload; callers must then
    // discard this connection immediately and reconnect, never send another
    // frame on the desynchronized stream.
    bool SendFrame(const uint8_t* data, uint32_t size, int timeoutMs = 1000);

    // Returns true if the socket can accept data now (send buffer has room).
    // Used for backpressure: the caller drops a whole frame rather than
    // blocking on a congested link and building latency. timeoutMs 0 = poll.
    bool WaitWritable(int timeoutMs);

    bool IsValid() const { return !interrupted_.load() && socket_.load() != INVALID_SOCKET; }

    // Cancels reads/writes from another thread without releasing the
    // numeric socket handle. The owner calls Close only after worker threads
    // have joined, preventing a closed handle value from being reused by a
    // different connection while an old thread still holds it locally.
    void Interrupt();
    void Close();

private:
    explicit Connection(SOCKET s) : socket_(s) {}

    bool ReadExact(uint8_t* buffer, size_t size, std::chrono::steady_clock::time_point deadline);
    bool WriteExactUntil(const uint8_t* buffer, size_t size,
                         std::chrono::steady_clock::time_point deadline);
    bool WaitForSocket(short events, std::chrono::steady_clock::time_point deadline);

    // Stop() interrupts the socket from the UI thread while the worker polls
    // or does nonblocking I/O. Atomic ownership avoids a handle data race.
    std::atomic<SOCKET> socket_{INVALID_SOCKET};
    std::atomic<bool> interrupted_{false};
    // Serializes Interrupt's load+shutdown with Close's exchange+close. It is
    // intentionally not held during polling or I/O, so interruption does not
    // wait for a frame deadline or the remote endpoint.
    std::mutex closeMutex_;
    std::mutex sendMutex_;
};

} // namespace od
