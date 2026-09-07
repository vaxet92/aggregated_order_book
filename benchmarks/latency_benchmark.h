#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace market_data {

// --- benchmark harness -------------------------------------------------------

struct LatencyStats {
    std::size_t samples = 0;
    double min_ns = 0.0;
    double median_ns = 0.0;
    double p99_ns = 0.0;
    double mean_ns = 0.0;  // from the total elapsed time, not the per-call clock
};

// Times a callable `iterations` times, one clock reading per call so we get a
// distribution and not just an average. A separate whole-loop timing feeds
// mean_ns, which is immune to per-call clock overhead.
//
// The callable must return something cheap to fold into a checksum; the
// checksum is printed so the optimiser cannot delete the work.
class LatencyBenchmark {
   public:
    LatencyBenchmark(std::size_t iterations, std::size_t warmup) : iterations_(iterations), warmup_(warmup) {
        per_call_ns_.reserve(iterations_);
    }

    template <typename Fn>
    LatencyStats Measure(const char* name, Fn&& fn) {
        using clock = std::chrono::steady_clock;

        checksum_ = 0;
        for (std::size_t i = 0; i < warmup_; ++i) checksum_ += fn();

        per_call_ns_.clear();
        const auto loop_start = clock::now();
        for (std::size_t i = 0; i < iterations_; ++i) {
            const auto t0 = clock::now();
            checksum_ += fn();
            const auto t1 = clock::now();
            per_call_ns_.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
        }
        const auto loop_end = clock::now();

        std::sort(per_call_ns_.begin(), per_call_ns_.end());
        LatencyStats stats;
        stats.samples = per_call_ns_.size();
        stats.min_ns = per_call_ns_.front();
        stats.median_ns = per_call_ns_[per_call_ns_.size() / 2];
        stats.p99_ns = per_call_ns_[static_cast<std::size_t>(per_call_ns_.size() * 0.99)];
        stats.mean_ns = std::chrono::duration<double, std::nano>(loop_end - loop_start).count() / iterations_;

        std::printf("  %-16s  min %8.1f  median %8.1f  p99 %9.1f  mean %8.1f  ns/call  (checksum %llu)\n", name,
                    stats.min_ns, stats.median_ns, stats.p99_ns, stats.mean_ns,
                    static_cast<unsigned long long>(checksum_));
        return stats;
    }

   private:
    std::size_t iterations_;
    std::size_t warmup_;
    std::vector<double> per_call_ns_;
    std::uint64_t checksum_ = 0;
};

}  // namespace market_data