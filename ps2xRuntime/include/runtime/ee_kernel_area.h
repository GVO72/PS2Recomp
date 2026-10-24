#pragma once

#include <cstdint>

// Guest RAM below 0x00100000 is the BIOS kernel's area on hardware. Everything the
// runtime keeps in guest memory for itself (HLE allocations, RPC/TLS/bootmode pools,
// interrupt/callback stacks) lives here, so the game owns all user RAM from the end of
// its image up to its stack, as it does on hardware.
//
// Pages the layout stays clear of: 0x0 to 0x24FFF (exception vectors, the SetSyscall mirror
// table at physical 0x11F80 + 4 * n, two probe words at 0x2F0/0x2F8, and kernel data the
// reference run keeps there), 0x75000 to 0x82FFF (code and data that programs of the SDK
// itself copy there) and the page at 0xA0000.
namespace ps2_kernel_area
{
    inline constexpr uint32_t kEnd = 0x00100000u;

    // Interrupt/alarm/callback invocation stacks, handed out from the top downwards.
    inline constexpr uint32_t kStackPoolBase = 0x00025000u;
    inline constexpr uint32_t kStackPoolEnd = 0x00075000u;

    inline constexpr uint32_t kRpcPacketPoolBase = 0x000A1000u;
    inline constexpr uint32_t kRpcPacketPoolBytes = 0x00010000u;
    inline constexpr uint32_t kRpcServerPoolBase = kRpcPacketPoolBase + kRpcPacketPoolBytes;
    inline constexpr uint32_t kRpcServerPoolBytes = 0x00010000u;
    inline constexpr uint32_t kTlsPoolBase = kRpcServerPoolBase + kRpcServerPoolBytes;
    inline constexpr uint32_t kTlsPoolBytes = 0x00010000u;
    inline constexpr uint32_t kBootModePoolBase = kTlsPoolBase + kTlsPoolBytes;
    inline constexpr uint32_t kBootModePoolBytes = 0x00001000u;

    // Heap behind guestMalloc (HLE stubs).
    inline constexpr uint32_t kHeapBase = kBootModePoolBase + kBootModePoolBytes;
    inline constexpr uint32_t kHeapEnd = kEnd;

    static_assert(kStackPoolBase < kStackPoolEnd);
    static_assert(kStackPoolEnd <= 0x00075000u);
    static_assert(kStackPoolBase >= 0x00025000u);
    static_assert(kRpcPacketPoolBase >= 0x000A1000u);
    static_assert(kHeapBase < kHeapEnd);
    static_assert(kHeapEnd <= 0x00100000u);
}
