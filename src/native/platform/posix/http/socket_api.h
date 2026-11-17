#pragma once

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstring>
#include <string>

namespace HttpSocket {

#ifdef _WIN32
using Handle = SOCKET;
using PollDescriptor = WSAPOLLFD;
constexpr Handle kInvalid = INVALID_SOCKET;
#else
using Handle = int;
using PollDescriptor = pollfd;
constexpr Handle kInvalid = -1;
#endif

class Runtime {
   public:
    Runtime()
    {
#ifdef _WIN32
        WSADATA data{};
        mError = WSAStartup(MAKEWORD(2, 2), &data);
#endif
    }
    ~Runtime()
    {
#ifdef _WIN32
        if (mError == 0) {
            WSACleanup();
        }
#endif
    }
    Runtime(const Runtime &) = delete;
    Runtime &operator=(const Runtime &) = delete;
    int Error() const { return mError; }

   private:
    int mError = 0;
};

inline std::string ErrorMessage()
{
#ifdef _WIN32
    return "Winsock error " + std::to_string(WSAGetLastError());
#else
    return std::strerror(errno);
#endif
}

inline bool Retryable()
{
#ifdef _WIN32
    const int error = WSAGetLastError();
    return error == WSAEINTR || error == WSAEWOULDBLOCK;
#else
    return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

inline int Poll(PollDescriptor *descriptor, int timeout)
{
#ifdef _WIN32
    return WSAPoll(descriptor, 1, timeout);
#else
    return poll(descriptor, 1, timeout);
#endif
}

inline bool Nonblocking(Handle socket)
{
#ifdef _WIN32
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

inline void Close(Handle socket)
{
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

}  // namespace HttpSocket
