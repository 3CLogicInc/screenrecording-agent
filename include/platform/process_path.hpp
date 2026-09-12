#pragma once

#include <string>

namespace screenrec::platform {

// Absolute, symlinks resolved where the OS allows; empty string only on hard failure.
std::string executablePath();

// No trailing slash; "." when the path has no separator.
std::string executableDir();

}
