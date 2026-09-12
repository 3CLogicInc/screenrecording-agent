#include "storage/file_manager.hpp"
#include <chrono>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

namespace screenrec {

FileManager::FileManager(const StorageConfig& config)
    : config_(config) {
    ensureDirectoryExists(config.outputDirectory);
}

std::string FileManager::createOutputFile(const SessionId& sessionId, const std::string& extension) const {
    std::string sessionDir = getSessionDirectory(sessionId);
    ensureDirectoryExists(sessionDir);

    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::system_clock::to_time_t(now);

    std::stringstream ss;
    ss << sessionDir << "/" << sessionId << "_" << timestamp << "." << extension;

    return ss.str();
}

std::string FileManager::getSessionDirectory(const SessionId& sessionId) const {
    return config_.outputDirectory + "/" + sessionId;
}

void FileManager::ensureDirectoryExists(const std::string& path) {
    try {
        fs::create_directories(path);
    } catch (const fs::filesystem_error&) {
        // Creation failure surfaces later as an unwritable output path.
    }
}

}
