#include "recording/capture_sources.hpp"

#include <chrono>
#include <thread>

#include "logging/log_service.hpp"

namespace screenrec::capture {

bool addScreenSourceToScene(obs_scene_t* scene, const MonitorInfo& monitor,
    const MonitorBounds& bounds, const char* sourceId,
    const char* sourceName, obs_data_t* settings,
    std::vector<obs_sceneitem_t*>& sceneItems,
    std::vector<obs_source_t*>& screenCaptures,
    const Logger& logger) {
    obs_source_t* source = obs_source_create(sourceId, sourceName, settings, nullptr);
    obs_data_release(settings);
    if (source == nullptr) {
        logger->error("RecordingEngine", std::string("Failed to create ") + sourceId + " source");
        return false;
    }

    // Force active and poll until the capture backend reports a size.
    obs_source_inc_active(source);
    int retries = 10;
    uint32_t srcWidth = 0;
    uint32_t srcHeight = 0;
    while (retries-- > 0) {
        srcWidth = obs_source_get_width(source);
        srcHeight = obs_source_get_height(source);
        if (srcWidth > 0 && srcHeight > 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    obs_source_dec_active(source);
    logger->info("RecordingEngine", "Source dimensions: " +
                                        std::to_string(srcWidth) + "x" + std::to_string(srcHeight));

    obs_sceneitem_t* item = obs_scene_add(scene, source);
    if (item == nullptr) {
        logger->error("RecordingEngine", "Failed to add source to scene");
        obs_source_release(source); // don't leak the orphaned source
        return false;
    }

    struct vec2 pos = {};
    vec2_set(&pos, static_cast<float>(monitor.x - bounds.minX),
        static_cast<float>(monitor.y - bounds.minY));
    obs_sceneitem_set_pos(item, &pos);

    struct vec2 tileSize = {};
    vec2_set(&tileSize, static_cast<float>(layoutWidth(monitor)),
        static_cast<float>(layoutHeight(monitor)));
    obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
    obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
    obs_sceneitem_set_bounds(item, &tileSize);
    obs_sceneitem_set_scale_filter(item, OBS_SCALE_AREA);

    sceneItems.push_back(item);
    screenCaptures.push_back(source);
    return true;
}

bool addFallbackDisplaySource(obs_scene_t* scene, const char* sourceId,
    obs_source_t** displaySource,
    std::vector<obs_sceneitem_t*>& sceneItems,
    const Logger& logger) {
    *displaySource = obs_source_create(sourceId, "Display", nullptr, nullptr);
    if (*displaySource == nullptr) {
        logger->error("RecordingEngine", std::string("Failed to create fallback ") + sourceId + " source");
        return false;
    }
    if (obs_sceneitem_t* item = obs_scene_add(scene, *displaySource); item != nullptr) {
        sceneItems.push_back(item);
    }
    return true;
}

void applyNoiseSuppression(obs_source_t* source, const std::string& method,
    const char* label, const Logger& logger) {
    if (source == nullptr) {
        return;
    }
    obs_data_t* settings = obs_data_create();
    obs_data_set_string(settings, "method", method.c_str());
    obs_source_t* filter =
        obs_source_create("noise_suppress_filter", "NoiseSuppression", settings, nullptr);
    obs_data_release(settings);
    if (filter == nullptr) {
        if (logger) {
            logger->warning("RecordingEngine",
                std::string("Noise suppression unavailable for ") + label + " (obs-filters not loaded)");
        }
        return;
    }
    obs_source_filter_add(source, filter);
    obs_source_release(filter); // the source now retains the filter
    if (logger) {
        logger->info("RecordingEngine",
            std::string("Applied ") + method + " noise suppression to " + label);
    }
}

}
