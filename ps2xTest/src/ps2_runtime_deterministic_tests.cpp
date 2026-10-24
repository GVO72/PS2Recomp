#include "MiniTest.h"
#include "ps2_runtime.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "runtime/ee_scheduler.h"
#include "Stubs/Pad.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace
{
    constexpr uint32_t kIdleLoopPc = 0x00160000u;
    constexpr uint32_t kPadDataAddr = 0x1000u;
    constexpr uint32_t kClockAddr = 0x1100u;

    struct TestEnv
    {
        std::vector<uint8_t> rdram;
        PS2Runtime runtime;

        TestEnv() : rdram(PS2_RAM_SIZE, 0u)
        {
        }
    };

    // An idle guest: wait for the next vsync, forever.
    void idleLoop(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &scheduler = runtime->eeScheduler();
        ctx->pc = kIdleLoopPc;
        scheduler.waitVSync(scheduler.currentVSyncTick());
    }

    void setRegU32(R5900Context &ctx, int reg, uint32_t value)
    {
        ctx.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(value));
    }

    PS2Runtime::RunMode deterministicMode(PS2Runtime::VideoMode videoMode, bool headless = true)
    {
        PS2Runtime::RunMode mode;
        mode.headless = headless;
        mode.deterministic = true;
        mode.videoMode = videoMode;
        return mode;
    }

    // Runs the idle guest on this thread until the observer has seen `lastFrame`.
    template <typename Observer>
    void runIdleGuest(TestEnv &env, const PS2Runtime::RunMode &mode, uint64_t lastFrame, Observer observer)
    {
        env.runtime.setRunMode(mode);
        env.runtime.registerFunction(kIdleLoopPc, idleLoop);
        env.runtime.setVSyncObserver([&](PS2Runtime &runtime, uint64_t frame)
                                     {
            observer(runtime, frame);
            if (frame == lastFrame)
            {
                runtime.requestStop();
            } });
        R5900Context mainContext{};
        mainContext.pc = kIdleLoopPc;
        env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
        env.runtime.eeScheduler().run();
    }

    // The EE cycle each requested frame's VBlankStart was processed at.
    std::vector<uint64_t> frameCycles(PS2Runtime::VideoMode videoMode, uint64_t lastFrame,
                                      std::chrono::milliseconds *wall = nullptr)
    {
        TestEnv env;
        std::vector<uint64_t> cycles;
        const auto started = std::chrono::steady_clock::now();
        runIdleGuest(env, deterministicMode(videoMode), lastFrame,
                     [&](PS2Runtime &runtime, uint64_t)
                     { cycles.push_back(runtime.eeScheduler().eeCycle()); });
        if (wall)
        {
            *wall = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
        }
        return cycles;
    }

    // sceCdReadClock's BCD fields as "YY-MM-DD hh:mm:ss".
    struct Clock
    {
        uint8_t sec, min, hour, day, month, year;
        bool operator==(const Clock &) const = default;
    };

    Clock readClock(TestEnv &env)
    {
        R5900Context ctx{};
        setRegU32(ctx, 4, kClockAddr);
        ps2_stubs::sceCdReadClock(env.rdram.data(), &ctx, &env.runtime);
        const uint8_t *c = env.rdram.data() + kClockAddr;
        return Clock{c[1], c[2], c[3], c[5], c[6], c[7]};
    }

    constexpr Clock kClockStart{0x00, 0x00, 0x00, 0x01, 0x01, 0x00};
    constexpr Clock kClockPlusOneSecond{0x01, 0x00, 0x00, 0x01, 0x01, 0x00};
}

void register_ps2_runtime_deterministic_tests()
{
    MiniTest::Case("PS2RuntimeDeterministic", [](TestCase &tc)
    {
        tc.Run("PAL vsync n is due at exactly (n + 1) * 5898240 EE cycles", [](TestCase &t)
        {
            const std::vector<uint64_t> cycles = frameCycles(PS2Runtime::VideoMode::Pal, 1000u);
            t.Equals(static_cast<uint64_t>(cycles.size()), 1001ull, "every field should be observed");
            for (const uint64_t frame : {0ull, 1ull, 2ull, 1000ull})
            {
                t.Equals(cycles[frame], (frame + 1u) * 5898240ull, "PAL field cycle");
            }
        });

        tc.Run("NTSC vsync n is due at floor((n + 1) * kEeClockHz * 1001 / 60000) with no drift", [](TestCase &t)
        {
            const std::vector<uint64_t> cycles = frameCycles(PS2Runtime::VideoMode::Ntsc, 1000u);
            t.Equals(static_cast<uint64_t>(cycles.size()), 1001ull, "every field should be observed");
            // 294912000 * 1001 / 60000 = 4920115.2 cycles per field.
            t.Equals(cycles[0], 4920115ull, "NTSC field 0");
            t.Equals(cycles[1], 9840230ull, "NTSC field 1");
            t.Equals(cycles[2], 14760345ull, "NTSC field 2");
            t.Equals(cycles[1000], 4925035315ull, "NTSC field 1000: the fractional cycles must not accumulate");
            t.Equals(cycles[59], 295206912ull, "NTSC field 59: 60 periods are 1.001 s exactly");
        });

        tc.Run("an idle guest reaches 300 vsyncs in far less than 300 field periods of wall time", [](TestCase &t)
        {
            std::chrono::milliseconds wall{0};
            const std::vector<uint64_t> cycles = frameCycles(PS2Runtime::VideoMode::Ntsc, 299u, &wall);
            t.Equals(static_cast<uint64_t>(cycles.size()), 300ull, "300 fields should be observed");
            // 300 NTSC fields are ~5.0 s of emulated time.
            t.IsTrue(wall < std::chrono::milliseconds(1500), "deterministic pacing must not sleep on the host clock");
        });

        tc.Run("the RTC reads 2000-01-01 00:00:00 and advances one second per 50 PAL vsyncs", [](TestCase &t)
        {
            TestEnv env;
            env.runtime.setRunMode(deterministicMode(PS2Runtime::VideoMode::Pal));
            t.IsTrue(readClock(env) == kClockStart, "before the first vsync");

            std::vector<Clock> seen;
            runIdleGuest(env, deterministicMode(PS2Runtime::VideoMode::Pal), 49u,
                         [&](PS2Runtime &, uint64_t frame)
                         {
                             if (frame == 0u || frame == 48u || frame == 49u)
                             {
                                 seen.push_back(readClock(env));
                             }
                         });
            t.Equals(static_cast<uint64_t>(seen.size()), 3ull, "three reads");
            t.IsTrue(seen[0] == kClockStart, "after 1 PAL vsync");
            t.IsTrue(seen[1] == kClockStart, "after 49 PAL vsyncs");
            t.IsTrue(seen[2] == kClockPlusOneSecond, "after 50 PAL vsyncs");
        });

        tc.Run("the RTC advances one second per 60 NTSC vsyncs", [](TestCase &t)
        {
            TestEnv env;
            std::vector<Clock> seen;
            runIdleGuest(env, deterministicMode(PS2Runtime::VideoMode::Ntsc), 59u,
                         [&](PS2Runtime &, uint64_t frame)
                         {
                             if (frame == 0u || frame == 58u || frame == 59u)
                             {
                                 seen.push_back(readClock(env));
                             }
                         });
            t.Equals(static_cast<uint64_t>(seen.size()), 3ull, "three reads");
            t.IsTrue(seen[0] == kClockStart, "after 1 NTSC vsync");
            t.IsTrue(seen[1] == kClockStart, "after 59 NTSC vsyncs");
            t.IsTrue(seen[2] == kClockPlusOneSecond, "after 60 NTSC vsyncs");
        });

        tc.Run("the pad reads released with centred sticks in deterministic mode", [](TestCase &t)
        {
            TestEnv env;
            env.runtime.setRunMode(deterministicMode(PS2Runtime::VideoMode::Ntsc, false));
            t.IsFalse(env.runtime.padBackend().hostInputEnabled(), "host input should be off");

            uint8_t data[32];
            std::memset(data, 0xAA, sizeof(data));
            t.IsTrue(env.runtime.padBackend().readState(0, 0, data, sizeof(data)), "readState should succeed");
            t.Equals(static_cast<uint32_t>(data[2]), 0xFFu, "no button in the low byte");
            t.Equals(static_cast<uint32_t>(data[3]), 0xFFu, "no button in the high byte");
            for (int stick = 4; stick < 8; ++stick)
            {
                t.Equals(static_cast<uint32_t>(data[stick]), 0x80u, "sticks should be centred");
            }

            R5900Context ctx{};
            ps2_stubs::scePadInit(env.rdram.data(), &ctx, &env.runtime);
            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, kPadDataAddr + 0x200u);
            ps2_stubs::scePadPortOpen(env.rdram.data(), &ctx, &env.runtime);
            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, kPadDataAddr);
            ps2_stubs::scePadRead(env.rdram.data(), &ctx, &env.runtime);
            const uint8_t *padData = env.rdram.data() + kPadDataAddr;
            t.Equals(static_cast<uint32_t>(padData[2] | (padData[3] << 8)), 0xFFFFu, "scePadRead: nothing pressed");

            ps2_stubs::setPadOverrideState(0xBFFFu, 0x00, 0xFF, 0x10, 0xEE);
            ps2_stubs::scePadRead(env.rdram.data(), &ctx, &env.runtime);
            t.Equals(static_cast<uint32_t>(padData[2] | (padData[3] << 8)), 0xBFFFu, "the override still wins");
            ps2_stubs::clearPadOverrideState();
            ps2_stubs::scePadPortClose(env.rdram.data(), &ctx, &env.runtime);
        });

        tc.Run("the vsync observer sees frames 0, 1, 2 in order on the game thread and can stop the run", [](TestCase &t)
        {
            TestEnv env;
            std::vector<uint64_t> frames;
            std::vector<uint64_t> ticks;
            bool onGameThread = true;
            const std::thread::id gameThread = std::this_thread::get_id();
            runIdleGuest(env, deterministicMode(PS2Runtime::VideoMode::Pal), 4u,
                         [&](PS2Runtime &runtime, uint64_t frame)
                         {
                             frames.push_back(frame);
                             ticks.push_back(runtime.eeScheduler().currentVSyncTick());
                             onGameThread = onGameThread && std::this_thread::get_id() == gameThread;
                             // Called before the guest sees the field: CSR.VSINT is already set.
                             onGameThread = onGameThread &&
                                            (runtime.memory().gs().csr.load() & 0x8ull) != 0u;
                         });
            const std::vector<uint64_t> expected{0, 1, 2, 3, 4};
            t.IsTrue(frames == expected, "frames should count up from 0 and stop after the requested one");
            t.IsTrue(ticks == std::vector<uint64_t>({1, 2, 3, 4, 5}), "the tick is already incremented");
            t.IsTrue(onGameThread, "the observer should run on the game thread, after CSR.VSINT is set");
            t.Equals(env.runtime.eeScheduler().currentVSyncTick(), 5ull, "no field after the stop is processed");
        });

        tc.Run("the observer also runs in the default mode", [](TestCase &t)
        {
            TestEnv env;
            env.runtime.registerFunction(kIdleLoopPc, idleLoop);
            std::vector<uint64_t> frames;
            env.runtime.setVSyncObserver([&](PS2Runtime &runtime, uint64_t frame)
                                         {
                frames.push_back(frame);
                if (frame == 1u)
                {
                    runtime.requestStop();
                } });
            R5900Context mainContext{};
            mainContext.pc = kIdleLoopPc;
            env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
            env.runtime.eeScheduler().run();
            t.IsTrue(frames == std::vector<uint64_t>({0, 1}), "two fields, paced by the host clock");
        });

        tc.Run("a headless runtime initialises and runs without a window or audio device", [](TestCase &t)
        {
            TestEnv env;
            PS2Runtime::RunMode mode = deterministicMode(PS2Runtime::VideoMode::Ntsc);
            env.runtime.setRunMode(mode);
            t.IsTrue(env.runtime.initialize("headless test"), "initialize should not need a display");
        });

        tc.Run("the default run mode is the interactive one", [](TestCase &t)
        {
            PS2Runtime runtime;
            t.IsFalse(runtime.runMode().headless, "not headless by default");
            t.IsFalse(runtime.runMode().deterministic, "not deterministic by default");
            t.IsTrue(runtime.runMode().videoMode == PS2Runtime::VideoMode::Ntsc, "NTSC by default");
            t.IsTrue(runtime.padBackend().hostInputEnabled(), "host input is on by default");
            t.IsFalse(ps2_host_clock::deterministic(), "the host clock is real by default");
        });
    });
}
