#pragma once

#include "ps2x/iop/iop_types.h"

#include <cstdint>

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopKernel;

    class IopVblank
    {
    public:
        explicit IopVblank(IopKernel &kernel) noexcept;

        [[nodiscard]] bool dispatchImport(uint16_t ordinal, IopCpuState &cpu, uint64_t currentCycle);
        void setTiming(const VblankTiming &timing) noexcept { m_timing = timing; }

    private:
        IopKernel &m_kernel;
        VblankTiming m_timing{};
    };
}
