#pragma once

#include "ps2_runtime.h"

#include <cstddef>
#include <cstdint>

// The IOP side of the SIF command protocol, for EE code that talks to the IOP
// through raw SIF command packets instead of library functions the runtime
// replaces by name (for example programs built with the open-source PS2SDK).
//
// EE -> IOP: the EE DMAs a command packet to the IOP's command buffer (whose
// address it reads from SIF_REG_SUBADDR). sceSifSetDma hands every packet that
// lands there to onCommandPacket().
// IOP -> EE: a reply is written into the EE's packet buffer (announced by
// SIF_CMD_INIT_CMD or SIF_CMD_CHANGE_SADDR) and the SIF0 DMAC interrupt is
// raised, so the EE's own command interrupt handler consumes it. The buffer
// holds one packet, so replies are queued and delivered one per interrupt.
namespace ps2_stubs::sif_protocol
{
    // Forget the EE packet buffer, the bound servers and any queued replies.
    void reset();

    // The IOP command buffer address reported in SIF_REG_SUBADDR. Allocated in
    // IOP memory on first use; 0 if the IOP allocator cannot provide it.
    uint32_t iopCommandBuffer(PS2Runtime *runtime);
    bool isIopCommandBuffer(uint32_t iopAddress);

    // Handle one command packet the EE sent to the IOP command buffer.
    void onCommandPacket(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                         const uint8_t *packet, size_t size);

    // Deliver the next queued reply if the EE packet buffer is free, and raise
    // the SIF0 interrupt for it. Each delivery schedules the next one when the
    // EE's handler returns.
    void deliverPending(uint8_t *rdram, PS2Runtime *runtime);
}
