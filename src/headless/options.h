#pragma once
#include <string>

namespace headless {
struct Options {
    std::string script, host, modules;
    int port = 0;
};

// Owns the process lock for the whole session. Profiles contain no credentials.
class ProfileLock {
public:
    ~ProfileLock();
    void acquire(const std::string& path);
private:
    int fd = -1;
};
Options parseOptions(int argc, char** argv, ProfileLock& lock);
}
