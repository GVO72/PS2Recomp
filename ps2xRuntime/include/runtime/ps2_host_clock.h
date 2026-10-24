#ifndef PS2_HOST_CLOCK_H
#define PS2_HOST_CLOCK_H

#include <atomic>
#include <ctime>

// Host wall-clock and time-zone access for the HLE layer, in one place so the
// deterministic run mode (PS2Runtime::RunMode::deterministic) can replace it.
// The flag is process-wide because the helpers that read host times are free
// functions with no runtime to ask; PS2Runtime::setRunMode sets it.
namespace ps2_host_clock
{
    // 2000-01-01 00:00:00 UTC, the start of the fixed clock and the time every
    // file or memory-card timestamp reports in deterministic mode.
    inline constexpr std::time_t kFixedEpoch = 946684800;

    inline std::atomic<bool> &deterministicFlag()
    {
        static std::atomic<bool> flag{false};
        return flag;
    }

    inline bool deterministic()
    {
        return deterministicFlag().load(std::memory_order_relaxed);
    }

    inline void setDeterministic(bool value)
    {
        deterministicFlag().store(value, std::memory_order_relaxed);
    }

    // The time to stamp on something created now: the host clock, or the fixed epoch.
    inline std::time_t now()
    {
        return deterministic() ? kFixedEpoch : std::time(nullptr);
    }

    // Broken-down time. Deterministic mode uses UTC so the host time zone never shows.
    inline bool breakDown(const std::time_t *value, std::tm *out)
    {
#ifdef _WIN32
        return (deterministic() ? gmtime_s(out, value) : localtime_s(out, value)) == 0;
#else
        return (deterministic() ? gmtime_r(value, out) : localtime_r(value, out)) != nullptr;
#endif
    }
}

#endif // PS2_HOST_CLOCK_H
