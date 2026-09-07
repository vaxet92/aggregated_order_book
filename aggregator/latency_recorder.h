#pragma once

// LatencyRecorder and TimingBreakdown: live self-instrumentation of the
// publish path, printed to the log as `[latency]` and `[timing]` lines rather
// than only benchmarked offline. LatencyRecorder times source-to-publish
// latency and tracks each venue's peak book depth; TimingBreakdown splits one
// update's cost into lock-wait/apply/merge so a live bottleneck can be
// attributed instead of guessed at. Both discard a warm-up window and report
// median/p99/max every `report_every` samples, never a mean alone.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

#include "logger/logger.h"
#include "types/venue.h"
#include "types/venue_registry.h"

namespace market_data {

// Live latency measurement for the publish path, running against real venues
// rather than a synthetic fixture.
//
// What is measured: BookUpdate::recv_mono_ns (stamped by the provider the
// moment a message parsed) to the instant the merged book reaches the
// publisher. That span covers the handoff, the per-venue book apply, and the
// k-way merge - all the parts the SPSC change touches.
//
// Not thread-safe. Today every publish arrives on the calling provider's
// thread, so this must be guarded by whatever already serialises that path -
// which is exactly why it lives in the aggregator and not inside Core.
class LatencyRecorder {
   public:
    // `report_every` samples between log lines. At ~30 updates/sec, 2000 is
    // roughly one line every 60 seconds - often enough to watch, rare enough
    // not to become the thing being measured.
    // `warmup` samples are discarded before any are kept.
    //
    // the first merges after startup are not representative of anything
    // a running system does. The Book's level vectors are empty and grow to
    // ~1500 entries with several reallocations, the book buffer pool
    // allocates its first buffer, and the std::map books take a node
    // allocation per price for a fresh 1000-level Binance snapshot. All of
    // that is one-time cost that a live system paid hours ago, and mixing it
    // into the same distribution puts a multi-millisecond outlier in `max`
    // that has nothing to do with steady-state latency.
    //
    // Discarded rather than reported separately, but COUNTED and printed, so
    // it is visible that samples were dropped rather than silently missing.
    LatencyRecorder(std::string name, size_t report_every, size_t warmup = 0)
        : name_(std::move(name)), report_every_(report_every), warmup_(warmup), warmup_remaining_(warmup) {
        samples_.reserve(report_every_);
    }

    // `source_mono_ns` == 0 means the field was never stamped. Recording it
    // would report the machine's uptime as a latency, which is both wrong and
    // large enough to swamp every real sample - the same trap ClassifyVenue
    // avoids with its never-seen check.
    // `venue_levels` is carried alongside the latency because the two answer
    // one question together. The live median came in ~10x above what
    // bench_md_core predicted, and the leading explanation is that the
    // benchmark assumed 1000-level books while the real ones are far deeper -
    // which would make the merge cache-bound rather than compute-bound.
    // Reporting them on the same line means one log answers both.
    //
    // Peak, not average: what matters is how big the tree GOT, since that is
    // what decides whether it still fits in cache.
    void Record(int64_t source_mono_ns, const std::array<uint32_t, kMaxVenues>& venue_levels) {
        for (size_t i = 0; i < kMaxVenues; ++i) {
            peak_levels_[i] = std::max(peak_levels_[i], venue_levels[i]);
        }
        Record(source_mono_ns);
    }

    void Record(int64_t source_mono_ns) {
        if (source_mono_ns == 0) {
            ++unstamped_;
            return;
        }

        const int64_t now = NowMonotonicNs();
        const int64_t elapsed = now - source_mono_ns;

        // A negative span means the two readings crossed - possible when the
        // stamp and this reading come from different threads. Not an error,
        // but it is not a latency either, so it is counted rather than
        // averaged into the distribution.
        if (elapsed < 0) {
            ++negative_;
            return;
        }

        if (warmup_remaining_ > 0) {
            --warmup_remaining_;
            ++warmed_up_;
            return;
        }

        samples_.push_back(elapsed);
        if (samples_.size() >= report_every_) {
            Report();
        }
    }

    // Percentiles, not just a mean. A mean hides the tail, and the tail is
    // what a latency change actually shows up in - a mutex that is uncontended
    // 99% of the time and blocks on the hundredth call has a fine mean.
    void Report() {
        if (samples_.empty()) {
            return;
        }
        std::sort(samples_.begin(), samples_.end());

        const size_t n = samples_.size();
        const int64_t min = samples_.front();
        const int64_t median = samples_[n / 2];
        const int64_t p99 = samples_[static_cast<size_t>(n * 0.99)];
        const int64_t max = samples_.back();

        int64_t sum = 0;
        for (int64_t sample : samples_) {
            sum += sample;
        }

        // Reported by SLOT, not by venue name.
        //
        // this used to print peak_levels_[VenueId::BINANCE] under the
        // label "binance". Slots are assigned in registration order and no
        // longer track the enum, so that label was about to
        // start naming the wrong exchange - a diagnostic that lies is worse
        // than one that is terse. Resolving slot -> name here would mean
        // handing the recorder a registry for a log line; printing the slot
        // is honest and the operator can match it against the "venue 'X'
        // registered in slot N" line at startup.
        //
        // Only non-empty slots are listed, so the line stays short when three
        // of eight are in use.
        std::string peaks;
        for (size_t i = 0; i < kMaxVenues; ++i) {
            if (peak_levels_[i] != 0) {
                fmt::format_to(std::back_inserter(peaks), "{}slot{}={}", peaks.empty() ? "" : " ", i, peak_levels_[i]);
            }
        }

        Logger::Log(LogLevel::kInfo,
                    "[latency] {} n={} min={:.1f}us median={:.1f}us p99={:.1f}us max={:.1f}us mean={:.1f}us "
                    "peak_levels[{}] (warmup_discarded={} unstamped={} negative={})",
                    name_, n, min / 1000.0, median / 1000.0, p99 / 1000.0, max / 1000.0,
                    static_cast<double>(sum) / static_cast<double>(n) / 1000.0, peaks, warmed_up_, unstamped_,
                    negative_);

        samples_.clear();
    }

    // steady_clock, matching Provider::GetMonotonicNs - the subtraction is
    // only meaningful if both readings come from the same never-jumping clock,
    static int64_t NowMonotonicNs() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

   private:
    std::string name_;
    size_t report_every_;
    size_t warmup_ = 0;
    size_t warmup_remaining_ = 0;
    uint64_t warmed_up_ = 0;
    std::vector<int64_t> samples_;
    uint64_t unstamped_ = 0;
    uint64_t negative_ = 0;

    // Deliberately NOT reset in Report(). This is a running high-water mark
    // across the whole process, because the question is whether a book grows
    // without bound over time - which a per-window figure would hide.
    std::array<uint32_t, kMaxVenues> peak_levels_{};
};

// Breaks ApplyUpdate's cost into its parts, so the ~140-190us live figure can
// be attributed instead of guessed at. Three hypotheses for that gap have
// already been killed by measurement - big books, cache misses, and my own
// arithmetic - so this reports each component separately rather than offering
// a fourth theory.
//
// Same threading note as LatencyRecorder: fed from inside Core's
// ProcessUpdate, which now runs only on the consolidator thread, so the
// accumulation is single-threaded. It used to be serialised by apply_mutex_
// instead; that mutex is gone.
class TimingBreakdown {
   public:
    // `warmup` samples discarded before any are kept - same reason as
    // LatencyRecorder: the first merges pay one-time vector growth, pool
    // allocation and std::map node allocation that a running system does not.
    explicit TimingBreakdown(size_t report_every, size_t warmup = 0)
        : report_every_(report_every), warmup_remaining_(warmup) {
        lock_wait_.reserve(report_every_);
        book_apply_.reserve(report_every_);
        merge_.reserve(report_every_);
    }

    // `fast_path_sides`/`applied_sides`: how many of this update's sides
    // FlatOrderBook resolved in place, out of how many carried a non-empty
    // delta. Answers a question stated as a hypothesis in three documents -
    // book_apply runs ~40x slower live than the benchmarked fast path for the
    // same delta size, suggesting most real deltas take the region rebuild
    // instead of the in-place path.
    void Record(int64_t lock_wait_ns, int64_t book_apply_ns, int64_t merge_ns, uint32_t merged_depth,
                uint32_t delta_levels, uint8_t fast_path_sides, uint8_t applied_sides) {
        if (warmup_remaining_ > 0) {
            --warmup_remaining_;
            return;
        }

        lock_wait_.push_back(lock_wait_ns);
        book_apply_.push_back(book_apply_ns);
        merge_.push_back(merge_ns);
        peak_merged_depth_ = std::max(peak_merged_depth_, merged_depth);
        peak_delta_levels_ = std::max(peak_delta_levels_, delta_levels);
        total_delta_levels_ += delta_levels;
        total_fast_path_sides_ += fast_path_sides;
        total_applied_sides_ += applied_sides;

        if (lock_wait_.size() >= report_every_) {
            Report();
        }
    }

    void Report() {
        if (lock_wait_.empty()) {
            return;
        }
        const size_t n = lock_wait_.size();
        const double avg_delta = static_cast<double>(total_delta_levels_) / static_cast<double>(n);
        // Percentage of DELTA-CARRYING sides that took the in-place fast path,
        // not of all sides - an empty delta is neither, per FlatOrderBook.
        const double fast_path_pct = total_applied_sides_ == 0 ? 0.0
                                                               : 100.0 * static_cast<double>(total_fast_path_sides_) /
                                                                     static_cast<double>(total_applied_sides_);

        Logger::Log(LogLevel::kInfo,
                    "[timing] n={} lock_wait[{}] book_apply[{}] merge[{}] "
                    "merged_depth_peak={} delta_levels avg={:.1f} peak={} fast_path={:.1f}% ({}/{})",
                    n, Summarise(lock_wait_), Summarise(book_apply_), Summarise(merge_), peak_merged_depth_, avg_delta,
                    peak_delta_levels_, fast_path_pct, total_fast_path_sides_, total_applied_sides_);

        lock_wait_.clear();
        book_apply_.clear();
        merge_.clear();
        total_delta_levels_ = 0;
        total_fast_path_sides_ = 0;
        total_applied_sides_ = 0;
    }

   private:
    // median/p99/max rather than a mean: the question is where the TAIL comes
    // from, and a mean would hide exactly the outliers being hunted.
    static std::string Summarise(std::vector<int64_t>& samples) {
        std::sort(samples.begin(), samples.end());
        const size_t n = samples.size();
        return fmt::format("med={:.1f}us p99={:.1f}us max={:.1f}us", samples[n / 2] / 1000.0,
                           samples[static_cast<size_t>(n * 0.99)] / 1000.0, samples.back() / 1000.0);
    }

    size_t report_every_;
    size_t warmup_remaining_ = 0;
    std::vector<int64_t> lock_wait_;
    std::vector<int64_t> book_apply_;
    std::vector<int64_t> merge_;
    uint64_t total_fast_path_sides_ = 0;
    uint64_t total_applied_sides_ = 0;
    uint32_t peak_merged_depth_ = 0;
    uint32_t peak_delta_levels_ = 0;
    uint64_t total_delta_levels_ = 0;
};

}  // namespace market_data
