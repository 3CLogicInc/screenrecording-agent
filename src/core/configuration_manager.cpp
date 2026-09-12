#include "core/configuration_manager.hpp"
#include "core/config_bounds.hpp"
#include "constants.hpp"
#include "platform/process_path.hpp"
#include <json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

using json = nlohmann::json;

namespace screenrec {

namespace {

std::map<std::string, std::string> parseKitKeyPaths(const json& kitMap) {
    std::map<std::string, std::string> out;
    for (const auto& [kit, path] : kitMap.items()) {
        if (path.is_string() && !path.get<std::string>().empty()) {
            out[kit] = path.get<std::string>();
        }
    }
    return out;
}

uint32_t clampField(uint32_t value, uint32_t lo, uint32_t hi, const char* name,
    std::vector<std::string>& warnings) {
    const uint32_t clamped = std::clamp(value, lo, hi);
    if (clamped != value) {
        warnings.push_back(std::string(name) + "=" + std::to_string(value) +
                           " out of range [" + std::to_string(lo) + ".." +
                           std::to_string(hi) + "]; clamped to " +
                           std::to_string(clamped));
    }
    return clamped;
}

}

ConfigurationManager::ConfigurationManager() = default;

ConfigurationManager::~ConfigurationManager() = default;

std::string ConfigurationManager::defaultConfigPath() {
    std::string path = platform::executableDir() + "/config/config.json";
    if (!std::filesystem::exists(path)) {
        const auto systemConfig =
            std::filesystem::path(constants::CONFIG_DIR) / "config.json";
        if (std::filesystem::exists(systemConfig)) {
            path = systemConfig.string();
        }
    }
    return path;
}

bool ConfigurationManager::loadFromFile(const std::string& filePath) {
    std::ifstream file(filePath);
    if (!file.is_open()) {
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();

    if (!loadFromJson(buffer.str())) {
        return false;
    }

    {
        std::scoped_lock lock(mutex_);
        configFilePath_ = filePath;
    }

    return true;
}

bool ConfigurationManager::saveToFile(const std::string& filePath) const {
    std::string jsonString = saveToJson();

    std::ofstream file(filePath);
    if (!file.is_open()) {
        return false;
    }

    file << jsonString;
    return true;
}

bool ConfigurationManager::saveToLoadedFile() const {
    std::string filePath;
    {
        std::scoped_lock lock(mutex_);
        filePath = configFilePath_;
    }

    if (filePath.empty()) {
        return false;
    }

    return saveToFile(filePath);
}

bool ConfigurationManager::loadFromJson(const std::string& jsonString) {
    try {
        auto j = json::parse(jsonString);

        std::scoped_lock lock(mutex_);
        loadWarnings_.clear();

        if (j.contains("recording")) {
            auto& rec = j["recording"];
            if (rec.contains("video")) {
                const auto& v = rec["video"];
                const VideoConfig defaults;
                config_.recording.video.width = v.value("width", defaults.width);
                config_.recording.video.height = v.value("height", defaults.height);
                config_.recording.video.fps = v.value("fps", defaults.fps);
                config_.recording.video.bitrate = v.value("bitrate", defaults.bitrate);
                config_.recording.video.maxBitrate = v.value("max_bitrate", defaults.maxBitrate);
                config_.recording.video.keyintSec = v.value("keyint_sec", defaults.keyintSec);
                config_.recording.video.codec = v.value("codec", defaults.codec);
                config_.recording.video.preset = v.value("preset", defaults.preset);
                config_.recording.video.rateControl = v.value("rate_control", defaults.rateControl);
                config_.recording.video.crf = v.value("crf", defaults.crf);
                config_.recording.video.encoder = v.value("encoder", defaults.encoder);
                config_.recording.video.layout = v.value("layout", defaults.layout);

                // Same bounds as the /api/v1/agent/configure validator; hand-edited files are clamped, not rejected.
                namespace cb = config_bounds;
                auto& vid = config_.recording.video;
                vid.width = clampField(vid.width, cb::WIDTH_MIN, cb::WIDTH_MAX, "video.width", loadWarnings_);
                vid.height = clampField(vid.height, cb::HEIGHT_MIN, cb::HEIGHT_MAX, "video.height", loadWarnings_);
                vid.fps = clampField(vid.fps, cb::FPS_MIN, cb::FPS_MAX, "video.fps", loadWarnings_);
                vid.bitrate = clampField(vid.bitrate, cb::VIDEO_BITRATE_MIN, cb::VIDEO_BITRATE_MAX, "video.bitrate", loadWarnings_);
                vid.maxBitrate = clampField(vid.maxBitrate, cb::MAX_BITRATE_MIN, cb::MAX_BITRATE_MAX, "video.max_bitrate", loadWarnings_);
                vid.crf = clampField(vid.crf, cb::CRF_MIN, cb::CRF_MAX, "video.crf", loadWarnings_);
                vid.keyintSec = clampField(vid.keyintSec, cb::KEYINT_SEC_MIN, cb::KEYINT_SEC_MAX, "video.keyint_sec", loadWarnings_);
                if (vid.layout != "native" && vid.layout != "equalTiles") {
                    loadWarnings_.push_back("video.layout=\"" + vid.layout +
                                            "\" unrecognized; using \"" + defaults.layout + "\"");
                    vid.layout = defaults.layout;
                }
            }

            if (rec.contains("audio")) {
                const auto& a = rec["audio"];
                const AudioConfig defaults;
                config_.recording.audio.enabled = a.value("enabled", defaults.enabled);
                config_.recording.audio.sampleRate = a.value("sampleRate", defaults.sampleRate);
                config_.recording.audio.channels = a.value("channels", defaults.channels);
                config_.recording.audio.bitrate = a.value("bitrate", defaults.bitrate);
                config_.recording.audio.noiseSuppression = a.value("noiseSuppression", defaults.noiseSuppression);

                namespace cb = config_bounds;
                auto& aud = config_.recording.audio;
                aud.bitrate = clampField(aud.bitrate, cb::AUDIO_BITRATE_MIN, cb::AUDIO_BITRATE_MAX, "audio.bitrate", loadWarnings_);
                aud.channels = clampField(aud.channels, cb::AUDIO_CHANNELS_MIN, cb::AUDIO_CHANNELS_MAX, "audio.channels", loadWarnings_);
            }

            config_.recording.maxDurationSeconds = rec.value("maxDurationSeconds", 3600);
            config_.recording.outputFormat = rec.value("outputFormat", "mp4");
        }

        if (j.contains("storage")) {
            const auto& stor = j["storage"];
            config_.storage.outputDirectory = stor.value("outputDirectory", constants::recordingsDir());
            config_.storage.maxDiskUsageGB = stor.value("maxDiskUsageGB", 10);
            config_.storage.cleanupOnUpload = stor.value("cleanupOnUpload", true);
            config_.storage.retentionDays = stor.value("retentionDays", 7u);
        }

        if (j.contains("upload")) {
            const auto& upl = j["upload"];
            const UploadConfig defaults;
            config_.upload.attemptsPerRound = upl.value("attemptsPerRound", defaults.attemptsPerRound);
            config_.upload.retryDelaySeconds = upl.value("retryDelaySeconds", defaults.retryDelaySeconds);
            config_.upload.pendingRescanSeconds =
                upl.value("pendingRescanSeconds", defaults.pendingRescanSeconds);
            config_.upload.uploadThreads = upl.value("uploadThreads", defaults.uploadThreads);
            config_.upload.connectTimeoutSeconds =
                upl.value("connectTimeoutSeconds", defaults.connectTimeoutSeconds);
            config_.upload.lowSpeedLimitBytesPerSec =
                upl.value("lowSpeedLimitBytesPerSec", defaults.lowSpeedLimitBytesPerSec);
            config_.upload.lowSpeedTimeSeconds =
                upl.value("lowSpeedTimeSeconds", defaults.lowSpeedTimeSeconds);
            config_.upload.maxUploadSpeedBytesPerSec =
                upl.value("maxUploadSpeedBytesPerSec", defaults.maxUploadSpeedBytesPerSec);

            namespace cb = config_bounds;
            config_.upload.uploadThreads = clampField(config_.upload.uploadThreads,
                cb::UPLOAD_THREADS_MIN, cb::UPLOAD_THREADS_MAX, "upload.uploadThreads", loadWarnings_);
        }

        if (const auto endpoint = j.find("uploadUrlsEndpoint");
            endpoint != j.end() && endpoint->is_string()) {
            config_.upload.urlsEndpoint = endpoint->get<std::string>();
        }

        if (j.contains("logging")) {
            const auto& log = j["logging"];
            (void)logLevelFromString(log.value("level", "info"), config_.logging.level);

            config_.logging.logDirectory = log.value("logDirectory", constants::logsDir());
            config_.logging.enableFileLogging = log.value("enableFileLogging", true);
            config_.logging.enableLokiReporting = log.value("enableLokiReporting", true);
            config_.logging.lokiEndpoint = log.value("lokiEndpoint", "");
            config_.logging.lokiApiKey = log.value("lokiApiKey", "");
            config_.logging.lokiSecretKey = log.value("lokiSecretKey", "");
            config_.logging.credentialsEndpoint = log.value("credentialsEndpoint", "");

            namespace cb = config_bounds;
            const LoggingConfig defaults;
            const auto fileSizeMB = clampField(
                log.value("maxFileSizeMB",
                    static_cast<uint32_t>(defaults.maxFileSizeBytes / (1024 * 1024))),
                cb::LOG_FILE_SIZE_MB_MIN, cb::LOG_FILE_SIZE_MB_MAX,
                "logging.maxFileSizeMB", loadWarnings_);
            config_.logging.maxFileSizeBytes = static_cast<size_t>(fileSizeMB) * 1024 * 1024;
            config_.logging.maxFiles = clampField(
                log.value("maxFiles", defaults.maxFiles),
                cb::LOG_MAX_FILES_MIN, cb::LOG_MAX_FILES_MAX,
                "logging.maxFiles", loadWarnings_);
        }

        if (j.contains("logUploadCred")) {
            const auto& cred = j["logUploadCred"];
            config_.logging.lokiEndpoint = cred.value("endpoint", config_.logging.lokiEndpoint);
            config_.logging.lokiApiKey = cred.value("apiKey", config_.logging.lokiApiKey);
            config_.logging.lokiSecretKey = cred.value("secretKey", config_.logging.lokiSecretKey);
        }

        if (j.contains("callcenterInfo")) {
            const auto& cc = j["callcenterInfo"];
            config_.callcenterInfo.ccId = cc.value("ccId", "");
            config_.callcenterInfo.agentId = cc.value("agentId", "");
        }

        if (j.contains("server")) {
            const auto& srv = j["server"];
            config_.server.host = srv.value("host", "127.0.0.1");
            config_.server.ports.clear();
            if (srv.contains("ports") && srv["ports"].is_array()) {
                for (const auto& port : srv["ports"]) {
                    if (!port.is_number_unsigned()) {
                        continue;
                    }

                    const auto parsedPort = port.get<uint32_t>();
                    if (parsedPort == 0 || parsedPort > 65535) {
                        continue;
                    }

                    config_.server.ports.push_back(static_cast<uint16_t>(parsedPort));
                }
            }
            if (config_.server.ports.empty()) {
                config_.server.ports = ServerConfig{}.ports;
            }
            config_.server.enableAuth = srv.value("enableAuth", false);
            // No secrets on purpose: auth is a JWT verified against the shipped public key.
            config_.server.enableHttps = srv.value("enableHttps", true);
            config_.server.sslCertPath = srv.value("sslCertPath", "certs/localhost.crt");
            config_.server.sslKeyPath = srv.value("sslKeyPath", "certs/localhost.key");
            config_.server.jwtPublicKeyPath =
                srv.value("jwtPublicKeyPath", ServerConfig{}.jwtPublicKeyPath);
            if (srv.contains("jwtPublicKeysByKit") && srv["jwtPublicKeysByKit"].is_object()) {
                config_.server.jwtPublicKeysByKit = parseKitKeyPaths(srv["jwtPublicKeysByKit"]);
            }
            config_.server.jwtIssuer = srv.value("jwtIssuer", ServerConfig{}.jwtIssuer);
            config_.server.threadPoolSize = std::clamp<uint32_t>(
                srv.value("threadPoolSize", ServerConfig{}.threadPoolSize), 2, 64);
            config_.server.allowedOrigins.clear();
            if (srv.contains("allowedOrigins") && srv["allowedOrigins"].is_array()) {
                for (const auto& origin : srv["allowedOrigins"]) {
                    if (origin.is_string() && !origin.get<std::string>().empty()) {
                        config_.server.allowedOrigins.push_back(origin.get<std::string>());
                    }
                }
            }
        }

        return true;

    } catch (const std::exception&) {
        return false;
    }
}

std::string ConfigurationManager::saveToJson() const {
    std::scoped_lock lock(mutex_);

    json j;

    j["recording"]["video"]["width"] = config_.recording.video.width;
    j["recording"]["video"]["height"] = config_.recording.video.height;
    j["recording"]["video"]["fps"] = config_.recording.video.fps;
    j["recording"]["video"]["bitrate"] = config_.recording.video.bitrate;
    j["recording"]["video"]["max_bitrate"] = config_.recording.video.maxBitrate;
    j["recording"]["video"]["keyint_sec"] = config_.recording.video.keyintSec;
    j["recording"]["video"]["codec"] = config_.recording.video.codec;
    j["recording"]["video"]["preset"] = config_.recording.video.preset;
    j["recording"]["video"]["rate_control"] = config_.recording.video.rateControl;
    j["recording"]["video"]["crf"] = config_.recording.video.crf;
    j["recording"]["video"]["encoder"] = config_.recording.video.encoder;
    j["recording"]["video"]["layout"] = config_.recording.video.layout;
    j["recording"]["audio"]["enabled"] = config_.recording.audio.enabled;
    j["recording"]["audio"]["sampleRate"] = config_.recording.audio.sampleRate;
    j["recording"]["audio"]["channels"] = config_.recording.audio.channels;
    j["recording"]["audio"]["bitrate"] = config_.recording.audio.bitrate;
    j["recording"]["audio"]["noiseSuppression"] = config_.recording.audio.noiseSuppression;
    j["recording"]["maxDurationSeconds"] = config_.recording.maxDurationSeconds;
    j["recording"]["outputFormat"] = config_.recording.outputFormat;

    j["storage"]["outputDirectory"] = config_.storage.outputDirectory;
    j["storage"]["maxDiskUsageGB"] = config_.storage.maxDiskUsageGB;
    j["storage"]["cleanupOnUpload"] = config_.storage.cleanupOnUpload;

    j["upload"]["attemptsPerRound"] = config_.upload.attemptsPerRound;
    j["upload"]["retryDelaySeconds"] = config_.upload.retryDelaySeconds;
    j["upload"]["pendingRescanSeconds"] = config_.upload.pendingRescanSeconds;
    j["upload"]["uploadThreads"] = config_.upload.uploadThreads;
    j["upload"]["connectTimeoutSeconds"] = config_.upload.connectTimeoutSeconds;
    j["upload"]["lowSpeedLimitBytesPerSec"] = config_.upload.lowSpeedLimitBytesPerSec;
    j["upload"]["lowSpeedTimeSeconds"] = config_.upload.lowSpeedTimeSeconds;
    j["upload"]["maxUploadSpeedBytesPerSec"] = config_.upload.maxUploadSpeedBytesPerSec;
    j["uploadUrlsEndpoint"] = config_.upload.urlsEndpoint;

    j["logging"]["level"] = logLevelToString(config_.logging.level);
    j["logging"]["logDirectory"] = config_.logging.logDirectory;
    j["logging"]["enableFileLogging"] = config_.logging.enableFileLogging;
    j["logging"]["maxFileSizeMB"] =
        static_cast<uint32_t>(config_.logging.maxFileSizeBytes / (1024 * 1024));
    j["logging"]["maxFiles"] = config_.logging.maxFiles;
    j["logging"]["enableLokiReporting"] = config_.logging.enableLokiReporting;
    j["logging"]["credentialsEndpoint"] = config_.logging.credentialsEndpoint;

    // Loki credentials deliberately not serialized: memory only, never in this world-readable file.

    j["server"]["host"] = config_.server.host;
    j["server"]["ports"] = config_.server.ports.empty()
                               ? json(ServerConfig{}.ports)
                               : json(config_.server.ports);
    j["server"]["enableAuth"] = config_.server.enableAuth;
    j["server"]["enableHttps"] = config_.server.enableHttps;
    j["server"]["sslCertPath"] = config_.server.sslCertPath;
    j["server"]["sslKeyPath"] = config_.server.sslKeyPath;
    j["server"]["jwtPublicKeyPath"] = config_.server.jwtPublicKeyPath;
    j["server"]["jwtPublicKeysByKit"] = config_.server.jwtPublicKeysByKit;
    j["server"]["jwtIssuer"] = config_.server.jwtIssuer;
    j["server"]["threadPoolSize"] = config_.server.threadPoolSize;
    j["server"]["allowedOrigins"] = config_.server.allowedOrigins;

    j["callcenterInfo"]["ccId"] = config_.callcenterInfo.ccId;
    j["callcenterInfo"]["agentId"] = config_.callcenterInfo.agentId;

    return j.dump(2);
}

Configuration ConfigurationManager::getConfiguration() const {
    std::scoped_lock lock(mutex_);
    return config_;
}

std::vector<std::string> ConfigurationManager::loadWarnings() const {
    std::scoped_lock lock(mutex_);
    return loadWarnings_;
}

RecordingConfig ConfigurationManager::getRecordingConfig() const {
    std::scoped_lock lock(mutex_);
    return config_.recording;
}

void ConfigurationManager::setRecordingConfig(const RecordingConfig& config) {
    {
        std::scoped_lock lock(mutex_);
        config_.recording = config;
    }
}

StorageConfig ConfigurationManager::getStorageConfig() const {
    std::scoped_lock lock(mutex_);
    return config_.storage;
}

UploadConfig ConfigurationManager::getUploadConfig() const {
    std::scoped_lock lock(mutex_);
    return config_.upload;
}

void ConfigurationManager::setUploadConfig(const UploadConfig& config) {
    std::scoped_lock lock(mutex_);
    config_.upload = config;
}

LoggingConfig ConfigurationManager::getLoggingConfig() const {
    std::scoped_lock lock(mutex_);
    return config_.logging;
}

void ConfigurationManager::setLoggingConfig(const LoggingConfig& config) {
    {
        std::scoped_lock lock(mutex_);
        config_.logging = config;
    }
}

CallCenterInfo ConfigurationManager::getCallcenterInfo() const {
    std::scoped_lock lock(mutex_);
    return config_.callcenterInfo;
}

void ConfigurationManager::setCallcenterInfo(const CallCenterInfo& info) {
    {
        std::scoped_lock lock(mutex_);
        config_.callcenterInfo = info;
    }
}

ServerConfig ConfigurationManager::getServerConfig() const {
    std::scoped_lock lock(mutex_);
    return config_.server;
}

}
