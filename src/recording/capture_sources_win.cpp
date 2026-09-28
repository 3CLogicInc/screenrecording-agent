#include "recording/capture_sources.hpp"

#include "logging/log_service.hpp"
#include "recording/monitor_capture_settings.hpp"

#include <windows.h>

namespace screenrec::capture {

namespace {

bool activeDisplayPathPresent(std::string& detail) {
    UINT32 paths = 0;
    UINT32 modes = 0;
    const LONG status = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes);
    if (status != ERROR_SUCCESS) {
        detail = "GetDisplayConfigBufferSizes failed with " + std::to_string(status);
        return false;
    }
    if (paths == 0 || modes == 0) {
        detail = "Windows reports no active display path for this session";
        return false;
    }
    return true;
}

}

// OBS 31 matches "monitor_id" against the display alt_id.
bool createScreenSources(obs_scene_t* scene, const std::vector<MonitorInfo>& monitors,
    std::vector<obs_sceneitem_t*>& sceneItems,
    std::vector<obs_source_t*>& screenCaptures,
    obs_source_t** displaySource,
    const Reporter& reportError, const Logger& logger) {
    std::string detail;
    if (!activeDisplayPathPresent(detail)) {
        logger->error("RecordingEngine", detail + "; refusing to create monitor capture");
        reportError("no_display: " + detail);
        return false;
    }

    logger->debug("RecordingEngine",
        "Creating monitor capture sources for " + std::to_string(monitors.size()) + " monitor(s)");

    MonitorBounds bounds;
    if (getCompositeBounds(monitors, bounds)) {
        for (const auto& monitor : monitors) {
            const MonitorCaptureSettings cfg = monitorCaptureSettings(monitor);
            obs_data_t* settings = obs_data_create();
            obs_data_set_string(settings, "monitor_id", cfg.monitorId.c_str());
            obs_data_set_bool(settings, "capture_cursor", cfg.captureCursor);
            obs_data_set_int(settings, "method", cfg.method);

            const std::string sourceName = "Monitor_" + std::to_string(monitor.index);
            logger->info("RecordingEngine",
                "Creating monitor_capture for " + monitor.name + " (" +
                    std::to_string(monitor.width) + "x" + std::to_string(monitor.height) + ")");

            addScreenSourceToScene(scene, monitor, bounds, "monitor_capture",
                sourceName.c_str(), settings, sceneItems,
                screenCaptures, logger);
        }
    }

    if (screenCaptures.empty()) {
        logger->warning("RecordingEngine", "No monitor sources created, using fallback display capture");
        addFallbackDisplaySource(scene, "monitor_capture", displaySource, sceneItems, logger);
    }

    logger->debug("RecordingEngine", "Total screen capture sources: " +
                                         std::to_string(screenCaptures.size() + (*displaySource != nullptr ? 1 : 0)));
    return true;
}

void createAudioSources(obs_source_t** systemAudio, obs_source_t** mic,
    const Reporter& reportWarning, const Logger& logger) {
    obs_data_t* desktopSettings = obs_data_create();
    *systemAudio = obs_source_create("wasapi_output_capture", "DesktopAudio",
        desktopSettings, nullptr);
    obs_data_release(desktopSettings);

    if (*systemAudio == nullptr) {
        reportWarning("Failed to create desktop audio source");
    } else {
        logger->info("RecordingEngine", "Desktop audio source created (wasapi_output_capture)");
    }

    obs_data_t* micSettings = obs_data_create();
    *mic = obs_source_create("wasapi_input_capture", "Microphone", micSettings, nullptr);
    obs_data_release(micSettings);

    if (*mic == nullptr) {
        reportWarning("Failed to create microphone source");
    } else {
        logger->info("RecordingEngine", "Microphone source created (wasapi_input_capture)");
    }
}

}
