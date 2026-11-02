#include <catch2/catch_test_macros.hpp>

#include "platform/posix/http/http_server.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

namespace {

class HttpServerHarness {
   public:
    HttpServerHarness() { mServer.mPort = 0; }

    ~HttpServerHarness() { Stop(); }

    void Stop()
    {
        StopHttpServer(mServer);
        if (mThread.joinable()) {
            mThread.join();
        }
    }

    void Start() { mThread = std::thread(RunHttpServer, std::ref(mControl), std::ref(mServer)); }

    bool WaitUntilReady() const
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            if (mServer.mServerFd.load(std::memory_order_relaxed) >= 0) {
                return true;
            }
            std::this_thread::yield();
        }
        return false;
    }

    std::string Request(const std::string &path) const
    {
        const int clientFd = Connect();

        const std::string request = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
        const ssize_t sent = send(clientFd, request.data(), request.size(), 0);
        REQUIRE(sent == static_cast<ssize_t>(request.size()));
        shutdown(clientFd, SHUT_WR);

        std::string response;
        char buffer[256];
        ssize_t received = 0;
        while ((received = recv(clientFd, buffer, sizeof(buffer), 0)) > 0) {
            response.append(buffer, static_cast<size_t>(received));
        }
        close(clientFd);
        return response;
    }

    int Connect() const
    {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(fd >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(Port());
        REQUIRE(connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
        timeval timeout{2, 0};
        REQUIRE(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
        return fd;
    }

    uint16_t Port() const
    {
        sockaddr_in address{};
        socklen_t addressSize = sizeof(address);
        const int serverFd = mServer.mServerFd.load(std::memory_order_relaxed);
        REQUIRE(serverFd >= 0);
        REQUIRE(getsockname(serverFd, reinterpret_cast<sockaddr *>(&address), &addressSize) == 0);
        return ntohs(address.sin_port);
    }

    EarTunerControlState mControl;

    void SetDiagnostics(std::function<std::string()> diagnostics)
    {
        mServer.mDiagnostics = std::move(diagnostics);
    }

   private:
    PosixHttpServerState mServer;
    std::thread mThread;
};

}  // namespace

TEST_CASE("POSIX HTTP server exposes read-only diagnostics")
{
    HttpServerHarness server;
    server.SetDiagnostics([] { return "{\"callbacks\":42}"; });
    server.Start();
    REQUIRE(server.WaitUntilReady());

    const std::string response = server.Request("/api/v1/diagnostics");

    REQUIRE(response.find("HTTP/1.1 200 OK") != std::string::npos);
    REQUIRE(response.find("Content-Type: application/json") != std::string::npos);
    REQUIRE(response.find("{\"callbacks\":42}") != std::string::npos);
}

TEST_CASE("POSIX HTTP server applies focus controls and gates bypass state")
{
    HttpServerHarness server;
    server.Start();
    REQUIRE(server.WaitUntilReady());

    const std::string response = server.Request(
        "/api/v1/control?focus_enabled=1&focus_bypass=1&left_mode=9&left_level=9&"
        "right_mode=-1&right_level=0&tune_voice_1=5&output_gain=2");

    REQUIRE(response.find("HTTP/1.1 200 OK") != std::string::npos);
    REQUIRE(response.find("X-Ear-Tuner-API-Version: 1") != std::string::npos);
    REQUIRE(server.mControl.mFocusEnabled.load(std::memory_order_relaxed));
    REQUIRE(server.mControl.mFocusBypassed.load(std::memory_order_relaxed));
    REQUIRE(server.mControl.mLeftFocusMode.load(std::memory_order_relaxed) == 4);
    REQUIRE(server.mControl.mLeftFocusLevel.load(std::memory_order_relaxed) == 3);
    REQUIRE(server.mControl.mRightFocusMode.load(std::memory_order_relaxed) == 0);
    REQUIRE(server.mControl.mRightFocusLevel.load(std::memory_order_relaxed) == 1);
    REQUIRE(server.mControl.mOutputGain.load(std::memory_order_relaxed) == 1.5f);
    REQUIRE(SnapshotDspTuning(server.mControl).mVoiceGainsDb[1] == 5.0f);

    const std::string disabledResponse = server.Request("/api/v1/control?focus_enabled=0");

    REQUIRE(disabledResponse.find("HTTP/1.1 200 OK") != std::string::npos);
    REQUIRE_FALSE(server.mControl.mFocusEnabled.load(std::memory_order_relaxed));
    REQUIRE_FALSE(server.mControl.mFocusBypassed.load(std::memory_order_relaxed));
}

TEST_CASE("POSIX HTTP server rejects unsupported control paths")
{
    HttpServerHarness server;
    server.Start();
    REQUIRE(server.WaitUntilReady());

    const std::string response = server.Request("/api/v2/control?focus_enabled=1");

    REQUIRE(response.find("HTTP/1.1 400 Bad Request") != std::string::npos);
}

TEST_CASE("POSIX HTTP server rejects obsolete output route controls")
{
    HttpServerHarness server;
    server.Start();
    REQUIRE(server.WaitUntilReady());

    const std::string response = server.Request("/api/v1/control?output_sink=deck-headphones");

    REQUIRE(response.find("HTTP/1.1 400 Bad Request") != std::string::npos);
}

TEST_CASE("POSIX HTTP server expires idle clients so later controls can proceed")
{
    HttpServerHarness server;
    server.Start();
    REQUIRE(server.WaitUntilReady());
    const int idle = server.Connect();
    const auto started = std::chrono::steady_clock::now();
    const auto response = server.Request("/api/v1/control?focus_enabled=1");
    const auto elapsed = std::chrono::steady_clock::now() - started;
    close(idle);
    REQUIRE(response.find("200 OK") != std::string::npos);
    REQUIRE(elapsed < std::chrono::seconds(2));
    REQUIRE(server.mControl.mFocusEnabled.load());
}

TEST_CASE("POSIX HTTP server assembles fragmented headers before applying controls")
{
    HttpServerHarness server;
    server.Start();
    REQUIRE(server.WaitUntilReady());
    const int fd = server.Connect();
    const std::string prefix = "GET /api/v1/control?focus_enabled=";
    REQUIRE(send(fd, prefix.data(), prefix.size(), 0) == static_cast<ssize_t>(prefix.size()));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const std::string suffix = "1 HTTP/1.1\r\nHost: localhost\r\n\r\n";
    REQUIRE(send(fd, suffix.data(), suffix.size(), 0) == static_cast<ssize_t>(suffix.size()));
    char response[256]{};
    const ssize_t received = recv(fd, response, sizeof(response), 0);
    close(fd);
    REQUIRE(received > 0);
    REQUIRE(std::string(response, static_cast<size_t>(received)).find("200 OK") !=
            std::string::npos);
    REQUIRE(server.mControl.mFocusEnabled.load());
}

TEST_CASE("POSIX HTTP server stops promptly with an idle client")
{
    HttpServerHarness server;
    server.Start();
    REQUIRE(server.WaitUntilReady());
    const int idle = server.Connect();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto started = std::chrono::steady_clock::now();
    server.Stop();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    close(idle);
    REQUIRE(elapsed < std::chrono::milliseconds(500));
}

TEST_CASE("POSIX HTTP server respects shutdown requested before startup")
{
    EarTunerControlState control;
    PosixHttpServerState state;
    StopHttpServer(state);
    RunHttpServer(control, state);
    REQUIRE(state.mServerFd.load() == -1);
}

TEST_CASE("POSIX HTTP server rejects oversized requests without applying partial controls")
{
    HttpServerHarness server;
    server.Start();
    REQUIRE(server.WaitUntilReady());
    const int fd = server.Connect();
    const std::string request =
        "GET /api/v1/control?focus_enabled=1 HTTP/1.1\r\nX-Padding: " + std::string(5000, 'x');
    REQUIRE(send(fd, request.data(), request.size(), 0) == static_cast<ssize_t>(request.size()));
    char response[256]{};
    const ssize_t received = recv(fd, response, sizeof(response), 0);
    close(fd);
    REQUIRE(received > 0);
    REQUIRE(std::string(response, static_cast<size_t>(received)).find("400 Bad Request") !=
            std::string::npos);
    REQUIRE_FALSE(server.mControl.mFocusEnabled.load());
    REQUIRE(server.Request("/api/v1/control?focus_enabled=1").find("200 OK") != std::string::npos);
}