#ifdef NDEBUG
#undef NDEBUG
#endif
#include "net/Connection.h"
#include <cassert>
#include <chrono>
#include <functional>
#include <future>
#include <cstdio>
#include <thread>

using namespace std::chrono_literals;

void WithPeer(const std::function<void(SOCKET)>& serve, const std::function<void(od::Connection&)>& check)
{
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(listener != INVALID_SOCKET);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    int length = sizeof(address);
    assert(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    assert(listen(listener, 1) == 0);
    std::thread peer([&] {
        SOCKET accepted = accept(listener, nullptr, nullptr);
        assert(accepted != INVALID_SOCKET);
        serve(accepted);
        closesocket(accepted);
    });
    auto connection = od::Connection::Connect("127.0.0.1", ntohs(address.sin_port));
    assert(connection);
    check(*connection);
    connection->Close();
    peer.join();
    closesocket(listener);
}

void CheckInterrupt(const std::vector<char>& prefix, int timeoutMs)
{
    std::promise<void> releasePeer;
    auto release = releasePeer.get_future().share();
    std::promise<void> peerReady;
    auto ready = peerReady.get_future();
    WithPeer([&](SOCKET peer) {
        if (!prefix.empty())
            assert(send(peer, prefix.data(), static_cast<int>(prefix.size()), 0) == static_cast<int>(prefix.size()));
        peerReady.set_value();
        // Keep the peer open until ReadFrame returns. The fallback prevents
        // a broken cancellation implementation from hanging the test forever.
        release.wait_for(2s);
    }, [&](auto& connection) {
        ready.wait();
        std::thread stop([&] { std::this_thread::sleep_for(30ms); connection.Interrupt(); });
        const auto start = std::chrono::steady_clock::now();
        const auto frame = connection.ReadFrame(timeoutMs);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        releasePeer.set_value();
        stop.join();
        assert(!frame);
        assert(elapsed < 350ms);
        assert(!connection.IsValid());
        assert(!connection.WaitWritable(1000));
        assert(!connection.SendFrame(reinterpret_cast<const uint8_t*>("x"), 1));
        assert(!connection.ReadFrame());
        // Moving ownership must not revive an interrupted connection.
        od::Connection moved(std::move(connection));
        assert(!moved.IsValid() && !connection.IsValid());
        connection = std::move(moved);
        assert(!connection.IsValid() && !moved.IsValid());
        std::printf("interrupt prefix=%zu timeout=%d completed in %.1f ms\n", prefix.size(), timeoutMs,
                    std::chrono::duration<double, std::milli>(elapsed).count());
    });
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    WSADATA wsa{};
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    // A segmented handshake is reassembled, not confused with a missing hello.
    WithPeer([](SOCKET peer) {
        const char header[]{0, 0, 0, 5};
        send(peer, header, 2, 0);
        std::this_thread::sleep_for(20ms);
        send(peer, header + 2, 2, 0);
        send(peer, "hello", 5, 0);
    }, [](auto& connection) {
        od::Connection moved(std::move(connection));
        assert(moved.IsValid() && !connection.IsValid());
        connection = std::move(moved);
        assert(connection.IsValid() && !moved.IsValid());
        auto frame = connection.ReadFrame(1000);
        assert(frame && std::string(frame->begin(), frame->end()) == "hello");
    });
    // An open TCP service that never speaks OpenDisplay cannot block address fallback.
    WithPeer([](SOCKET) { std::this_thread::sleep_for(350ms); }, [](auto& connection) {
        const auto start = std::chrono::steady_clock::now();
        assert(!connection.ReadFrame(100));
        assert(std::chrono::steady_clock::now() - start < 300ms);
    });
    // The same deadline covers a partial prefix and partial payload.
    WithPeer([](SOCKET peer) {
        const char prefix[]{0, 0, 0, 5, 'h'};
        send(peer, prefix, sizeof(prefix), 0);
        std::this_thread::sleep_for(350ms);
    }, [](auto& connection) {
        const auto start = std::chrono::steady_clock::now();
        assert(!connection.ReadFrame(100));
        assert(std::chrono::steady_clock::now() - start < 300ms);
    });
    // Both indefinite streaming and long handshakes cancel independently of
    // the peer, including when stopped halfway through prefix or payload.
    CheckInterrupt({}, -1);
    CheckInterrupt({0, 0}, -1);
    CheckInterrupt({0, 0, 0, 5, 'h'}, -1);
    CheckInterrupt({}, 10'000);

    // Nonblocking sends keep complete frame boundaries under contention.
    WithPeer([](SOCKET peer) {
        auto exact = [&](char* destination, size_t size) {
            size_t read = 0;
            while (read < size) {
                const int n = recv(peer, destination + read, static_cast<int>(size - read), 0);
                assert(n > 0);
                read += static_cast<size_t>(n);
            }
        };
        for (int i = 0; i < 2; ++i) {
            char header[4]{};
            exact(header, 4);
            assert(header[0] == 0 && header[1] == 0 && header[2] == 0 && header[3] == 64);
            std::vector<char> payload(64);
            exact(payload.data(), payload.size());
            assert(payload.front() == 'a' || payload.front() == 'b');
            for (char byte : payload) assert(byte == payload.front());
        }
    }, [](auto& connection) {
        std::vector<uint8_t> a(64, 'a'), b(64, 'b');
        std::thread first([&] { assert(connection.SendFrame(a.data(), static_cast<uint32_t>(a.size()))); });
        assert(connection.SendFrame(b.data(), static_cast<uint32_t>(b.size())));
        first.join();
    });

    // A stalled receiver cannot trap a writer past its whole-frame deadline;
    // partial writes poison the stream so no later control frame is appended.
    std::promise<void> releaseStalledPeer;
    auto stalledRelease = releaseStalledPeer.get_future().share();
    std::promise<void> stalledPeerReady;
    auto stalledReady = stalledPeerReady.get_future();
    WithPeer([&](SOCKET peer) {
        int smallBuffer = 1024;
        assert(setsockopt(peer, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&smallBuffer), sizeof(smallBuffer)) == 0);
        stalledPeerReady.set_value();
        stalledRelease.wait_for(3s);
    }, [&](auto& connection) {
        stalledReady.wait();
        std::vector<uint8_t> largeFrame(32 * 1024 * 1024, 'x');
        bool timedOut = false;
        std::chrono::steady_clock::duration elapsed{};
        size_t acceptedBytes = 0;
        for (int attempt = 0; attempt < 4 && !timedOut; ++attempt) {
            // Windows can accept one entire large send into its local queue
            // even when it exceeds SO_SNDBUF. A successful warmup therefore
            // does not demonstrate that the remote peer consumed anything.
            // Fill that queue with bounded sends until a subsequent prefix
            // or payload really encounters write backpressure.
            const bool writable = connection.WaitWritable(0);
            const uint32_t bytes = writable ? static_cast<uint32_t>(largeFrame.size()) : 5;
            const auto start = std::chrono::steady_clock::now();
            const bool sent = connection.SendFrame(largeFrame.data(), bytes, 100);
            elapsed = std::chrono::steady_clock::now() - start;
            std::printf("stalled send writable=%d bytes=%u result=%d completed in %.1f ms\n", writable, bytes,
                        sent, std::chrono::duration<double, std::milli>(elapsed).count());
            if (sent)
                acceptedBytes += bytes;
            else
                timedOut = true;
        }
        releaseStalledPeer.set_value();
        std::printf("stalled peer locally accepted %zu bytes before timeout\n", acceptedBytes);
        assert(timedOut && elapsed >= 70ms && elapsed < 350ms);
        assert(!connection.IsValid());
        assert(!connection.SendFrame(reinterpret_cast<const uint8_t*>("later"), 5));
    });
    WSACleanup();
}
