#include "exrc/PlaybackController.h"

#include <algorithm>

namespace exrc {

void PlaybackController::setRange(int first, int last) {
    if (last < first) std::swap(first, last);
    first_ = in_ = first;
    last_ = out_ = last;
    current_ = std::clamp(current_, first_, last_);
    anchorFrame_ = current_;
}

void PlaybackController::setInOut(int in, int out) {
    if (out < in) std::swap(in, out);
    in_ = std::clamp(in, first_, last_);
    out_ = std::clamp(out, first_, last_);
}

void PlaybackController::clearInOut() {
    in_ = first_;
    out_ = last_;
}

void PlaybackController::setFrameRate(Rational fps, TimePoint now) {
    if (fps.num <= 0 || fps.den <= 0) return;
    fps_ = fps;
    anchor(now);
}

void PlaybackController::setMode(PlaybackMode mode, TimePoint now) {
    mode_ = mode;
    anchor(now);
}

void PlaybackController::anchor(TimePoint now) {
    anchorTime_ = now;
    anchorFrame_ = current_;
    advanced_ = 0;
}

int64_t PlaybackController::framesElapsed(TimePoint now) const {
    const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - anchorTime_).count();
    if (ns <= 0) return 0;
    // Exact integer maths: frames = ns * num / (den * 1e9). No drift at 23.976 / 29.97.
    return ns * fps_.num / (fps_.den * 1'000'000'000LL);
}

TimePoint PlaybackController::dueTime(int64_t n) const {
    // Round up so a frame is never shown before its exact due time.
    const int64_t num = n * fps_.den * 1'000'000'000LL;
    return anchorTime_ + std::chrono::nanoseconds((num + fps_.num - 1) / fps_.num);
}

bool PlaybackController::wrapOrStop(int64_t offset, int& frameOut) {
    const int64_t len = int64_t(out_) - in_ + 1;
    if (loop_) {
        frameOut = in_ + int(((offset % len) + len) % len);
        return true;
    }
    if (offset < 0) {
        frameOut = in_;
        return false;
    }
    if (offset >= len) {
        frameOut = out_;
        return false;
    }
    frameOut = in_ + int(offset);
    return true;
}

void PlaybackController::play(int direction, TimePoint now) {
    dir_ = direction >= 0 ? 1 : -1;
    if (current_ < in_ || current_ > out_) current_ = dir_ > 0 ? in_ : out_;
    // Pressing play at the end with looping off starts again from the beginning.
    if (!loop_ && ((dir_ > 0 && current_ >= out_) || (dir_ < 0 && current_ <= in_)))
        current_ = dir_ > 0 ? in_ : out_;
    playing_ = true;
    anchor(now);
}

void PlaybackController::step(int delta) {
    playing_ = false;
    int f = current_;
    wrapOrStop(int64_t(current_) - in_ + delta, f);
    current_ = f;
}

void PlaybackController::seek(int frame, TimePoint now) {
    current_ = std::clamp(frame, first_, last_);
    anchor(now);
}

bool PlaybackController::update(TimePoint now, const ReadyFn& isReady) {
    if (!playing_) return false;
    const int prev = current_;

    if (mode_ == PlaybackMode::Realtime) {
        const int64_t offset = int64_t(anchorFrame_) - in_ + dir_ * framesElapsed(now);
        int f = current_;
        if (!wrapOrStop(offset, f)) playing_ = false;
        current_ = f;
    } else {
        if (now >= dueTime(advanced_ + 1)) {
            int f = current_;
            if (!wrapOrStop(int64_t(current_) - in_ + dir_, f)) {
                playing_ = false;  // reached the end with looping off
            } else if (!isReady || isReady(f)) {
                current_ = f;
                ++advanced_;
                // After a stall (waiting on decode), restart the clock instead of
                // racing through frames to "catch up".
                if (now >= dueTime(advanced_ + 1)) anchor(now);
            }
        }
    }
    return current_ != prev;
}

}  // namespace exrc
