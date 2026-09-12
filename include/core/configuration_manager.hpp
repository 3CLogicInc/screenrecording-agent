#pragma once

#include <string>
#include <mutex>
#include <vector>
#include "../types.hpp"

namespace screenrec {

class ConfigurationManager {
public:
    ConfigurationManager();
    ~ConfigurationManager();
    static std::string defaultConfigPath();
    bool loadFromFile(const std::string& filePath);
    bool saveToFile(const std::string& filePath) const;
    bool saveToLoadedFile() const;
    bool loadFromJson(const std::string& jsonString);
    std::string saveToJson() const;
    std::vector<std::string> loadWarnings() const;

    Configuration getConfiguration() const;

    RecordingConfig getRecordingConfig() const;
    void setRecordingConfig(const RecordingConfig& config);

    StorageConfig getStorageConfig() const;

    UploadConfig getUploadConfig() const;
    void setUploadConfig(const UploadConfig& config);

    LoggingConfig getLoggingConfig() const;
    void setLoggingConfig(const LoggingConfig& config);

    CallCenterInfo getCallcenterInfo() const;
    void setCallcenterInfo(const CallCenterInfo& info);

    ServerConfig getServerConfig() const;

private:
    Configuration config_;
    std::string configFilePath_;
    std::vector<std::string> loadWarnings_;
    mutable std::mutex mutex_;
};

}
