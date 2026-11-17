#include <catch2/catch_test_macros.hpp>

#include "platform/posix/http/control_token.h"
#include "platform/posix/http/http_server.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

namespace {

class HttpServerHarness {
   public:
    HttpServerHarness()
    {
        mServer.mPort = 0;
        mServer.mToken = std::string(64, 'a');
    }

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

    std::string Request(const std::string &path,
                        const std::string &headers = "Authorization: Bearer " +
                                                     std::string(64, 'a') + "\r\n") const
    {
        const int clientFd = Connect();

        const std::string request =
            "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n" + headers + "\r\n";
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
    const std::string suffix = "1 HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer " +
                               std::string(64, 'a') + "\r\n\r\n";
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

TEST_CASE("POSIX HTTP authentication protects all controls and diagnostics before dispatch")
{
    HttpServerHarness server;
    std::atomic<int> diagnostics{0};
    server.SetDiagnostics([&] {
        ++diagnostics;
        return "{}";
    });
    server.Start();
    REQUIRE(server.WaitUntilReady());
    const std::string authorization = "Authorization: Bearer " + std::string(64, 'a') + "\r\n";
    EarTunerControlState expected;
    for (const std::string &headers :
         {std::string(""), std::string("Authorization: Bearer ") + std::string(64, 'b') + "\r\n",
          authorization + authorization,
          std::string("Origin: https://untrusted.example\r\n") + authorization,
          std::string("Origin : https://untrusted.example\r\n") + authorization,
          std::string(" Authorization: hidden\r\n") + authorization,
          std::string(": invalid\r\n") + authorization,
          std::string("Authorization: Basic invalid\r\n"),
          std::string("Authorization: Bearer ") + std::string(63, 'a') + "\r\n"}) {
        for (const auto &path : {"/api/v1/"
                                 "control?focus_enabled=1&focus_bypass=1&output_gain=1.5&left_mode="
                                 "4&left_level=3&right_mode=4&right_level=3&tune_voice_1=12",
                                 "/?focus_enabled=1&focus_bypass=1&output_gain=1.5&left_mode=4&"
                                 "left_level=3&right_mode=4&right_level=3&tune_voice_1=12",
                                 "/api/v1/diagnostics"}) {
            const auto response = server.Request(path, headers);
            REQUIRE(response.find("401 Unauthorized") != std::string::npos);
            REQUIRE(response.find("Access-Control-Allow-Origin") == std::string::npos);
            REQUIRE(server.mControl.mFocusEnabled.load() == expected.mFocusEnabled.load());
            REQUIRE(server.mControl.mFocusBypassed.load() == expected.mFocusBypassed.load());
            REQUIRE(server.mControl.mOutputGain.load() == expected.mOutputGain.load());
            REQUIRE(server.mControl.mLeftFocusMode.load() == expected.mLeftFocusMode.load());
            REQUIRE(server.mControl.mRightFocusMode.load() == expected.mRightFocusMode.load());
            REQUIRE(server.mControl.mLeftFocusLevel.load() == expected.mLeftFocusLevel.load());
            REQUIRE(server.mControl.mRightFocusLevel.load() == expected.mRightFocusLevel.load());
            REQUIRE(SnapshotDspTuning(server.mControl).mVoiceGainsDb[1] ==
                    SnapshotDspTuning(expected).mVoiceGainsDb[1]);
            REQUIRE(diagnostics.load() == 0);
        }
    }
    const auto allowed = server.Request("/?focus_enabled=1",
                                        "aUtHoRiZaTiOn: bEaReR " + std::string(64, 'a') + "\r\n");
    REQUIRE(allowed.find("200 OK") != std::string::npos);
    REQUIRE(allowed.find("X-Ear-Tuner-Auth: bearer") != std::string::npos);
    REQUIRE(allowed.find("Access-Control-Allow-Origin") == std::string::npos);
    REQUIRE(server.mControl.mFocusEnabled.load());
}

TEST_CASE("POSIX HTTP server refuses missing or malformed server credentials")
{
    EarTunerControlState control;
    for (const auto &token : {std::string(""), std::string(64, 'z')}) {
        PosixHttpServerState state;
        state.mToken = token;
        RunHttpServer(control, state);
        REQUIRE(state.mServerFd.load() == -1);
    }
}

TEST_CASE("Native control token reader rejects unsafe paths and permissions")
{
    char directory[] = "/tmp/ear-tuner-token-XXXXXX";
    REQUIRE(mkdtemp(directory) != nullptr);
    const std::string path = std::string(directory) + "/control.token";
    struct Cleanup {
        std::string file, directory;
        ~Cleanup()
        {
            unlink(file.c_str());
            rmdir(directory.c_str());
        }
    } cleanup{path, directory};
    {
        std::ofstream file(path);
        file << std::string(64, 'a') << '\n';
    }
    REQUIRE(chmod(path.c_str(), 0600) == 0);
    REQUIRE(ControlToken::Read(path) == std::string(64, 'a'));
    REQUIRE(chmod(path.c_str(), 0644) == 0);
    REQUIRE_THROWS(ControlToken::Read(path));
    REQUIRE(chmod(path.c_str(), 0600) == 0);
    REQUIRE(chmod(directory, 0755) == 0);
    REQUIRE_THROWS(ControlToken::Read(path));
    REQUIRE(chmod(directory, 0700) == 0);
    REQUIRE(unlink(path.c_str()) == 0);
    REQUIRE(symlink("/etc/passwd", path.c_str()) == 0);
    REQUIRE_THROWS(ControlToken::Read(path));
}