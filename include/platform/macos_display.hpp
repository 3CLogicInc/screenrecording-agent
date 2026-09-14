#pragma once

#if defined(__APPLE__)

#include <vector>

#include "types.hpp"

namespace screenrec::macos {

// name = display UUID (OBS screen_capture "display_uuid"); width/height = native capture pixels;
// x/y/renderScale are normalized by normalizeMixedDpiLayout.
std::vector<MonitorInfo> detectMonitors();

}

#endif
