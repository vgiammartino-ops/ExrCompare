// Transport logic: which frame should be on screen right now. Pure logic, no threads or I/O,
// so it can be unit-tested with a fake clock.
//
// Mirrors RV's two playback behaviours (Control menu):
//   Realtime       - a clock decides the frame; frames that aren't ready are skipped.
//   PlayAllFrames  - never skip; if the next frame isn't decoded yet, hold until it is.
#pragma once

#include "exrc/Types.h"

#include <functional>

namespace exrc {

enum class PlaybackMode { Realtime, PlayAllFrames };

class PlaybackController {
public:
    using ReadyFn = std::function<bool(int frame)>;

    void setRange(int first, int last);  // whole sequence; resets in/out
    int first() const { return first_; }
    int last() const { return last_; }

    void setInOut(int in, int out);
    void clearInOut();
    int inPoint() const { return in_; }
    int outPoint() const { return out_; }

    void setFrameRate(Rational fps, TimePoint now);
    Rational frameRate() const { return fps_; }

    void setLoop(bool loop) { loop_ = loop; }
    bool loop() const { return loop_; }

    void setMode(PlaybackMode mode, TimePoint now);
    PlaybackMode mode() const { return mode_; }

    void play(int direction, TimePoint now);
    void pause() { playing_ = false; }
    bool playing() const { return playing_; }
    int direction() const { return dir_; }

    void step(int delta);  // pauses; wraps around in/out when looping, otherwise clamps
    void seek(int frame, TimePoint now);
    void goToStart(TimePoint now) { seek(in_, now); }
    void goToEnd(TimePoint now) { seek(out_, now); }

    int current() const { return current_; }

    // Call once per display refresh. Returns true if the current frame changed.
    bool update(TimePoint now, const ReadyFn& isReady);

private:
    void anchor(TimePoint now);
    int64_t framesElapsed(TimePoint now) const;     // whole frame periods since anchor
    TimePoint dueTime(int64_t framesFromAnchor) const;
    bool wrapOrStop(int64_t offsetFromIn, int& frameOut);  // false = reached end, not looping

    int first_ = 1, last_ = 1, in_ = 1, out_ = 1;
    int current_ = 1;
    int dir_ = 1;
    bool playing_ = false;
    bool loop_ = true;
    PlaybackMode mode_ = PlaybackMode::Realtime;
    Rational fps_{24, 1};
    TimePoint anchorTime_{};
    int anchorFrame_ = 1;
    int64_t advanced_ = 0;  // frames shown since anchor (PlayAllFrames)
};

}  // namespace exrc
