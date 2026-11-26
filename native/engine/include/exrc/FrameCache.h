// RAM frame cache shared by both layer slots, with one byte budget and playhead-aware eviction.
//
// Every cached frame is scored by how far *ahead* of the playhead it is in the playback
// direction (wrapping around the in/out range when looping). Frames just behind the playhead
// have the largest score and go first, so frames pre-fetched for upcoming playback are never
// thrown away to make room for frames that are further away. Frames whose key is not active
// (an AOV or layer nobody is looking at) go before anything else.
#pragma once

#include "exrc/Types.h"

#include <mutex>
#include <unordered_map>

namespace exrc {

struct PlayheadHint {
    struct Active {
        uint32_t key = 0;  // cacheKey(slot, generation, aov)
        int frame = 0;     // playhead mapped into this layer's frame numbers
        int rangeStart = 0;
        int rangeEnd = 0;
    };
    std::vector<Active> active;
    int direction = 1;  // +1 forward, -1 reverse
    bool loop = true;
};

class FrameCache {
public:
    explicit FrameCache(size_t budgetBytes = size_t(4) << 30);

    void setBudget(size_t bytes);
    size_t budget() const;
    size_t bytesUsed() const;

    FramePtr get(uint32_t key, int frame) const;
    bool contains(uint32_t key, int frame) const;

    // Returns false (and does not store) if the frame would only fit by evicting frames that
    // are needed sooner than it is.
    bool insert(const FramePtr& frame);
    bool wouldAccept(uint32_t key, int frame, size_t bytes) const;

    void setPlayhead(const PlayheadHint& hint);
    void clear();
    void eraseKey(uint32_t key);
    std::vector<int> cachedFrames(uint32_t key) const;  // sorted, for the timeline cache bar

private:
    static uint64_t id(uint32_t key, int frame) { return (uint64_t(key) << 32) | uint32_t(frame); }
    int64_t scoreLocked(uint32_t key, int frame) const;  // larger = evict sooner
    void evictLocked(size_t needed, int64_t protectBelowOrEqual);

    mutable std::mutex m_;
    std::unordered_map<uint64_t, FramePtr> frames_;
    size_t budget_;
    size_t used_ = 0;
    PlayheadHint hint_;
};

}  // namespace exrc
