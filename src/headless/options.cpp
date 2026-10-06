#include "options.h"
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace headless {
ProfileLock::~ProfileLock() { if (fd >= 0) ::close(fd); }
void ProfileLock::acquire(const std::string& path) {
    fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    struct stat info{};
    if (fd < 0 || fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_uid != geteuid())
        throw std::runtime_error("profile lock unavailable or unsafe");
    if (flock(fd, LOCK_EX | LOCK_NB))
        throw std::runtime_error("profile already running");
    const auto pid = std::to_string(getpid()) + "\n";
    if (ftruncate(fd, 0) || write(fd, pid.data(), pid.size()) != static_cast<ssize_t>(pid.size()))
        throw std::runtime_error("cannot record profile lock owner");
}
static int portOf(const std::string& value) {
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("port must be an integer in [1,65535]");
    const auto n = std::stoul(value);
    if (n < 1 || n > 65535) throw std::runtime_error("port out of range");
    return static_cast<int>(n);
}
static std::string trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r");
    if (begin == std::string::npos) return {};
    return s.substr(begin, s.find_last_not_of(" \t\r") - begin + 1);
}
Options parseOptions(int argc, char** argv, ProfileLock& lock) {
    Options out;
    if (argc >= 2 && std::string(argv[1]) == "--profile") {
        if (argc != 3) throw std::runtime_error("usage: ultimate-headless --profile path");
        const auto path = std::filesystem::canonical(argv[2]);
        std::ifstream file(path);
        if (!file) throw std::runtime_error("cannot read profile");
        std::map<std::string,std::string> values;
        std::string line;
        while (std::getline(file,line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos) throw std::runtime_error("profile expects key=value");
            const auto key = trim(line.substr(0,eq));
            if (key != "script" && key != "host" && key != "port" && key != "modules")
                throw std::runtime_error("unsupported profile key (credentials forbidden)");
            if (!values.emplace(key,trim(line.substr(eq+1))).second)
                throw std::runtime_error("duplicate profile key");
        }
        if (values["script"].empty()) throw std::runtime_error("profile requires script");
        auto script = std::filesystem::path(values["script"]);
        out.script = (script.is_absolute() ? script : path.parent_path()/script).string();
        out.host = values["host"];
        if (!values["modules"].empty()) {
            auto modules = std::filesystem::path(values["modules"]);
            out.modules = (modules.is_absolute() ? modules : path.parent_path()/modules).string();
        }
        if (!values["port"].empty()) out.port = portOf(values["port"]);
        lock.acquire(path.string()+".lock");
    } else {
        if (argc != 2 && argc != 4)
            throw std::runtime_error("usage: ultimate-headless script.lua [host port] | --profile path");
        out.script = argv[1];
        if (argc == 4) { out.host = argv[2]; out.port = portOf(argv[3]); }
    }
    if ((!out.host.empty()) != (out.port != 0)) throw std::runtime_error("host and port must be provided together");
    if (!out.host.empty() && out.host != "127.0.0.1")
        throw std::runtime_error("only loopback is supported until login configuration is verified");
    if (out.modules.empty()) {
        auto installed = std::filesystem::canonical("/proc/self/exe").parent_path().parent_path()/"share/ultimate-headless/scripts";
        out.modules = (std::filesystem::is_directory(installed) ? installed : std::filesystem::current_path()/"scripts").string();
    }
    return out;
}
}
