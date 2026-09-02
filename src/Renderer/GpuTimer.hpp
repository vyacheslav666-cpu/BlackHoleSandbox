#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <glad/gl.h>

namespace bhs::renderer {

// Measures how long one render pass actually takes on the GPU, using
// GL_TIME_ELAPSED.
//
// CPU-side timing cannot answer this question: the driver buffers commands and
// returns from the draw call long before the GPU has run it, so a stopwatch
// around the draw measures submission, not work.  Nor can the query be read
// back in the frame that issued it -- that blocks until the GPU has caught up,
// which serialises the pipeline and inflates the very number being measured.
//
// So a small ring of query objects is cycled instead.  Each frame issues into
// the next free slot and results are harvested only once the driver reports
// them ready, typically a few frames later.  Nothing here touches the rendered
// image; a timer query only observes.
//
// Samples are kept in a bounded window, so a long interactive session reports
// recent performance rather than a lifetime average.  A --shot run issues far
// fewer samples than the window holds, so it reports its whole run.
class GpuTimer {
public:
    static constexpr std::size_t kWindowSize = 1024;

    GpuTimer() = default;
    ~GpuTimer();
    GpuTimer(const GpuTimer&) = delete;
    GpuTimer& operator=(const GpuTimer&) = delete;

    // Brackets the pass being measured.  The two must alternate: OpenGL allows
    // only one GL_TIME_ELAPSED query to be active at a time, so these cannot be
    // nested.
    void begin();
    void end();

    // Collects any results the GPU has finished, without ever waiting.  begin()
    // does this too, so it is only needed on frames that skip the measured pass
    // entirely -- otherwise their in-flight timings would never be collected.
    void poll();

    // Blocks until every issued query has been read back.  Call once after the
    // last end() and before reporting final statistics, or the last few frames
    // are missing from the window.
    void flush();

    // Drops the collected window.  Outstanding queries are still harvested
    // first, so a stale in-flight result cannot land in the new window.
    void resetStatistics();

    // Destroys the query objects.  Must run while the GL context is current.
    void release();

    [[nodiscard]] std::size_t sampleCount() const { return samples_.size(); }
    [[nodiscard]] double lastMilliseconds() const { return toMilliseconds(lastSample_); }

    // Percentiles use the nearest-rank definition: the reported figure is
    // always a timing that was genuinely observed, never an interpolation
    // between two frames that each ran at some other speed.  `fraction` is in
    // 0..1, so p95 is percentileMilliseconds(0.95).
    [[nodiscard]] double percentileMilliseconds(double fraction) const;
    [[nodiscard]] double medianMilliseconds() const { return percentileMilliseconds(0.5); }
    [[nodiscard]] double minMilliseconds() const;
    [[nodiscard]] double maxMilliseconds() const;
    [[nodiscard]] double totalMilliseconds() const;

private:
    // Deep enough that the CPU running several frames ahead of the GPU never
    // has to wait for a slot, which would reintroduce the stall this class
    // exists to avoid.
    static constexpr std::size_t kRingSize = 8;

    struct Slot {
        GLuint query = 0;
    };

    // Reads back one finished query.  Returns false when nothing is pending, or
    // when `blocking` is false and the oldest result is not ready yet.
    bool harvestOne(bool blocking);
    void harvestAll(bool blocking);
    void record(std::uint64_t nanoseconds);
    [[nodiscard]] static double toMilliseconds(std::uint64_t nanoseconds) {
        return static_cast<double>(nanoseconds) / 1.0e6;
    }

    Slot ring_[kRingSize];
    std::size_t writeIndex_ = 0;    // Next slot to issue into.
    std::size_t readIndex_ = 0;     // Oldest slot still awaiting a result.
    std::size_t pendingCount_ = 0;
    bool active_ = false;           // A query is open between begin() and end().

    // Ring buffer of timings in nanoseconds.  Order does not matter: every
    // statistic here sorts a copy.
    std::vector<std::uint64_t> samples_;
    std::size_t sampleWriteIndex_ = 0;
    std::uint64_t lastSample_ = 0;
};

} // namespace bhs::renderer
