#pragma once

#include <string>

// Windows-only (no-op elsewhere): the screen_recording_<tag>_crash_*.log name is the prefix LokiShipper tails.
namespace screenrec::crash {

void install(const std::string& processTag, const std::string& logsDir);

}
