// Worker pool that decodes requested frames into the FrameCache, in priority order.
#pragma once

#include "exrc/FrameCache.h"

#include <condition_variable>
#include <functional>
#include <set>
#include <thread>

namespace exrc {

// Something that can produce frames for one cache key (one AOV of one layer slot, or a
// computed AOV such as the contact sheet). Immutable once created; a new source is made when
// the layer is reopened or its decode options change.
class DecodeSource {
public:
    virtual ~DecodeSource() = default;
    virtual FramePtr decode(int frame) const = 0;
    uint32_t key = 0;
    size_t estimatedBytes = 0;
};
using DecodeSourcePtr = std::shared_ptr<const DecodeSource>;

struct DecodeRequest {
    DecodeSourcePtr source;
    int frame = 0;
    bool urgent = false;  // the frame on screen right now: decoded even if the cache won't keep it
};

class DecodeScheduler {
public:
    DecodeScheduler(FrameCache& cache, int threads);
    ~DecodeScheduler();
    DecodeScheduler(const DecodeScheduler&) = delete;
    DecodeScheduler& operator=(const DecodeScheduler&) = delete;

    // Replaces the wanted list; earlier entries are more urgent.
    void setRequests(std::vector<DecodeRequest> requests);
    void clear();

    // Most recently decoded frame for a key, kept even when the cache refused it (cache off,
    // or a full region cache). This is how urgent frames reach the screen.
    FramePtr lastDecoded(uint32_t key, int frame) const;

    bool idle() const;
    void waitIdle();  // tests

private:
    void workerLoop();
    bool pickLocked(size_t& indexOut) const;
    static uint64_t id(uint32_t key, int frame) { return (uint64_t(key) << 32) | uint32_t(frame); }

    FrameCache& cache_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::condition_variable idleCv_;
    std::vector<DecodeRequest> requests_;
    std::set<uint32_t> activeKeys_;
    std::set<uint64_t> inflight_;
    std::unordered_map<uint32_t, FramePtr> lastDecoded_;
    bool stop_ = false;
    std::vector<std::thread> threads_;
};

}  // namespace exrc
