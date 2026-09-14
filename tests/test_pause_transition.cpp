#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "recording/pause_transition.hpp"

using namespace screenrec;

namespace {
constexpr bool PAUSE = true;
constexpr bool RESUME = false;

bool isPaused(PauseState s, PauseStream stream) {
    if (stream == PauseStream::AUDIO) {
        return s == PauseState::AUDIO_ONLY || s == PauseState::BOTH;
    }
    return s == PauseState::VIDEO_ONLY || s == PauseState::BOTH;
}

// Apply a transition, returning the resulting state (no-ops leave state as-is).
PauseState apply(PauseState current, PauseStream stream, bool pausing) {
    const PauseTransition t = computePauseTransition(current, stream, pausing);
    return t.alreadyInDesiredState ? current : t.next;
}

// Convenience: assert a transition is a no-op (state unchanged, engine untouched).
void expectNoop(PauseState current, PauseStream stream, bool pausing) {
    const PauseTransition t = computePauseTransition(current, stream, pausing);
    CHECK(t.alreadyInDesiredState);
    CHECK(t.next == current); // next is defined as `current` for no-ops
}

// Convenience: assert a transition acts and lands on `expected`.
void expectMove(PauseState current, PauseStream stream, bool pausing, PauseState expected) {
    const PauseTransition t = computePauseTransition(current, stream, pausing);
    CHECK_FALSE(t.alreadyInDesiredState);
    CHECK(t.next == expected);
}
}

TEST_CASE("pause audio moves NONE -> AUDIO_ONLY") {
    expectMove(PauseState::NONE, PauseStream::AUDIO, PAUSE, PauseState::AUDIO_ONLY);
}
TEST_CASE("pause audio moves VIDEO_ONLY -> BOTH") {
    expectMove(PauseState::VIDEO_ONLY, PauseStream::AUDIO, PAUSE, PauseState::BOTH);
}
TEST_CASE("pause audio is a no-op when already AUDIO_ONLY") {
    expectNoop(PauseState::AUDIO_ONLY, PauseStream::AUDIO, PAUSE);
}
TEST_CASE("pause audio is a no-op when already BOTH") {
    expectNoop(PauseState::BOTH, PauseStream::AUDIO, PAUSE);
}

TEST_CASE("pause video moves NONE -> VIDEO_ONLY") {
    expectMove(PauseState::NONE, PauseStream::VIDEO, PAUSE, PauseState::VIDEO_ONLY);
}
TEST_CASE("pause video moves AUDIO_ONLY -> BOTH") {
    expectMove(PauseState::AUDIO_ONLY, PauseStream::VIDEO, PAUSE, PauseState::BOTH);
}
TEST_CASE("pause video is a no-op when already VIDEO_ONLY") {
    expectNoop(PauseState::VIDEO_ONLY, PauseStream::VIDEO, PAUSE);
}
TEST_CASE("pause video is a no-op when already BOTH") {
    expectNoop(PauseState::BOTH, PauseStream::VIDEO, PAUSE);
}

TEST_CASE("resume audio moves AUDIO_ONLY -> NONE") {
    expectMove(PauseState::AUDIO_ONLY, PauseStream::AUDIO, RESUME, PauseState::NONE);
}
TEST_CASE("resume audio moves BOTH -> VIDEO_ONLY") {
    expectMove(PauseState::BOTH, PauseStream::AUDIO, RESUME, PauseState::VIDEO_ONLY);
}
TEST_CASE("resume audio is a no-op when NONE") {
    expectNoop(PauseState::NONE, PauseStream::AUDIO, RESUME);
}
TEST_CASE("resume audio is a no-op when only VIDEO_ONLY is paused") {
    expectNoop(PauseState::VIDEO_ONLY, PauseStream::AUDIO, RESUME);
}

TEST_CASE("resume video moves VIDEO_ONLY -> NONE") {
    expectMove(PauseState::VIDEO_ONLY, PauseStream::VIDEO, RESUME, PauseState::NONE);
}
TEST_CASE("resume video moves BOTH -> AUDIO_ONLY") {
    expectMove(PauseState::BOTH, PauseStream::VIDEO, RESUME, PauseState::AUDIO_ONLY);
}
TEST_CASE("resume video is a no-op when NONE") {
    expectNoop(PauseState::NONE, PauseStream::VIDEO, RESUME);
}
TEST_CASE("resume video is a no-op when only AUDIO_ONLY is paused") {
    expectNoop(PauseState::AUDIO_ONLY, PauseStream::VIDEO, RESUME);
}

TEST_CASE("repeating an operation is idempotent (second call is a no-op)") {
    // For every start state and stream: pausing then pausing-again is a no-op,
    // and the same for resume. This is exactly the bug that produced HTTP 500s.
    const PauseState states[] = {PauseState::NONE, PauseState::AUDIO_ONLY,
        PauseState::VIDEO_ONLY, PauseState::BOTH};
    const PauseStream streams[] = {PauseStream::AUDIO, PauseStream::VIDEO};
    for (PauseState s : states) {
        for (PauseStream stream : streams) {
            for (bool pausing : {PAUSE, RESUME}) {
                const PauseState afterFirst = apply(s, stream, pausing);
                const PauseTransition second =
                    computePauseTransition(afterFirst, stream, pausing);
                CHECK(second.alreadyInDesiredState); // second identical call does nothing
            }
        }
    }
}

TEST_CASE("pause always ends with the stream paused; resume always ends running") {
    const PauseState states[] = {PauseState::NONE, PauseState::AUDIO_ONLY,
        PauseState::VIDEO_ONLY, PauseState::BOTH};
    for (PauseState s : states) {
        for (PauseStream stream : {PauseStream::AUDIO, PauseStream::VIDEO}) {
            CHECK(isPaused(apply(s, stream, PAUSE), stream));
            CHECK_FALSE(isPaused(apply(s, stream, RESUME), stream));
        }
    }
}

TEST_CASE("operating on one stream never disturbs the other stream") {
    const PauseState states[] = {PauseState::NONE, PauseState::AUDIO_ONLY,
        PauseState::VIDEO_ONLY, PauseState::BOTH};
    for (PauseState s : states) {
        // Toggling audio leaves video's paused-ness untouched, and vice versa.
        CHECK(isPaused(apply(s, PauseStream::AUDIO, PAUSE), PauseStream::VIDEO) == isPaused(s, PauseStream::VIDEO));
        CHECK(isPaused(apply(s, PauseStream::AUDIO, RESUME), PauseStream::VIDEO) == isPaused(s, PauseStream::VIDEO));
        CHECK(isPaused(apply(s, PauseStream::VIDEO, PAUSE), PauseStream::AUDIO) == isPaused(s, PauseStream::AUDIO));
        CHECK(isPaused(apply(s, PauseStream::VIDEO, RESUME), PauseStream::AUDIO) == isPaused(s, PauseStream::AUDIO));
    }
}
