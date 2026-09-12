#pragma once

#ifdef __APPLE__

// AppKit requires the process main thread to pump an event loop: main() calls runMainLoop(), the watchdog runs
// on a worker thread and ends the process via stopMainLoop(). show/hide are idempotent and thread-safe.
namespace screenrec::macos {

void runMainLoop();
void stopMainLoop();

void showRecordingIndicator();
void hideRecordingIndicator();

}

#endif
