#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <string_view>

#include <fmt/chrono.h>
#include <fmt/format.h>

enum class LogLevel {
    kDebug,
    kInfo,
    kWarning,
    kError,
};

class Logger {
   public:
    template <typename... Args>
    static void Log(LogLevel level, fmt::format_string<Args...> format, Args&&... args) {
        // Early-out BEFORE formatting: the arguments are never converted to
        // strings for a suppressed level, which is the whole point.
        if (level < min_level_.load(std::memory_order_relaxed)) {
            return;
        }
        // Timestamp read AFTER the early-out, so a suppressed line still costs
        // no clock read - the same reasoning that keeps the formatting below it.
        fmt::print("[{}] [{}] {}\n", Timestamp(), ToString(level),
                   fmt::vformat(format.get(), fmt::make_format_args(args...)));
    }

   private:
    // UTC, microsecond resolution: "17:42:31.123456".
    //
    // UTC rather than local time. Every venue timestamps in UTC, and
    // correlating a log line against an exchange message is precisely what
    // these lines are for during a sequence-gap or staleness investigation.
    // A local-time stamp would put a timezone conversion between the reader
    // and that comparison, at the moment they can least afford one.
    //
    // Microseconds because that is the scale of the thing being diagnosed: a
    // publish is ~50us end to end, so a millisecond stamp would collapse
    // twenty of them onto the same instant and hide the ordering.
    //
    // Time of day only, no date. These are session logs read against a run
    // that started hours ago at most; the date is in the container's own
    // metadata and would cost 11 characters on every line.
    static std::string Timestamp() {
        const auto now = std::chrono::system_clock::now();
        const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now - seconds);
        return fmt::format("{:%H:%M:%S}.{:06}", fmt::gmtime(std::chrono::system_clock::to_time_t(now)), micros.count());
    }

    static std::string_view ToString(LogLevel level) {
        switch (level) {
            case LogLevel::kDebug:
                return "DEBUG";
            case LogLevel::kInfo:
                return "INFO";
            case LogLevel::kWarning:
                return "WARNING";
            case LogLevel::kError:
                return "ERROR";
        }

        return "UNKNOWN";
    }
    static inline std::atomic<LogLevel> min_level_{LogLevel::kInfo};
};
