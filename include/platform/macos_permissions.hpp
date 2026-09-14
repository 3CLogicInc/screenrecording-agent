#pragma once

#if defined(__APPLE__)

namespace screenrec::macos {

// Preflight is side-effect free; request fires the one-time system prompt. Screen-capture status is
// FROZEN at process launch — a grant made while the process runs is only observable to a fresh launch.
bool screenCaptureAccessPreflight();
bool screenCaptureAccessRequest();

// Marker file survives the denied-relaunch cycle so the prompt fires once; the installer removes it.
bool tccPromptAlreadyShown();
void markTccPromptShown();

enum class MicAccess : unsigned char { GRANTED,
    DENIED,
    UNDETERMINED };
MicAccess micAccessStatus();

}

#endif
