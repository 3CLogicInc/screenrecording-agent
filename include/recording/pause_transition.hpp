#pragma once

#include "../types.hpp"

namespace screenrec {

enum class PauseStream { AUDIO,
    VIDEO };

// Single source of truth so the four RecordingSession pause/resume variants stay in sync, and testable without libobs.
struct PauseTransition {
    // Idempotent no-op: report success without touching the engine.
    bool alreadyInDesiredState;
    // Only meaningful when alreadyInDesiredState is false.
    PauseState next;
};

// pausing == true pauses the stream; false resumes it.
inline PauseTransition computePauseTransition(PauseState current,
    PauseStream stream,
    bool pausing) {
    const bool audioPaused = current == PauseState::AUDIO_ONLY || current == PauseState::BOTH;
    const bool videoPaused = current == PauseState::VIDEO_ONLY || current == PauseState::BOTH;

    if (stream == PauseStream::AUDIO) {
        if (pausing) {
            if (audioPaused) {
                return {true, current};
            }
            return {false, videoPaused ? PauseState::BOTH : PauseState::AUDIO_ONLY};
        }
        if (!audioPaused) {
            return {true, current};
        }
        return {false, videoPaused ? PauseState::VIDEO_ONLY : PauseState::NONE};
    }

    if (pausing) {
        if (videoPaused) {
            return {true, current};
        }
        return {false, audioPaused ? PauseState::BOTH : PauseState::VIDEO_ONLY};
    }
    if (!videoPaused) {
        return {true, current};
    }
    return {false, audioPaused ? PauseState::AUDIO_ONLY : PauseState::NONE};
}

}
