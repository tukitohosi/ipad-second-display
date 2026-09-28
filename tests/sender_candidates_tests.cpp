#ifdef NDEBUG
#undef NDEBUG
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include "app/SenderApp.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
enum class Reply { Silent, PartialPrefix, WrongIdentity, InvalidPanel };

bool WaitUntil(const std::function<bool()>& condition, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return condition();
}

bool SendAll(SOCKET socket, const char* bytes, size_t length)
{
    while (length) {
        const int sent = send(socket, bytes, static_cast<int>(length), 0);
        if (sent <= 0) return false;
        bytes += sent;
        length -= static_cast<size_t>(sent);
    }
    return true;
}

// Every socket binds a specific loopback address. This receiver never advertises
// mDNS, sends a valid panel, or invokes any display/network configuration API.
class LoopbackReceiver {
public:
    LoopbackReceiver(const char* address, uint16_t port, Reply reply, std::string identity)
        : reply_(reply), identity_(std::move(identity))
    {
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        assert(listener_ != INVALID_SOCKET);
        BOOL exclusive = TRUE;
        assert(setsockopt(listener_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                          reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == 0);
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(port);
        assert(inet_pton(AF_INET, address, &local.sin_addr) == 1);
        assert((ntohl(local.sin_addr.s_addr) >> 24) == 127);
        assert(bind(listener_, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == 0);
        int length = sizeof(local);
        assert(getsockname(listener_, reinterpret_cast<sockaddr*>(&local), &length) == 0);
        port_ = ntohs(local.sin_port);
        assert(listen(listener_, 2) == 0);
        worker_ = std::thread([this] { Run(); });
    }

    ~LoopbackReceiver()
    {
        stopping_ = true;
        worker_.join();
        closesocket(listener_);
    }

    uint16_t Port() const { return port_; }
    unsigned Accepted() const { return accepted_.load(); }
    unsigned ReceivedBytes() const { return receivedBytes_.load(); }

private:
    static bool Readable(SOCKET socket)
    {
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(socket, &ready);
        timeval wait{};
        wait.tv_usec = 20'000;
        return select(0, &ready, nullptr, nullptr, &wait) > 0;
    }

    void Run()
    {
        while (!stopping_) {
            if (!Readable(listener_)) continue;
            SOCKET client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) continue;
            ++accepted_;
            DWORD timeout = 1000;
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            if (reply_ == Reply::PartialPrefix) {
                const char fragment[2]{};
                SendAll(client, fragment, sizeof(fragment));
            } else if (reply_ != Reply::Silent) {
                const std::string id = reply_ == Reply::WrongIdentity ? "different-receiver" : identity_;
                const std::string hello = "{\"type\":\"hello\",\"pixelsWide\":1,\"pixelsHigh\":1,\"scale\":1,"
                                          "\"device\":\"iPad\",\"id\":\"" + id + "\",\"pv\":3}";
                const uint32_t length = htonl(static_cast<uint32_t>(hello.size()));
                if (SendAll(client, reinterpret_cast<const char*>(&length), sizeof(length)))
                    SendAll(client, hello.data(), hello.size());
            }
            while (!stopping_) {
                if (!Readable(client)) continue;
                char bytes[1024];
                const int count = recv(client, bytes, sizeof(bytes), 0);
                if (count <= 0) break;
                receivedBytes_ += static_cast<unsigned>(count);
            }
            shutdown(client, SD_BOTH);
            closesocket(client);
        }
    }

    SOCKET listener_ = INVALID_SOCKET;
    uint16_t port_ = 0;
    Reply reply_;
    std::string identity_;
    std::atomic<bool> stopping_{false};
    std::atomic<unsigned> accepted_{0}, receivedBytes_{0};
    std::thread worker_;
};

od::StreamSettings TestSettings(std::vector<std::string> targets)
{
    od::StreamSettings settings;
    settings.candidateTargets = std::move(targets);
    settings.requirePrivateNetwork = true; // production loopback exemption, no network-policy bypass
    // Even if panel validation regresses, the test must not create a VDD.
    settings.existingDisplayName = L"\\\\.\\DISPLAY_CANDIDATE_TEST_UNUSED";
    return settings;
}

std::string TestId(const char* scenario)
{
    return "candidate-test-" + std::to_string(GetCurrentProcessId()) + "-" + scenario;
}

void ExpectSecondCandidate(Reply firstReply, const char* scenario, bool refreshWhileWaiting)
{
    const std::string identity = TestId(scenario);
    LoopbackReceiver first("127.0.0.1", 0, firstReply, identity);
    LoopbackReceiver second("127.0.0.2", first.Port(), Reply::InvalidPanel, identity);
    od::SenderApp sender;
    const auto started = std::chrono::steady_clock::now();
    sender.Start("127.0.0.1", first.Port(),
                 TestSettings(refreshWhileWaiting ? std::vector<std::string>{"127.0.0.1"} :
                              std::vector<std::string>{"127.0.0.1", "127.0.0.2"}), identity);
    assert(WaitUntil([&] { return first.Accepted() > 0; }, 3s));
    if (refreshWhileWaiting) {
        sender.UpdateConnectionCandidates({"127.0.0.1", "127.0.0.2", "127.0.0.2", ""});
        assert(sender.Snapshot().candidateCount == 2);
    }
    assert(WaitUntil([&] {
        const auto snapshot = sender.Snapshot();
        return snapshot.connectedAddress == "127.0.0.2" && snapshot.failure == od::FailureReason::InvalidPanel;
    }, 7s));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    if (firstReply == Reply::Silent || firstReply == Reply::PartialPrefix) assert(elapsed >= 2500ms);
    assert(elapsed < 8s);
    const auto snapshot = sender.Snapshot();
    assert(snapshot.deviceId == identity);
    assert(snapshot.width == 0 && snapshot.height == 0);
    assert(snapshot.displayName.empty());
    assert(sender.GetState() != od::SenderApp::State::Streaming);
    assert(first.ReceivedBytes() == 0); // no welcome/input/video for wrong or missing identity
    assert(WaitUntil([&] { return second.ReceivedBytes() > 0; }, 1s));
    const auto stopping = std::chrono::steady_clock::now();
    sender.Stop();
    assert(std::chrono::steady_clock::now() - stopping < 1500ms);
    assert(!sender.IsRunning());
    std::printf("candidate scenario passed: %s\n", scenario);
}

void StopDuringHello()
{
    const std::string identity = TestId("stop");
    LoopbackReceiver receiver("127.0.0.3", 0, Reply::PartialPrefix, identity);
    od::SenderApp sender;
    sender.Start("127.0.0.3", receiver.Port(), TestSettings({"127.0.0.3"}), identity);
    assert(WaitUntil([&] {
        return receiver.Accepted() > 0 && sender.Snapshot().phase == od::ConnectionPhase::WaitingHello;
    }, 3s));
    const auto started = std::chrono::steady_clock::now();
    sender.Stop();
    assert(std::chrono::steady_clock::now() - started < 1500ms);
    assert(!sender.IsRunning());
    assert(sender.Snapshot().connectedAddress.empty());
    assert(sender.Snapshot().displayName.empty());
    assert(receiver.ReceivedBytes() == 0);
    std::puts("candidate scenario passed: stop during partial hello");
}

} // namespace

int main()
{
    WSADATA winsock{};
    assert(WSAStartup(MAKEWORD(2, 2), &winsock) == 0);
    ExpectSecondCandidate(Reply::Silent, "silent-fallback", false);
    ExpectSecondCandidate(Reply::WrongIdentity, "identity-fallback", false);
    ExpectSecondCandidate(Reply::PartialPrefix, "partial-fallback", false);
    ExpectSecondCandidate(Reply::Silent, "refresh-candidates", true);
    StopDuringHello();
    WSACleanup();
    return 0;
}
