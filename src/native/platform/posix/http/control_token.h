#pragma once

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <stdexcept>
#include <string>

namespace ControlToken {

class Descriptor {
   public:
    explicit Descriptor(int fd)
        : mFd(fd)
    {
        if (fd < 0) {
            throw std::runtime_error("Could not open private native control credentials");
        }
    }
    ~Descriptor() { close(mFd); }
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
    int Get() const { return mFd; }

   private:
    int mFd;
};

inline bool Valid(const std::string &token)
{
    return token.size() == 64 && token.find_first_not_of("0123456789abcdef") == std::string::npos;
}

inline std::string Read(const std::string &path)
{
    const auto separator = path.find_last_of('/');
    if (separator == std::string::npos || separator + 1 == path.size()) {
        throw std::runtime_error("Native control token requires an explicit private directory");
    }
    Descriptor directory(
        open(path.substr(0, separator).c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat info{};
    if (fstat(directory.Get(), &info) != 0 || info.st_uid != geteuid() ||
        (info.st_mode & 0777) != 0700) {
        throw std::runtime_error(
            "Native control credential directory must be private and user-owned");
    }
    Descriptor file(openat(directory.Get(), path.substr(separator + 1).c_str(),
                           O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (fstat(file.Get(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != geteuid() ||
        (info.st_mode & 0777) != 0600 || info.st_size != 65) {
        throw std::runtime_error("Native control token must be a private user-owned regular file");
    }
    std::array<char, 65> bytes{};
    size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = read(file.Get(), bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            throw std::runtime_error("Could not read complete native control credentials");
        }
        offset += static_cast<size_t>(count);
    }
    const std::string token(bytes.data(), 64);
    if (bytes.back() != '\n' || !Valid(token)) {
        throw std::runtime_error("Malformed native control credentials");
    }
    return token;
}

}  // namespace ControlToken
