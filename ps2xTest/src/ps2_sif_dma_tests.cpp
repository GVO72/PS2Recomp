#include "MiniTest.h"
#include "ps2_runtime.h"
#include "ps2_iop_host.h"
#include "ps2_iop_transport.h"
#include "ps2_syscalls.h"
#include "ps2_stubs.h"
#include "Kernel/Stubs/SIF.h"
#include "Kernel/Stubs/SifCmdProtocol.h"
#include "runtime/ee_scheduler.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ps2_stubs
{
    void resetSifState();
}

namespace
{
    constexpr int KE_OK = 0;

    struct TestEnv
    {
        std::vector<uint8_t> rdram;
        R5900Context ctx{};
        PS2Runtime runtime;

        TestEnv() : rdram(PS2_RAM_SIZE, 0u)
        {
            ps2_stubs::resetSifState();
            std::memset(&ctx, 0, sizeof(ctx));
        }
    };

    #pragma pack(push, 1)
    struct Ps2SifDmaTransfer
    {
        uint32_t src;
        uint32_t dest;
        int32_t size;
        int32_t attr;
    };

    struct SifRpcHeader
    {
        uint32_t pkt_addr;
        uint32_t rpc_id;
        int32_t sema_id;
        uint32_t mode;
    };

    struct SifRpcReceiveData
    {
        SifRpcHeader hdr;
        uint32_t src;
        uint32_t dest;
        int32_t size;
    };
    #pragma pack(pop)

    static_assert(sizeof(Ps2SifDmaTransfer) == 16u, "Unexpected Ps2SifDmaTransfer size.");
    static_assert(sizeof(SifRpcReceiveData) == 28u, "Unexpected SifRpcReceiveData size.");

    void setRegU32(R5900Context &ctx, int reg, uint32_t value)
    {
        ctx.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(value));
    }

    int32_t getRegS32(const R5900Context &ctx, int reg)
    {
        return static_cast<int32_t>(::getRegU32(&ctx, reg));
    }

    void writeGuestU32(uint8_t *rdram, uint32_t addr, uint32_t value)
    {
        std::memcpy(rdram + addr, &value, sizeof(value));
    }

    uint32_t readGuestU32(const uint8_t *rdram, uint32_t addr)
    {
        uint32_t value = 0;
        std::memcpy(&value, rdram + addr, sizeof(value));
        return value;
    }

    uint32_t g_dmacHandlerWriteAddr = 0u;
    uint32_t g_dmacHandlerValue = 0u;
    uint32_t g_dmacHandlerLastCause = 0u;
    uint32_t g_dmacHandlerLastArg = 0u;
    int32_t g_sifDmaResult = 0;

    constexpr uint32_t kSchedulerSifDmaEntryPc = 0x00101000u;
    constexpr uint32_t kSchedulerSifDmaResumePc = 0x00101010u;
    constexpr uint32_t kSchedulerSifDmaHandlerPc = 0x00101020u;
    constexpr uint32_t kSchedulerSifDmaDescAddr = 0x00020300u;
    constexpr uint32_t kSchedulerSifDmaHandlerArg = 0x12345678u;

    void testDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        g_dmacHandlerLastCause = ::getRegU32(ctx, 4);
        g_dmacHandlerLastArg = ::getRegU32(ctx, 5);
        if (g_dmacHandlerWriteAddr != 0u)
        {
            writeGuestU32(rdram, g_dmacHandlerWriteAddr, g_dmacHandlerValue);
        }
        ctx->pc = 0u;
    }

    void schedulerSifDmaEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        runtime->eeScheduler().addIrqHandler(true,
                                             5u,
                                             kSchedulerSifDmaHandlerPc,
                                             true,
                                             kSchedulerSifDmaHandlerArg,
                                             0u,
                                             0u);
        setRegU32(*ctx, 4, kSchedulerSifDmaDescAddr);
        setRegU32(*ctx, 5, 1u);
        ctx->pc = kSchedulerSifDmaResumePc;
        ps2_stubs::sceSifSetDma(rdram, ctx, runtime);
    }

    // Send one SIF command packet from EE RAM to the IOP command buffer, the way
    // the EE's sceSifSendCmd does: a single SetDma descriptor.
    void sendSifCommand(TestEnv &env, uint32_t iopBuffer, const std::vector<uint32_t> &words)
    {
        constexpr uint32_t kCommandSrcAddr = 0x00022000u;
        constexpr uint32_t kCommandDescAddr = 0x00022100u;
        std::memcpy(env.rdram.data() + kCommandSrcAddr, words.data(), words.size() * sizeof(uint32_t));
        const Ps2SifDmaTransfer desc{
            kCommandSrcAddr,
            iopBuffer,
            static_cast<int32_t>(words.size() * sizeof(uint32_t)),
            0x44};
        std::memcpy(env.rdram.data() + kCommandDescAddr, &desc, sizeof(desc));
        setRegU32(env.ctx, 4, kCommandDescAddr);
        setRegU32(env.ctx, 5, 1u);
        ps2_stubs::sceSifSetDma(env.rdram.data(), &env.ctx, &env.runtime);
    }

    uint32_t querySifReg(TestEnv &env, uint32_t reg)
    {
        setRegU32(env.ctx, 4, reg);
        ps2_stubs::sceSifGetReg(env.rdram.data(), &env.ctx, &env.runtime);
        return ::getRegU32(&env.ctx, 2);
    }

    void schedulerSifDmaResume(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_sifDmaResult = getRegS32(*ctx, 2);
        ctx->pc = 0u;
        runtime->requestStop();
    }
}

void register_ps2_sif_dma_tests()
{
    MiniTest::Case("PS2SifDma", [](TestCase &tc)
    {
        tc.Run("sceSifSetDma copies payload and sceSifDmaStat reports complete", [](TestCase &t)
        {
            TestEnv env;

            constexpr uint32_t kDescAddr = 0x00020000u;
            constexpr uint32_t kSrcAddr = 0x00020100u;
            constexpr uint32_t kDstAddr = 0x00020200u;

            std::array<uint8_t, 16> payload{};
            for (size_t i = 0; i < payload.size(); ++i)
            {
                payload[i] = static_cast<uint8_t>(0x30u + i);
            }
            std::memcpy(env.rdram.data() + kSrcAddr, payload.data(), payload.size());
            std::memset(env.rdram.data() + kDstAddr, 0x5A, payload.size());

            const Ps2SifDmaTransfer desc{
                kSrcAddr,
                kDstAddr,
                static_cast<int32_t>(payload.size()),
                0};
            std::memcpy(env.rdram.data() + kDescAddr, &desc, sizeof(desc));

            setRegU32(env.ctx, 4, kDescAddr);
            setRegU32(env.ctx, 5, 1u);
            ps2_stubs::sceSifSetDma(env.rdram.data(), &env.ctx, &env.runtime);
            const int32_t dmaId = getRegS32(env.ctx, 2);
            t.IsTrue(dmaId > 0, "sceSifSetDma should return a positive transfer id on success");

            std::array<uint8_t, 16> iopReadback{};
            t.IsTrue(env.runtime.readIopMemory(kDstAddr, iopReadback.data(), iopReadback.size()) &&
                         iopReadback == payload,
                     "sceSifSetDma should copy EE payload into IOP RAM");
            const std::array<uint8_t, 16> eeSentinel = {
                0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A,
                0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A};
            t.IsTrue(std::memcmp(env.rdram.data() + kDstAddr, eeSentinel.data(), eeSentinel.size()) == 0,
                     "sceSifSetDma must not alias an equal-numbered EE address");

            setRegU32(env.ctx, 4, static_cast<uint32_t>(dmaId));
            ps2_stubs::sceSifDmaStat(env.rdram.data(), &env.ctx, &env.runtime);
            t.IsTrue(getRegS32(env.ctx, 2) < 0, "sceSifDmaStat should be negative when transfer is complete");
        });

        tc.Run("SIF command protocol answers SIFRPC initialisation with SET_SREG", [](TestCase &t)
        {
            TestEnv env;
            constexpr uint32_t kEePacketBuffer = 0x20021000u; // uncached, as the EE announces it
            constexpr uint32_t kEePacketPhysical = 0x00021000u;

            const uint32_t iopBuffer = querySifReg(env, 0x2u);
            t.IsTrue(iopBuffer != 0u, "SIF_REG_SUBADDR should report an IOP command buffer");
            t.IsTrue(ps2_stubs::sif_protocol::isIopCommandBuffer(iopBuffer),
                     "the reported buffer should be recognised as the command buffer");

            // sceSifInitCmd: INIT_CMD with opt 0 carries the EE packet buffer; no reply.
            sendSifCommand(env, iopBuffer, {20u, 0u, 0x80000002u, 0u, kEePacketBuffer});
            t.Equals(readGuestU32(env.rdram.data(), kEePacketPhysical), 0u,
                     "SIFCMD initialisation should not produce a reply");

            // sceSifInitRpc: INIT_CMD with opt 1; the IOP sets SREG 0 to 1.
            sendSifCommand(env, iopBuffer, {16u, 0u, 0x80000002u, 1u});
            t.Equals(readGuestU32(env.rdram.data(), kEePacketPhysical) & 0xFFu, 24u,
                     "the reply should be a 24-byte packet");
            t.Equals(readGuestU32(env.rdram.data(), kEePacketPhysical + 8u), 0x80000001u,
                     "the reply should be SIF_CMD_SET_SREG");
            t.Equals(readGuestU32(env.rdram.data(), kEePacketPhysical + 16u), 0u, "SREG index 0");
            t.Equals(readGuestU32(env.rdram.data(), kEePacketPhysical + 20u), 1u, "SREG value 1");
        });

        tc.Run("SIF command protocol queues replies until the EE frees its packet buffer", [](TestCase &t)
        {
            TestEnv env;
            constexpr uint32_t kEePacketPhysical = 0x00021000u;
            constexpr uint32_t kClient = 0x00023000u;
            const uint32_t iopBuffer = querySifReg(env, 0x2u);
            sendSifCommand(env, iopBuffer, {20u, 0u, 0x80000002u, 0u, kEePacketPhysical});

            // Two binds to a SID nothing serves: each is answered by RPC_END with
            // no server, which tells the EE to retry.
            const std::vector<uint32_t> bind{64u, 0u, 0x80000009u, 0u, 7u, 0x00024000u, 3u, kClient, 0x12340000u,
                                             0u, 0u, 0u, 0u, 0u, 0u, 0u};
            sendSifCommand(env, iopBuffer, bind);
            sendSifCommand(env, iopBuffer, bind);

            t.Equals(readGuestU32(env.rdram.data(), kEePacketPhysical + 8u), 0x80000008u,
                     "the first reply should be SIF_CMD_RPC_END");
            t.Equals(readGuestU32(env.rdram.data(), kEePacketPhysical + 16u), 7u, "rec_id is echoed");
            t.Equals(readGuestU32(env.rdram.data(), kEePacketPhysical + 20u), 0x00024000u, "pkt_addr is echoed");
            t.Equals(readGuestU32(env.rdram.data(), kEePacketPhysical + 24u), 3u, "rpc_id is echoed");
            t.Equals(readGuestU32(env.rdram.data(), kEePacketPhysical + 28u), kClient, "the client is echoed");
            t.Equals(readGuestU32(env.rdram.data(), kEePacketPhysical + 32u), 0x80000009u, "it ends a bind");
            t.Equals(readGuestU32(env.rdram.data(), kEePacketPhysical + 36u), 0u, "no server for an unknown SID");

            // The EE handler has not cleared psize yet, so the second reply waits.
            writeGuestU32(env.rdram.data(), kEePacketPhysical + 16u, 0xFFFFFFFFu);
            ps2_stubs::sif_protocol::deliverPending(env.rdram.data(), &env.runtime);
            t.Equals(readGuestU32(env.rdram.data(), kEePacketPhysical + 16u), 0xFFFFFFFFu,
                     "a reply must not overwrite an unconsumed packet");

            // Once the handler clears psize, the next reply is delivered.
            writeGuestU32(env.rdram.data(), kEePacketPhysical, 0u);
            ps2_stubs::sif_protocol::deliverPending(env.rdram.data(), &env.runtime);
            t.Equals(readGuestU32(env.rdram.data(), kEePacketPhysical + 8u), 0x80000008u,
                     "the second reply should be delivered after the first is consumed");
            t.Equals(readGuestU32(env.rdram.data(), kEePacketPhysical + 16u), 7u,
                     "the second reply should overwrite the consumed packet");
        });

        tc.Run("SIF command protocol RPC calls touch EE memory only through replies", [](TestCase &t)
        {
            TestEnv env;
            constexpr uint32_t kEePacketPhysical = 0x00021000u;
            constexpr uint32_t kClient = 0x00023000u;
            constexpr uint32_t kEePacketAddr = 0x00024000u;
            constexpr uint32_t kArgAddr = 0x00025000u;
            constexpr uint32_t kTextAddr = 0x00026000u;
            constexpr uint32_t kRecvAddr = 0x00027000u;
            constexpr uint32_t kCallSrcAddr = 0x00028000u;
            constexpr uint32_t kCallDescAddr = 0x00028100u;
            const uint32_t iopBuffer = querySifReg(env, 0x2u);
            sendSifCommand(env, iopBuffer, {20u, 0u, 0x80000002u, 0u, kEePacketPhysical});

            // Bind FILEIO, which the IOP always provides.
            sendSifCommand(env, iopBuffer, {64u, 0u, 0x80000009u, 0u, 1u, kEePacketAddr, 2u, kClient, 0x80000001u,
                                            0u, 0u, 0u, 0u, 0u, 0u, 0u});
            const uint32_t server = readGuestU32(env.rdram.data(), kEePacketPhysical + 36u);
            const uint32_t serverBuffer = readGuestU32(env.rdram.data(), kEePacketPhysical + 40u);
            t.IsTrue(server != 0u && serverBuffer != 0u, "binding FILEIO should return a server and its buffer");
            writeGuestU32(env.rdram.data(), kEePacketPhysical, 0u); // the EE consumed the reply

            // fioWrite(1, "hello\n", 6): the argument goes to the server buffer,
            // then the call packet to the command buffer, in one SetDma.
            const std::array<uint32_t, 8> arg{1u, kTextAddr, 6u, 0u, 0u, 0u, 0u, 0u};
            std::memcpy(env.rdram.data() + kArgAddr, arg.data(), sizeof(arg));
            std::memcpy(env.rdram.data() + kTextAddr, "hello\n", 6u);
            const std::array<uint32_t, 16> call{64u, 0u, 0x8000000Au, 0u, 3u, kEePacketAddr, 4u, kClient, 3u,
                                                32u, kRecvAddr, 4u, 1u, server, 0u, 0u};
            std::memcpy(env.rdram.data() + kCallSrcAddr, call.data(), sizeof(call));
            const std::array<Ps2SifDmaTransfer, 2> descs{{
                {kArgAddr, serverBuffer, 32, 0},
                {kCallSrcAddr, iopBuffer, 64, 0x44},
            }};
            std::memcpy(env.rdram.data() + kCallDescAddr, descs.data(), sizeof(descs));

            const std::vector<uint8_t> before = env.rdram;
            setRegU32(env.ctx, 4, kCallDescAddr);
            setRegU32(env.ctx, 5, 2u);
            ps2_stubs::sceSifSetDma(env.rdram.data(), &env.ctx, &env.runtime);

            t.Equals(readGuestU32(env.rdram.data(), kRecvAddr), 6u, "the write result reaches the receive buffer");
            t.Equals(readGuestU32(env.rdram.data(), kEePacketPhysical + 8u), 0x80000008u, "RPC_END is delivered");
            t.Equals(readGuestU32(env.rdram.data(), kEePacketPhysical + 32u), 0x8000000Au, "it ends the call");

            bool untouched = true;
            for (size_t i = 0; i < before.size(); ++i)
            {
                const bool reply = i >= kEePacketPhysical && i < kEePacketPhysical + 128u;
                const bool received = i >= kRecvAddr && i < kRecvAddr + 4u;
                if (!reply && !received && env.rdram[i] != before[i])
                {
                    untouched = false;
                    break;
                }
            }
            t.IsTrue(untouched, "an RPC call must not write EE memory outside its reply and receive buffer");
        });

        tc.Run("IOP heap DMA uses private backing instead of aliasing EE RDRAM", [](TestCase &t)
        {
            TestEnv env;

            constexpr uint32_t kDescAddr = 0x00020040u;
            constexpr uint32_t kSrcAddr = 0x00020140u;
            constexpr uint32_t kRoundTripAddr = 0x00020240u;
            constexpr uint32_t kIopBlockSize = 0x880u;

            std::array<uint8_t, 32> payload{};
            for (size_t i = 0; i < payload.size(); ++i)
            {
                payload[i] = static_cast<uint8_t>(0x80u + i);
            }
            std::memcpy(env.rdram.data() + kSrcAddr, payload.data(), payload.size());
            std::memset(env.rdram.data() + kRoundTripAddr, 0, payload.size());

            setRegU32(env.ctx, 4, kIopBlockSize);
            ps2_stubs::sceSifAllocIopHeap(env.rdram.data(), &env.ctx, &env.runtime);
            const uint32_t iopAddress = ::getRegU32(&env.ctx, 2);
            t.IsTrue(iopAddress >= 0x00120000u && iopAddress < 0x00200000u,
                     "sceSifAllocIopHeap should return an address in physical IOP RAM");
            std::memset(env.rdram.data() + iopAddress, 0x5Au, payload.size());

            Ps2SifDmaTransfer desc{
                kSrcAddr,
                iopAddress,
                static_cast<int32_t>(payload.size()),
                0};
            std::memcpy(env.rdram.data() + kDescAddr, &desc, sizeof(desc));
            setRegU32(env.ctx, 4, kDescAddr);
            setRegU32(env.ctx, 5, 1u);
            ps2_stubs::sceSifSetDma(env.rdram.data(), &env.ctx, &env.runtime);
            t.IsTrue(getRegS32(env.ctx, 2) > 0,
                     "EE-to-IOP DMA should accept a private IOP heap destination");

            const std::array<uint8_t, 32> aliasSentinel{
                0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A,
                0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A,
                0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A,
                0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A};
            t.IsTrue(std::memcmp(env.rdram.data() + iopAddress,
                                 aliasSentinel.data(), aliasSentinel.size()) == 0,
                     "IOP DMA must not overwrite the equal-numbered EE range");

            PS2IopHostAdapter host(env.runtime);
            auto scope = host.enterCall(&env.ctx, env.rdram.data());
            std::array<uint8_t, 32> hostReadback{};
            t.IsTrue(host.readIopMemory(iopAddress, hostReadback.data(), hostReadback.size()) &&
                         hostReadback == payload,
                     "IOP modules should read the shared physical IOP RAM");

            constexpr uint32_t kRdAddr = 0x00020340u;
            setRegU32(env.ctx, 4, kRdAddr);
            setRegU32(env.ctx, 5, iopAddress);
            setRegU32(env.ctx, 6, kRoundTripAddr);
            setRegU32(env.ctx, 7, static_cast<uint32_t>(payload.size()));
            ps2_stubs::sceSifGetOtherData(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), 0,
                     "IOP-to-EE transfer should accept a physical IOP source");
            t.IsTrue(std::memcmp(env.rdram.data() + kRoundTripAddr,
                                 payload.data(), payload.size()) == 0,
                     "IOP-to-EE DMA should round-trip the payload");
        });

        tc.Run("isceSifSetDma and isceSifSetDChain alias the SIF DMA helpers", [](TestCase &t)
        {
            TestEnv env;

            constexpr uint32_t kDescAddr = 0x00020240u;
            constexpr uint32_t kSrcAddr = 0x00020340u;
            constexpr uint32_t kDstAddr = 0x00020440u;

            std::array<uint8_t, 12> payload{};
            for (size_t i = 0; i < payload.size(); ++i)
            {
                payload[i] = static_cast<uint8_t>(0x50u + i);
            }
            std::memcpy(env.rdram.data() + kSrcAddr, payload.data(), payload.size());
            std::memset(env.rdram.data() + kDstAddr, 0x5A, payload.size());

            const Ps2SifDmaTransfer desc{
                kSrcAddr,
                kDstAddr,
                static_cast<int32_t>(payload.size()),
                0};
            std::memcpy(env.rdram.data() + kDescAddr, &desc, sizeof(desc));

            setRegU32(env.ctx, 4, kDescAddr);
            setRegU32(env.ctx, 5, 1u);
            ps2_stubs::isceSifSetDma(env.rdram.data(), &env.ctx, &env.runtime);
            t.IsTrue(getRegS32(env.ctx, 2) > 0, "isceSifSetDma should report a successful transfer id");
            std::array<uint8_t, 12> iopReadback{};
            t.IsTrue(env.runtime.readIopMemory(kDstAddr, iopReadback.data(), iopReadback.size()) &&
                         iopReadback == payload,
                     "isceSifSetDma should copy EE payload into IOP RAM");

            ps2_stubs::isceSifSetDChain(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), 0, "isceSifSetDChain should mirror sceSifSetDChain");
        });

        tc.Run("sceSifSetDma dispatches enabled DMAC handlers for cause 5", [](TestCase &t)
        {
            TestEnv env;

            constexpr uint32_t kSrcAddr = 0x00020400u;
            constexpr uint32_t kDstAddr = 0x00020500u;
            constexpr uint32_t kHandlerWriteAddr = 0x00020600u;

            g_dmacHandlerWriteAddr = kHandlerWriteAddr;
            g_dmacHandlerValue = 0xCAFEBABEu;
            g_dmacHandlerLastCause = 0u;
            g_dmacHandlerLastArg = 0u;
            g_sifDmaResult = 0;
            env.runtime.registerFunction(kSchedulerSifDmaEntryPc, schedulerSifDmaEntry);
            env.runtime.registerFunction(kSchedulerSifDmaResumePc, schedulerSifDmaResume);
            env.runtime.registerFunction(kSchedulerSifDmaHandlerPc, testDmacHandler);

            std::array<uint8_t, 16> payload{};
            for (size_t i = 0; i < payload.size(); ++i)
            {
                payload[i] = static_cast<uint8_t>(0x40u + i);
            }
            std::memcpy(env.rdram.data() + kSrcAddr, payload.data(), payload.size());

            const Ps2SifDmaTransfer desc{
                kSrcAddr,
                kDstAddr,
                static_cast<int32_t>(payload.size()),
                0};
            std::memcpy(env.rdram.data() + kSchedulerSifDmaDescAddr, &desc, sizeof(desc));

            R5900Context mainContext{};
            mainContext.pc = kSchedulerSifDmaEntryPc;
            env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
            env.runtime.eeScheduler().run();

            t.IsTrue(g_sifDmaResult > 0, "sceSifSetDma should still report success");
            t.Equals(readGuestU32(env.rdram.data(), kHandlerWriteAddr), g_dmacHandlerValue,
                     "the scheduler should execute the queued DMAC invocation");
            t.Equals(g_dmacHandlerLastCause, 5u, "DMAC handler should observe cause 5");
            t.Equals(g_dmacHandlerLastArg, kSchedulerSifDmaHandlerArg,
                     "DMAC handler should receive registered argument");
        });

        tc.Run("resetSifState seeds boot-ready SIF registers", [](TestCase &t)
        {
            TestEnv env;

            auto getReg = [&](uint32_t reg) -> uint32_t
            {
                setRegU32(env.ctx, 4, reg);
                ps2_stubs::sceSifGetReg(env.rdram.data(), &env.ctx, &env.runtime);
                return ::getRegU32(&env.ctx, 2);
            };

            t.Equals(getReg(0x4u), 0x00020000u, "SIF boot status register should expose ready bit by default");
            t.Equals(getReg(0x80000000u), 0u, "SIF main-address register should default to zero");
            t.Equals(getReg(0x80000001u), 0u, "SIF sub-address register should default to zero");
            t.Equals(getReg(0x80000002u), 0u, "SIF mscom register should default to zero");
        });

        tc.Run("sceSifExitCmd restores default boot-ready SIF registers", [](TestCase &t)
        {
            TestEnv env;

            setRegU32(env.ctx, 4, 0x4u);
            setRegU32(env.ctx, 5, 0x12340000u);
            ps2_stubs::sceSifSetReg(env.rdram.data(), &env.ctx, &env.runtime);

            setRegU32(env.ctx, 4, 0x80000002u);
            setRegU32(env.ctx, 5, 0x89ABCDEFu);
            ps2_stubs::sceSifSetReg(env.rdram.data(), &env.ctx, &env.runtime);

            ps2_stubs::sceSifExitCmd(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), 0, "sceSifExitCmd should succeed");

            auto getReg = [&](uint32_t reg) -> uint32_t
            {
                setRegU32(env.ctx, 4, reg);
                ps2_stubs::sceSifGetReg(env.rdram.data(), &env.ctx, &env.runtime);
                return ::getRegU32(&env.ctx, 2);
            };

            t.Equals(getReg(0x4u), 0x00020000u, "sceSifExitCmd should restore the boot-ready status bit");
            t.Equals(getReg(0x80000002u), 0u, "sceSifExitCmd should clear transient mscom state");
        });

        tc.Run("sceSifSetDma rejects invalid descriptors without partial writes", [](TestCase &t)
        {
            TestEnv env;

            constexpr uint32_t kDescAddr = 0x00021000u;
            constexpr uint32_t kSrcA = 0x00021100u;
            constexpr uint32_t kDstA = 0x00021200u;
            constexpr uint32_t kSrcB = 0x00021300u;
            constexpr uint32_t kInvalidDstB = 0xE0000100u; // unsupported guest segment

            std::array<uint8_t, 8> payloadA{};
            for (size_t i = 0; i < payloadA.size(); ++i)
            {
                payloadA[i] = static_cast<uint8_t>(0x70u + i);
            }
            std::array<uint8_t, 8> payloadB{};
            for (size_t i = 0; i < payloadB.size(); ++i)
            {
                payloadB[i] = static_cast<uint8_t>(0x90u + i);
            }

            std::memcpy(env.rdram.data() + kSrcA, payloadA.data(), payloadA.size());
            std::memcpy(env.rdram.data() + kSrcB, payloadB.data(), payloadB.size());
            std::memset(env.rdram.data() + kDstA, 0x5Au, payloadA.size());

            const Ps2SifDmaTransfer descs[2] = {
                {kSrcA, kDstA, static_cast<int32_t>(payloadA.size()), 0},
                {kSrcB, kInvalidDstB, static_cast<int32_t>(payloadB.size()), 0}};
            std::memcpy(env.rdram.data() + kDescAddr, descs, sizeof(descs));

            setRegU32(env.ctx, 4, kDescAddr);
            setRegU32(env.ctx, 5, 2u);
            ps2_stubs::sceSifSetDma(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), 0, "sceSifSetDma should fail when any descriptor is invalid");

            const std::array<uint8_t, 8> expectedUnchanged{
                0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A};
            t.IsTrue(std::memcmp(env.rdram.data() + kDstA, expectedUnchanged.data(), expectedUnchanged.size()) == 0,
                     "failed multi-descriptor sceSifSetDma should not partially write earlier descriptors");
        });

        tc.Run("sceSifSetDma enforces descriptor count limit", [](TestCase &t)
        {
            TestEnv env;
            constexpr uint32_t kDescAddr = 0x00022000u;

            setRegU32(env.ctx, 4, kDescAddr);
            setRegU32(env.ctx, 5, 33u);
            ps2_stubs::sceSifSetDma(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), 0, "sceSifSetDma should reject count > 32");
        });

        tc.Run("sceSifGetOtherData copies payload and writes receive metadata", [](TestCase &t)
        {
            TestEnv env;

            constexpr uint32_t kRdAddr = 0x00023000u;
            constexpr uint32_t kSrcAddr = 0x00023100u;
            constexpr uint32_t kDstAddr = 0x00023200u;
            constexpr uint32_t kSize = 20u;

            std::array<uint8_t, kSize> payload{};
            for (size_t i = 0; i < payload.size(); ++i)
            {
                payload[i] = static_cast<uint8_t>((i * 7u) & 0xFFu);
            }
            t.IsTrue(env.runtime.writeIopMemory(kSrcAddr, payload.data(), payload.size()),
                     "test setup should populate physical IOP RAM");
            std::memset(env.rdram.data() + kDstAddr, 0, payload.size());
            std::memset(env.rdram.data() + kRdAddr, 0, sizeof(SifRpcReceiveData));

            setRegU32(env.ctx, 4, kRdAddr);
            setRegU32(env.ctx, 5, kSrcAddr);
            setRegU32(env.ctx, 6, kDstAddr);
            setRegU32(env.ctx, 7, kSize);
            ps2_stubs::sceSifGetOtherData(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), 0, "sceSifGetOtherData should succeed for valid transfer");

            t.IsTrue(std::memcmp(env.rdram.data() + kDstAddr, payload.data(), payload.size()) == 0,
                     "sceSifGetOtherData should copy payload");

            const SifRpcReceiveData rd = *reinterpret_cast<const SifRpcReceiveData *>(env.rdram.data() + kRdAddr);
            t.Equals(rd.src, kSrcAddr, "receive metadata src should be populated");
            t.Equals(rd.dest, kDstAddr, "receive metadata dest should be populated");
            t.Equals(static_cast<uint32_t>(rd.size), kSize, "receive metadata size should be populated");
        });

        tc.Run("sceSifGetOtherData rejects unsupported guest segments", [](TestCase &t)
        {
            TestEnv env;

            constexpr uint32_t kRdAddr = 0x00024000u;
            constexpr uint32_t kDstAddr = 0x00024100u;
            constexpr uint32_t kInvalidSrcAddr = 0xE0000200u;
            constexpr uint32_t kSize = 16u;

            std::memset(env.rdram.data() + kDstAddr, 0xA5, kSize);
            writeGuestU32(env.rdram.data(), kRdAddr + 0x10u, 0x11111111u);
            writeGuestU32(env.rdram.data(), kRdAddr + 0x14u, 0x22222222u);
            writeGuestU32(env.rdram.data(), kRdAddr + 0x18u, 0x33333333u);

            setRegU32(env.ctx, 4, kRdAddr);
            setRegU32(env.ctx, 5, kInvalidSrcAddr);
            setRegU32(env.ctx, 6, kDstAddr);
            setRegU32(env.ctx, 7, kSize);
            ps2_stubs::sceSifGetOtherData(env.rdram.data(), &env.ctx, &env.runtime);
            t.Equals(getRegS32(env.ctx, 2), -1, "sceSifGetOtherData should fail for unsupported source segment");

            std::array<uint8_t, kSize> expected{};
            expected.fill(0xA5u);
            t.IsTrue(std::memcmp(env.rdram.data() + kDstAddr, expected.data(), expected.size()) == 0,
                     "failed sceSifGetOtherData should not modify destination");
            t.Equals(readGuestU32(env.rdram.data(), kRdAddr + 0x10u), 0x11111111u,
                     "failed sceSifGetOtherData should not overwrite rd metadata");
        });
    });
}
