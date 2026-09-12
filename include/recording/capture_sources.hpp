#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "obs/obs.h"
#include "recording/monitor_layout.hpp"
#include "types.hpp"

namespace screenrec {
class LogService;
}

namespace screenrec::capture {

using Logger = std::shared_ptr<LogService>;
using Reporter = std::function<void(const std::string&)>;

// One source per monitor at virtual-desktop offsets, plus a fallback; false only where that's fatal.
bool createScreenSources(obs_scene_t* scene, const std::vector<MonitorInfo>& monitors,
    std::vector<obs_sceneitem_t*>& sceneItems,
    std::vector<obs_source_t*>& screenCaptures,
    obs_source_t** displaySource,
    const Reporter& reportError, const Logger& logger);

// Every failure here is non-fatal — recording continues without the source, hence a WARNING reporter.
void createAudioSources(obs_source_t** systemAudio, obs_source_t** mic,
    const Reporter& reportWarning, const Logger& logger);

// obs-filters noise suppression ("rnnoise" or "speex") on one audio source.
void applyNoiseSuppression(obs_source_t* source, const std::string& method,
    const char* label, const Logger& logger);

// Creates the source, waits for backend dimensions, adds it at its offset. Takes ownership of `settings`.
bool addScreenSourceToScene(obs_scene_t* scene, const MonitorInfo& monitor,
    const MonitorBounds& bounds, const char* sourceId,
    const char* sourceName, obs_data_t* settings,
    std::vector<obs_sceneitem_t*>& sceneItems,
    std::vector<obs_source_t*>& screenCaptures,
    const Logger& logger);

bool addFallbackDisplaySource(obs_scene_t* scene, const char* sourceId,
    obs_source_t** displaySource,
    std::vector<obs_sceneitem_t*>& sceneItems,
    const Logger& logger);

}
