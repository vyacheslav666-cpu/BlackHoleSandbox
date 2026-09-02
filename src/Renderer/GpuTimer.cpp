#include "Renderer/GpuTimer.hpp"

#include <algorithm>
#include <cmath>

namespace bhs::renderer {

GpuTimer::~GpuTimer() {
    // Deliberately does not call release(): a timer that outlives its context
    // would be deleting names that no longer belong to anything.  Application
    // releases it explicitly while the context is still current.
}

void GpuTimer::begin() {
    if (active_) {
        return;
    }
    // Collect whatever the GPU has finished since the last frame.  This never
    // waits, so it costs nothing when results are not ready yet.
    harvestAll(false);
    // Only if the GPU has fallen a whole ring behind is there no free slot, and
    // then waiting for the oldest result is the only way to get one.
    while (pendingCount_ == kRingSize) {
        harvestOne(true);
    }

    Slot& slot = ring_[writeIndex_];
    if (slot.query == 0) {
        glGenQueries(1, &slot.query);
        if (slot.query == 0) {
            return;
        }
    }
    glBeginQuery(GL_TIME_ELAPSED, slot.query);
    active_ = true;
}

void GpuTimer::end() {
    if (!active_) {
        return;
    }
    glEndQuery(GL_TIME_ELAPSED);
    active_ = false;
    writeIndex_ = (writeIndex_ + 1) % kRingSize;
    ++pendingCount_;
}

bool GpuTimer::harvestOne(bool blocking) {
    if (pendingCount_ == 0) {
        return false;
    }
    const GLuint query = ring_[readIndex_].query;
    if (query == 0) {
        // A slot whose query object could not be created carries no timing.
        readIndex_ = (readIndex_ + 1) % kRingSize;
        --pendingCount_;
        return true;
    }
    if (!blocking) {
        GLuint available = GL_FALSE;
        glGetQueryObjectuiv(query, GL_QUERY_RESULT_AVAILABLE, &available);
        if (available == GL_FALSE) {
            // Queries complete in submission order, so an unfinished oldest one
            // means none of the newer ones are ready either.
            return false;
        }
    }
    GLuint64 elapsed = 0;
    glGetQueryObjectui64v(query, GL_QUERY_RESULT, &elapsed);
    record(static_cast<std::uint64_t>(elapsed));
    readIndex_ = (readIndex_ + 1) % kRingSize;
    --pendingCount_;
    return true;
}

void GpuTimer::harvestAll(bool blocking) {
    while (harvestOne(blocking)) {
    }
}

void GpuTimer::record(std::uint64_t nanoseconds) {
    lastSample_ = nanoseconds;
    if (samples_.size() < kWindowSize) {
        samples_.push_back(nanoseconds);
        return;
    }
    samples_[sampleWriteIndex_] = nanoseconds;
    sampleWriteIndex_ = (sampleWriteIndex_ + 1) % kWindowSize;
}

void GpuTimer::poll() { harvestAll(false); }

void GpuTimer::flush() {
    if (active_) {
        end();
    }
    harvestAll(true);
}

void GpuTimer::resetStatistics() {
    harvestAll(true);
    samples_.clear();
    sampleWriteIndex_ = 0;
    lastSample_ = 0;
}

void GpuTimer::release() {
    if (active_) {
        glEndQuery(GL_TIME_ELAPSED);
        active_ = false;
    }
    for (Slot& slot : ring_) {
        if (slot.query != 0) {
            glDeleteQueries(1, &slot.query);
            slot.query = 0;
        }
    }
    writeIndex_ = 0;
    readIndex_ = 0;
    pendingCount_ = 0;
}

double GpuTimer::percentileMilliseconds(double fraction) const {
    if (samples_.empty()) {
        return 0.0;
    }
    std::vector<std::uint64_t> sorted(samples_);
    std::sort(sorted.begin(), sorted.end());

    const double clamped = std::clamp(fraction, 0.0, 1.0);
    const double rank = std::ceil(clamped * static_cast<double>(sorted.size()));
    std::size_t index = rank < 1.0 ? 0u : static_cast<std::size_t>(rank) - 1u;
    index = std::min(index, sorted.size() - 1u);
    return toMilliseconds(sorted[index]);
}

double GpuTimer::minMilliseconds() const {
    if (samples_.empty()) {
        return 0.0;
    }
    return toMilliseconds(*std::min_element(samples_.begin(), samples_.end()));
}

double GpuTimer::maxMilliseconds() const {
    if (samples_.empty()) {
        return 0.0;
    }
    return toMilliseconds(*std::max_element(samples_.begin(), samples_.end()));
}

double GpuTimer::totalMilliseconds() const {
    std::uint64_t total = 0;
    for (const std::uint64_t sample : samples_) {
        total += sample;
    }
    return toMilliseconds(total);
}

} // namespace bhs::renderer
