#include "exrc/FrameCache.h"

#include <algorithm>
#include <limits>

namespace exrc {

FrameCache::FrameCache(size_t budgetBytes) : budget_(budgetBytes) {}

void FrameCache::setBudget(size_t bytes) {
    std::lock_guard lock(m_);
    budget_ = bytes;
    if (used_ > budget_) evictLocked(used_ - budget_, -1);
}

size_t FrameCache::budget() const {
    std::lock_guard lock(m_);
    return budget_;
}

size_t FrameCache::bytesUsed() const {
    std::lock_guard lock(m_);
    return used_;
}

FramePtr FrameCache::get(uint32_t key, int frame) const {
    std::lock_guard lock(m_);
    auto it = frames_.find(id(key, frame));
    return it == frames_.end() ? nullptr : it->second;
}

bool FrameCache::contains(uint32_t key, int frame) const {
    std::lock_guard lock(m_);
    return frames_.count(id(key, frame)) != 0;
}

int64_t FrameCache::scoreLocked(uint32_t key, int frame) const {
    constexpr int64_t kInactive = std::numeric_limits<int64_t>::max();
    constexpr int64_t kOutOfRange = std::numeric_limits<int64_t>::max() - 1;
    const PlayheadHint::Active* a = nullptr;
    for (const auto& act : hint_.active)
        if (act.key == key) {
            a = &act;
            break;
        }
    if (!a) return kInactive;
    if (frame < a->rangeStart || frame > a->rangeEnd) return kOutOfRange;
    const int64_t len = int64_t(a->rangeEnd) - a->rangeStart + 1;
    int64_t ahead = (int64_t(frame) - a->frame) * hint_.direction;
    if (hint_.loop) {
        ahead = ((ahead % len) + len) % len;
    } else if (ahead < 0) {
        ahead = len - ahead;  // behind the playhead without looping: least useful in range
    }
    return ahead;
}

void FrameCache::evictLocked(size_t needed, int64_t protectBelowOrEqual) {
    size_t freed = 0;
    while (freed < needed && !frames_.empty()) {
        auto victim = frames_.end();
        int64_t worst = protectBelowOrEqual;
        for (auto it = frames_.begin(); it != frames_.end(); ++it) {
            const int64_t s = scoreLocked(it->second->key, it->second->frameNumber);
            if (s > worst) {
                worst = s;
                victim = it;
            }
        }
        if (victim == frames_.end()) return;  // everything left is more important
        const size_t sz = victim->second->byteSize();
        freed += sz;
        used_ -= sz;
        frames_.erase(victim);
    }
}

bool FrameCache::wouldAccept(uint32_t key, int frame, size_t bytes) const {
    std::lock_guard lock(m_);
    if (used_ + bytes <= budget_) return true;
    const int64_t mine = scoreLocked(key, frame);
    size_t reclaimable = budget_ > used_ ? budget_ - used_ : 0;
    for (const auto& [k, f] : frames_) {
        if (scoreLocked(f->key, f->frameNumber) > mine) reclaimable += f->byteSize();
        if (reclaimable >= bytes) return true;
    }
    return false;
}

bool FrameCache::insert(const FramePtr& frame) {
    if (!frame) return false;
    std::lock_guard lock(m_);
    const uint64_t k = id(frame->key, frame->frameNumber);
    if (auto it = frames_.find(k); it != frames_.end()) {
        used_ -= it->second->byteSize();
        frames_.erase(it);
    }
    const size_t sz = frame->byteSize();
    if (used_ + sz > budget_) {
        evictLocked(used_ + sz - budget_, scoreLocked(frame->key, frame->frameNumber));
        if (used_ + sz > budget_) return false;
    }
    frames_.emplace(k, frame);
    used_ += sz;
    return true;
}

void FrameCache::setPlayhead(const PlayheadHint& hint) {
    std::lock_guard lock(m_);
    hint_ = hint;
}

void FrameCache::clear() {
    std::lock_guard lock(m_);
    frames_.clear();
    used_ = 0;
}

void FrameCache::eraseKey(uint32_t key) {
    std::lock_guard lock(m_);
    for (auto it = frames_.begin(); it != frames_.end();) {
        if (it->second->key == key) {
            used_ -= it->second->byteSize();
            it = frames_.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<int> FrameCache::cachedFrames(uint32_t key) const {
    std::vector<int> out;
    {
        std::lock_guard lock(m_);
        for (const auto& [k, f] : frames_)
            if (f->key == key) out.push_back(f->frameNumber);
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace exrc
