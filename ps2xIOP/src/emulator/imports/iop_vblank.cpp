#include "iop_vblank.h"

#include "../core/iop_cpu.h"
#include "../iop_emulator_const.h"
#include "../core/iop_kernel.h"

namespace ps2x::iop::detail
{
    namespace
    {
        // Start cycle of field n at the exact rate: PAL 50 Hz, NTSC 60000/1001 Hz,
        // floor(n * kIopClockHz * 1001 / 60000) without overflowing 64 bits.
        uint64_t exactFieldStart(uint64_t n, bool pal)
        {
            if (pal)
                return n * (kIopClockHz / 50u);
            constexpr uint64_t kScaled = kIopClockHz * 1001u;
            return (n / 60'000u) * kScaled + ((n % 60'000u) * kScaled) / 60'000u;
        }

        // The field containing currentCycle: the largest n with start(n) <= currentCycle.
        uint64_t exactFieldIndex(uint64_t currentCycle, bool pal)
        {
            // An estimate from the floored period, then exact corrections.
            const uint64_t periodFloor = pal ? kIopClockHz / 50u : kIopClockHz * 1001u / 60'000u;
            uint64_t n = currentCycle / periodFloor;
            while (n > 0u && exactFieldStart(n, pal) > currentCycle)
                --n;
            // start() is floored, so the next field can begin at currentCycle itself.
            while (exactFieldStart(n + 1u, pal) <= currentCycle)
                ++n;
            return n;
        }
    }

    IopVblank::IopVblank(IopKernel &kernel) noexcept
        : m_kernel(kernel)
    {
    }

    bool IopVblank::dispatchImport(uint16_t ordinal, IopCpuState &cpu, uint64_t currentCycle)
    {
        switch (ordinal)
        {
        case 4: // WaitVblankStart
        case 5: // WaitVblankEnd
        case 6: // WaitVblank
        case 7: // WaitNonVblank
        {
            const bool waitForEnd = ordinal == 5u || ordinal == 7u;
            if (m_timing.exact)
            {
                const uint64_t period = m_timing.pal ? kIopClockHz / 50u : kVblankPeriodCycles;
                const uint64_t phase = waitForEnd ? period / 16u : 0u;
                const uint64_t field = exactFieldIndex(currentCycle, m_timing.pal);
                uint64_t wakeCycle = exactFieldStart(field, m_timing.pal) + phase;
                if (wakeCycle <= currentCycle)
                    wakeCycle = exactFieldStart(field + 1u, m_timing.pal) + phase;
                m_kernel.delayCurrentUntil(wakeCycle, cpu);
                cpu.gpr[2] = 0u;
                return true;
            }
            const uint64_t phase = waitForEnd ? kVblankEndPhaseCycles : 0u;
            const uint64_t fieldStart = currentCycle - (currentCycle % kVblankPeriodCycles);
            uint64_t wakeCycle = fieldStart + phase;
            if (wakeCycle <= currentCycle)
                wakeCycle += kVblankPeriodCycles;
            m_kernel.delayCurrentUntil(wakeCycle, cpu);
            cpu.gpr[2] = 0u;
            return true;
        }
        case 8: // RegisterVblankHandler
        case 9: // ReleaseVblankHandler
            // Callback delivery is not required by the scheduler wait ABI yet.
            cpu.gpr[2] = 0u;
            return true;
        default:
            return false;
        }
    }
}
