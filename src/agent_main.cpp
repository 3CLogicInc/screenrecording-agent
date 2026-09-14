#include "agent/recording_agent.hpp"
#include "platform/crash_handler.hpp"

#include <atomic>
#include <csignal>
#include <iostream>
#include <string>
#ifndef _WIN32
#include <sys/stat.h>
#endif
#ifdef __APPLE__
#include "platform/macos_status_indicator.hpp"
#include <thread>
#endif

// Set on SIGTERM/SIGINT; run() then exits so ~RecordingAgent can finalize the session.
std::atomic<bool> gStopRequested{false};

void agentSignalHandler(const int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        gStopRequested.store(true);
    }
}

int main(int argc, char* argv[]) {
    screenrec::crash::install("agent", screenrec::constants::logsDir());

#ifdef __APPLE__
    // Probe mode: report the grant via exit code — only a fresh process sees a post-launch grant.
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--check-screen-permission") {
            return screenrec::macos::screenCaptureAccessPreflight() ? 0 : 1;
        }
    }
#endif

#ifdef _WIN32
    ShowWindow(GetConsoleWindow(), SW_HIDE);
#else
    // Owner-only recordings (0600/0700): other local users can't read them.
    umask(077);

    // launchd sends SIGTERM at logoff/unload; finalize instead of dying mid-write.
    (void)std::signal(SIGTERM, agentSignalHandler);
    (void)std::signal(SIGINT, agentSignalHandler);
    (void)std::signal(SIGPIPE, SIG_IGN);
#endif

    try {
        screenrec::agent::RecordingAgent agent;

        if (!agent.start()) {
            return 1;
        }

#ifdef __APPLE__
        // AppKit (menu-bar indicator) must own the main thread's event loop; the watchdog runs on a worker.
        std::thread worker([&agent] {
            agent.run(gStopRequested);
            screenrec::macos::stopMainLoop();
        });
        screenrec::macos::runMainLoop();
        worker.join();
#else
        agent.run(gStopRequested);
#endif

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Screen Recording Agent error: " << e.what() << "\n";
        return 1;
    }
}
