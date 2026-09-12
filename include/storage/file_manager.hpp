#pragma once

#include <string>
#include "../types.hpp"

namespace screenrec {

class FileManager {
public:
    explicit FileManager(const StorageConfig& config);

    std::string createOutputFile(const SessionId& sessionId, const std::string& extension) const;

private:
    StorageConfig config_;

    std::string getSessionDirectory(const SessionId& sessionId) const;
    static void ensureDirectoryExists(const std::string& path);
};

}
