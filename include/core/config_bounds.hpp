#pragma once

#include <cstdint>

namespace screenrec::config_bounds {
inline constexpr uint32_t WIDTH_MIN = 1;
inline constexpr uint32_t WIDTH_MAX = 7680;
inline constexpr uint32_t HEIGHT_MIN = 1;
inline constexpr uint32_t HEIGHT_MAX = 4320;
inline constexpr uint32_t FPS_MIN = 1;
inline constexpr uint32_t FPS_MAX = 120;
inline constexpr uint32_t VIDEO_BITRATE_MIN = 1;
inline constexpr uint32_t VIDEO_BITRATE_MAX = 50000000;
inline constexpr uint32_t MAX_BITRATE_MIN = 0;
inline constexpr uint32_t MAX_BITRATE_MAX = 50000000;
inline constexpr uint32_t CRF_MIN = 0;
inline constexpr uint32_t CRF_MAX = 51;
inline constexpr uint32_t KEYINT_SEC_MIN = 1;
inline constexpr uint32_t KEYINT_SEC_MAX = 60;
inline constexpr uint32_t AUDIO_BITRATE_MIN = 1;
inline constexpr uint32_t AUDIO_BITRATE_MAX = 512000;
inline constexpr uint32_t AUDIO_CHANNELS_MIN = 1;
inline constexpr uint32_t AUDIO_CHANNELS_MAX = 8;
inline constexpr uint32_t UPLOAD_THREADS_MIN = 1;
inline constexpr uint32_t UPLOAD_THREADS_MAX = 16;
inline constexpr uint32_t LOG_FILE_SIZE_MB_MIN = 1;
inline constexpr uint32_t LOG_FILE_SIZE_MB_MAX = 1024;
inline constexpr uint32_t LOG_MAX_FILES_MIN = 1;
inline constexpr uint32_t LOG_MAX_FILES_MAX = 100;
}
