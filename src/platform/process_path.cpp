#include "platform/process_path.hpp"

#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <climits>
#include <unistd.h>
#endif

namespace screenrec::platform {

std::string executablePath() {
#ifdef _WIN32
    char path[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    return std::string(path, n);
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return "";
    }
    buffer.resize(buffer.find('\0')); // trim past the NUL dyld wrote
    // _NSGetExecutablePath can return a symlinked/relative path.
    std::error_code ec;
    const auto canonical = std::filesystem::canonical(buffer, ec);
    return ec ? buffer : canonical.string();
#else
    char result[PATH_MAX];
    const ssize_t count = readlink("/proc/self/exe", result, PATH_MAX);
    return std::string(result, count > 0 ? static_cast<size_t>(count) : 0);
#endif
}

std::string executableDir() {
    const std::string exePath = executablePath();
    const size_t lastSlash = exePath.find_last_of("/\\");
    if (lastSlash == std::string::npos) {
        return ".";
    }
    return exePath.substr(0, lastSlash);
}

}
