#include "platform/posix/http/http_server.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

namespace {

struct Socket {
    HttpSocket::Handle mValue = HttpSocket::kInvalid;
    ~Socket()
    {
        if (mValue != HttpSocket::kInvalid) {
            HttpSocket::Close(mValue);
        }
    }
};

struct Server {
    HttpSocket::Runtime mSockets;
    EarTunerControlState mControl;
    PosixHttpServerState mState;
    std::thread mThread;
    uint16_t mPort = 0;

    void Start()
    {
        REQUIRE(mSockets.Error() == 0);
        mState.mPort = 0;
        mState.mDiagnostics = [] { return "{\"platform\":\"wasapi\",\"audio_ready\":true}"; };
        mThread = std::thread([this] { RunHttpServer(mControl, mState); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (mState.mServerFd.load() == HttpSocket::kInvalid &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        REQUIRE(mState.mServerFd.load() != HttpSocket::kInvalid);
        sockaddr_in address{};
        int size = sizeof(address);
        REQUIRE(getsockname(mState.mServerFd.load(), reinterpret_cast<sockaddr *>(&address),
                            &size) == 0);
        mPort = ntohs(address.sin_port);
    }
    ~Server()
    {
        StopHttpServer(mState);
        if (mThread.joinable()) {
            mThread.join();
        }
    }
    std::string Request(const std::string &request)
    {
        Socket client{socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
        REQUIRE(client.mValue != HttpSocket::kInvalid);
        DWORD timeout = 2000;
        REQUIRE(setsockopt(client.mValue, SOL_SOCKET, SO_RCVTIMEO,
                           reinterpret_cast<const char *>(&timeout), sizeof(timeout)) == 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(mPort);
        REQUIRE(connect(client.mValue, reinterpret_cast<sockaddr *>(&address), sizeof(address)) ==
                0);
        REQUIRE(send(client.mValue, request.data(), static_cast<int>(request.size()), 0) ==
                static_cast<int>(request.size()));
        std::string response;
        char buffer[1024];
        for (;;) {
            const int count = recv(client.mValue, buffer, sizeof(buffer), 0);
            REQUIRE(count >= 0);
            if (count == 0) {
                return response;
            }
            response.append(buffer, static_cast<size_t>(count));
        }
    }
};

}  // namespace

TEST_CASE("Winsock serves the shared versioned control and diagnostics", "[windows][http]")
{
    Server server;
    server.Start();
    const auto response = server.Request(
        "GET /api/v1/control?focus_enabled=1&left_mode=2&left_level=3&right_mode=1&right_level=2 "
        "HTTP/1.1\r\nHost: localhost\r\n\r\n");
    REQUIRE(response.find("200 OK") != std::string::npos);
    REQUIRE(server.mControl.mFocusEnabled.load());
    REQUIRE(server.mControl.mLeftFocusMode.load() == 2);
    REQUIRE(server.mControl.mRightFocusMode.load() == 1);
    REQUIRE(server.mControl.mLeftFocusLevel.load() == 3);
    const auto diagnostics =
        server.Request("GET /api/v1/diagnostics HTTP/1.1\r\nHost: localhost\r\n\r\n");
    REQUIRE(diagnostics.find("\"platform\":\"wasapi\"") != std::string::npos);
    const auto bad = server.Request("GET /unrelated HTTP/1.1\r\nHost: localhost\r\n\r\n");
    REQUIRE(bad.find("400 Bad Request") != std::string::npos);
}

TEST_CASE("Winsock shutdown interrupts an incomplete request", "[windows][http]")
{
    Server server;
    server.Start();
    Socket client{socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
    REQUIRE(client.mValue != HttpSocket::kInvalid);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(server.mPort);
    REQUIRE(connect(client.mValue, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    REQUIRE(send(client.mValue, "GET /", 5, 0) == 5);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const auto start = std::chrono::steady_clock::now();
    StopHttpServer(server.mState);
    server.mThread.join();
    REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
}
