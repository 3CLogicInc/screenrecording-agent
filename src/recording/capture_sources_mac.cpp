#include "recording/capture_sources.hpp"

#include "logging/log_service.hpp"
#include "platform/macos_permissions.hpp"

namespace screenrec::capture {

// "screen_capture" is the ScreenCaptureKit source (macOS 13+); "display_capture" is a deprecated fallback.
bool createScreenSources(obs_scene_t* scene, const std::vector<MonitorInfo>& monitors,
    std::vector<obs_sceneitem_t*>& sceneItems,
    std::vector<obs_source_t*>& screenCaptures,
    obs_source_t** displaySource,
    const Reporter& reportError, const Logger& logger) {
    logger->info("RecordingEngine",
        "Creating screen_capture sources for " + std::to_string(monitors.size()) + " monitor(s)");

    MonitorBounds bounds;
    if (getCompositeBounds(monitors, bounds)) {
        for (const auto& monitor : monitors) {
            obs_data_t* settings = obs_data_create();
            obs_data_set_int(settings, "type", 0); // display capture
            obs_data_set_string(settings, "display_uuid", monitor.name.c_str());
            obs_data_set_bool(settings, "show_cursor", true);

            const std::string sourceName = "Display_" + std::to_string(monitor.index);
            logger->info("RecordingEngine",
                "Creating screen_capture for display " + monitor.name + " (" +
                    std::to_string(monitor.width) + "x" + std::to_string(monitor.height) + ")");

            addScreenSourceToScene(scene, monitor, bounds, "screen_capture",
                sourceName.c_str(), settings, sceneItems,
                screenCaptures, logger);
        }
    }

    if (screenCaptures.empty()) {
        logger->warning("RecordingEngine",
            "No screen_capture sources created, trying deprecated display_capture fallback");
        if (!addFallbackDisplaySource(scene, "display_capture", displaySource, sceneItems, logger)) {
            reportError("Failed to create any screen capture source");
            return false;
        }
    }
    return true;
}

void createAudioSources(obs_source_t** systemAudio, obs_source_t** mic,
    const Reporter& reportWarning, const Logger& logger) {
    // SCK desktop audio is covered by the Screen Recording TCC grant; no separate permission.
    obs_data_t* desktopSettings = obs_data_create();
    obs_data_set_int(desktopSettings, "type", 0); // desktop audio
    *systemAudio = obs_source_create("sck_audio_capture", "DesktopAudio",
        desktopSettings, nullptr);
    obs_data_release(desktopSettings);

    if (*systemAudio == nullptr) {
        reportWarning("Failed to create desktop audio source");
    } else {
        logger->info("RecordingEngine", "Desktop audio source created (sck_audio_capture)");
    }

    // Microphone needs its own TCC grant; denial is non-fatal, matching Windows.
    if (macos::micAccessStatus() == macos::MicAccess::DENIED) {
        logger->warning("RecordingEngine", "Microphone permission denied; recording without mic");
        return;
    }
    *mic = obs_source_create("coreaudio_input_capture", "Microphone", nullptr, nullptr);
    if (*mic == nullptr) {
        reportWarning("Failed to create microphone source");
    } else {
        logger->info("RecordingEngine", "Microphone source created (coreaudio_input_capture)");
    }
}

}
