#include "exrc/DecodeScheduler.h"

#include <algorithm>

namespace exrc {

DecodeScheduler::DecodeScheduler(FrameCache& cache, int threads) : cache_(cache) {
    threads = std::max(1, threads);
    for (int i = 0; i < threads; ++i) threads_.emplace_back([this] { workerLoop(); });
}

DecodeScheduler::~DecodeScheduler() {
    {
        std::lock_guard lock(m_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_) t.join();
}

void DecodeScheduler::setRequests(std::vector<DecodeRequest> requests) {
    {
        std::lock_guard lock(m_);
        requests_ = std::move(requests);
        activeKeys_.clear();
        for (const auto& r : requests_)
            if (r.source) activeKeys_.insert(r.source->key);
        // Forget urgent results for keys nobody wants any more.
        for (auto it = lastDecoded_.begin(); it != lastDecoded_.end();)
            it = activeKeys_.count(it->first) ? std::next(it) : lastDecoded_.erase(it);
    }
    cv_.notify_all();
}

void DecodeScheduler::clear() {
    {
        std::lock_guard lock(m_);
        lastDecoded_.clear();
    }
    setRequests({});
}

FramePtr DecodeScheduler::lastDecoded(uint32_t key, int frame) const {
    std::lock_guard lock(m_);
    auto it = lastDecoded_.find(key);
    if (it != lastDecoded_.end() && it->second->frameNumber == frame) return it->second;
    return nullptr;
}

bool DecodeScheduler::pickLocked(size_t& indexOut) const {
    for (size_t i = 0; i < requests_.size(); ++i) {
        const DecodeRequest& r = requests_[i];
        if (!r.source) continue;
        const uint32_t key = r.source->key;
        if (inflight_.count(id(key, r.frame)) || cache_.contains(key, r.frame)) continue;
        if (r.urgent) {
            auto it = lastDecoded_.find(key);
            if (it != lastDecoded_.end() && it->second->frameNumber == r.frame) continue;
            indexOut = i;
            return true;
        }
        // Don't decode a frame the cache would refuse to keep (budget full of nearer frames).
        if (!cache_.wouldAccept(key, r.frame, r.source->estimatedBytes)) continue;
        indexOut = i;
        return true;
    }
    return false;
}

bool DecodeScheduler::idle() const {
    std::lock_guard lock(m_);
    size_t i;
    return inflight_.empty() && !pickLocked(i);
}

void DecodeScheduler::waitIdle() {
    std::unique_lock lock(m_);
    idleCv_.wait(lock, [this] {
        size_t i;
        return inflight_.empty() && !pickLocked(i);
    });
}

void DecodeScheduler::workerLoop() {
    std::unique_lock lock(m_);
    while (true) {
        size_t index = 0;
        cv_.wait(lock, [&] { return stop_ || pickLocked(index); });
        if (stop_) return;

        const DecodeRequest req = requests_[index];
        const uint64_t job = id(req.source->key, req.frame);
        inflight_.insert(job);
        lock.unlock();

        FramePtr result = req.source->decode(req.frame);

        lock.lock();
        inflight_.erase(job);
        // Discard results for a layer, AOV or sequence the user has since moved away from.
        const bool stillWanted = std::any_of(requests_.begin(), requests_.end(), [&](const DecodeRequest& r) {
            return r.source == req.source;
        });
        if (result && stillWanted) {
            cache_.insert(result);
            if (req.urgent) lastDecoded_[req.source->key] = result;
        }
        idleCv_.notify_all();
        cv_.notify_all();
    }
}

}  // namespace exrc
