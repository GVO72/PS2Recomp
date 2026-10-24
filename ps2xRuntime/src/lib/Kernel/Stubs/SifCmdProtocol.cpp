#include "Common.h"
#include "SifCmdProtocol.h"
#include "../../ps2_iop_transport.h"
#include "runtime/ee_scheduler.h"

#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <vector>

namespace ps2_stubs::sif_protocol
{
    namespace
    {
        constexpr uint32_t kCmdChangeSaddr = 0x80000000u;
        constexpr uint32_t kCmdSetSreg = 0x80000001u;
        constexpr uint32_t kCmdInitCmd = 0x80000002u;
        constexpr uint32_t kCmdResetCmd = 0x80000003u;
        constexpr uint32_t kCmdRpcEnd = 0x80000008u;
        constexpr uint32_t kCmdRpcBind = 0x80000009u;
        constexpr uint32_t kCmdRpcCall = 0x8000000Au;
        constexpr uint32_t kCmdRpcRdata = 0x8000000Cu;

        constexpr uint32_t kSregRpcInit = 0u;
        constexpr uint32_t kDmacSif0 = 5u;
        constexpr uint32_t kCommandBufferSize = 128u;   // CMD_PACKET_MAX
        constexpr uint32_t kServerBufferSize = 0x10000u; // receives each call's send data
        constexpr uint32_t kRpcEndPacketSize = 48u;      // SifRpcRendPkt_t
        constexpr uint32_t kSetSregPacketSize = 24u;     // header + sreg + value
        constexpr uint32_t kMaxLogsPerKind = 16u;

        struct Reply
        {
            std::vector<uint8_t> packet;
        };

        struct BoundServer
        {
            uint32_t sid = 0u;
            uint32_t serverToken = 0u; // the sd the EE keeps in its client data
            uint32_t buffer = 0u;      // IOP buffer the EE sends call data to
        };

        std::mutex g_mutex;
        uint32_t g_iopCommandBuffer = 0u;
        uint32_t g_eePacketBuffer = 0u;
        std::deque<Reply> g_replies;
        std::vector<BoundServer> g_servers;
        std::unordered_map<std::string, uint32_t> g_logCounts;

        uint32_t word(const uint8_t *packet, size_t size, size_t index)
        {
            uint32_t value = 0u;
            if ((index + 1u) * 4u <= size)
                std::memcpy(&value, packet + index * 4u, sizeof(value));
            return value;
        }

        void putWord(std::vector<uint8_t> &packet, size_t index, uint32_t value)
        {
            std::memcpy(packet.data() + index * 4u, &value, sizeof(value));
        }

        std::vector<uint8_t> makePacket(uint32_t cid, uint32_t packetSize)
        {
            std::vector<uint8_t> packet(packetSize, 0u);
            putWord(packet, 0, packetSize & 0xFFu); // psize, no payload
            putWord(packet, 2, cid);
            return packet;
        }

        // Physical EE RAM range, or nullptr. SIF DMA only reaches main RAM.
        uint8_t *eeRam(uint8_t *rdram, uint32_t address, size_t size)
        {
            const uint32_t physical = address & 0x1FFFFFFFu;
            if (!rdram || size > PS2_RAM_SIZE || physical > PS2_RAM_SIZE - size)
                return nullptr;
            return rdram + physical;
        }

        // Log the first few events of each kind, then say they are suppressed.
        void logEvent(const std::string &kind, const std::string &message)
        {
            uint32_t count = 0u;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                count = ++g_logCounts[kind];
            }
            if (count <= kMaxLogsPerKind)
                std::cerr << "[sif-cmd] " << message << std::endl;
            if (count == kMaxLogsPerKind)
                std::cerr << "[sif-cmd] further '" << kind << "' messages are suppressed" << std::endl;
        }

        std::string hex(uint32_t value)
        {
            std::ostringstream out;
            out << "0x" << std::hex << value;
            return out.str();
        }

        void queueReply(Reply reply)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_replies.push_back(std::move(reply));
        }

        // An RPC_END reply, echoing the request's rec_id, pkt_addr and rpc_id.
        Reply rpcEnd(const uint8_t *request, size_t size, uint32_t client, uint32_t requestCid,
                     uint32_t server, uint32_t buffer)
        {
            Reply reply;
            reply.packet = makePacket(kCmdRpcEnd, kRpcEndPacketSize);
            putWord(reply.packet, 4, word(request, size, 4));
            putWord(reply.packet, 5, word(request, size, 5));
            putWord(reply.packet, 6, word(request, size, 6));
            putWord(reply.packet, 7, client);
            putWord(reply.packet, 8, requestCid);
            putWord(reply.packet, 9, server);
            putWord(reply.packet, 10, buffer);
            return reply;
        }

        void handleBind(PS2Runtime *runtime, const uint8_t *packet, size_t size)
        {
            const uint32_t client = word(packet, size, 7);
            const uint32_t sid = word(packet, size, 8);
            BoundServer bound{};
            if (PS2IopTransport::canBindRpc(runtime, sid))
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                auto it = std::find_if(g_servers.begin(), g_servers.end(),
                                       [sid](const BoundServer &server)
                                       { return server.sid == sid; });
                if (it != g_servers.end())
                {
                    bound = *it;
                }
                else if (runtime)
                {
                    bound.sid = sid;
                    bound.serverToken = runtime->allocateIopMemory(64u, 16u);
                    bound.buffer = runtime->allocateIopMemory(kServerBufferSize, 64u);
                    if (bound.serverToken != 0u && bound.buffer != 0u)
                        g_servers.push_back(bound);
                    else
                        bound = BoundServer{};
                }
            }
            if (bound.serverToken == 0u)
            {
                // cd->server stays NULL, which tells the EE to retry the bind.
                logEvent("bind-" + hex(sid), "RPC bind to SID " + hex(sid) + ": no server");
            }
            queueReply(rpcEnd(packet, size, client, kCmdRpcBind, bound.serverToken, bound.buffer));
        }

        void handleCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                        const uint8_t *packet, size_t size)
        {
            const uint32_t client = word(packet, size, 7);
            const uint32_t function = word(packet, size, 8);
            uint32_t sendSize = word(packet, size, 9);
            const uint32_t receiveBuffer = word(packet, size, 10);
            const uint32_t receiveSize = word(packet, size, 11);
            const uint32_t replyMode = word(packet, size, 12);
            const uint32_t serverToken = word(packet, size, 13);

            BoundServer bound{};
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                for (const BoundServer &server : g_servers)
                {
                    if (server.serverToken == serverToken)
                        bound = server;
                }
            }
            if (bound.serverToken == 0u || !runtime)
            {
                logEvent("call-unbound", "RPC call " + hex(function) + " to unknown server " +
                                             hex(serverToken) + "; replying without running it");
                if (replyMode != 0u)
                    queueReply(rpcEnd(packet, size, client, kCmdRpcCall, serverToken, 0u));
                return;
            }
            if (sendSize > kServerBufferSize)
            {
                logEvent("call-oversize", "RPC call to SID " + hex(bound.sid) + " sends " +
                                              std::to_string(sendSize) + " bytes; only " +
                                              std::to_string(kServerBufferSize) + " are kept");
                sendSize = kServerBufferSize;
            }

            // The call data arrived in the server's IOP buffer just before this
            // packet. It is handed to the RPC handlers as staged data, never
            // written into EE memory, which belongs to the guest.
            std::vector<uint8_t> sendData(sendSize);
            if (sendSize != 0u && !runtime->readIopMemory(bound.buffer, sendData.data(), sendData.size()))
                sendData.assign(sendSize, 0u);

            ps2x::iop::RpcRequest request{};
            request.clientAddress = client;
            request.sid = bound.sid;
            request.function = function;
            request.receive = {receiveBuffer, receiveSize};
            const ps2x::iop::RpcResult result =
                PS2IopTransport::handleRpcWithSendData(runtime, rdram, ctx, request, sendData);

            if (!result.handled)
            {
                logEvent("call-unhandled-" + hex(bound.sid),
                         "RPC call " + hex(function) + " to SID " + hex(bound.sid) + " was not handled");
            }
            if (result.guestFunction != 0u)
            {
                logEvent("call-guest-function", "RPC call to SID " + hex(bound.sid) +
                                                    " asked for an EE-side server function; not supported here");
            }
            // rmode 0 (no wait, no end function) means the IOP sends no RPC_END.
            if (replyMode != 0u)
                queueReply(rpcEnd(packet, size, client, kCmdRpcCall, serverToken, bound.buffer));
        }

        void handleOtherData(uint8_t *rdram, PS2Runtime *runtime, const uint8_t *packet, size_t size)
        {
            const uint32_t receiveData = word(packet, size, 7);
            const uint32_t source = word(packet, size, 8);
            const uint32_t dest = word(packet, size, 9);
            const uint32_t length = word(packet, size, 10);
            std::vector<uint8_t> data(length);
            uint8_t *target = eeRam(rdram, dest, length);
            if (!runtime || !target || !runtime->readIopMemory(source, data.data(), data.size()))
            {
                logEvent("rdata-failed", "RPC other-data copy of " + std::to_string(length) +
                                             " bytes from IOP " + hex(source) + " to EE " + hex(dest) + " failed");
            }
            else
            {
                std::memcpy(target, data.data(), data.size());
            }
            queueReply(rpcEnd(packet, size, receiveData, kCmdRpcRdata, 0u, 0u));
        }
    }

    void reset()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_iopCommandBuffer = 0u;
        g_eePacketBuffer = 0u;
        g_replies.clear();
        g_servers.clear();
        g_logCounts.clear();
    }

    uint32_t iopCommandBuffer(PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_iopCommandBuffer == 0u && runtime)
            g_iopCommandBuffer = runtime->allocateIopMemory(kCommandBufferSize, 64u);
        return g_iopCommandBuffer;
    }

    bool isIopCommandBuffer(uint32_t iopAddress)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_iopCommandBuffer != 0u &&
               (iopAddress & 0x1FFFFFFFu) == (g_iopCommandBuffer & 0x1FFFFFFFu);
    }

    void onCommandPacket(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                         const uint8_t *packet, size_t size)
    {
        if (!packet || size < 16u)
        {
            logEvent("short-packet", "command packet of " + std::to_string(size) + " bytes ignored");
            return;
        }

        const uint32_t cid = word(packet, size, 2);
        switch (cid)
        {
        case kCmdChangeSaddr:
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_eePacketBuffer = word(packet, size, 4);
            break;
        }
        case kCmdInitCmd:
            if (word(packet, size, 3) == 0u)
            {
                // SIFCMD initialisation: the packet carries the EE packet buffer.
                std::lock_guard<std::mutex> lock(g_mutex);
                g_eePacketBuffer = word(packet, size, 4);
            }
            else
            {
                // SIFRPC initialisation: the IOP answers by setting SREG 0.
                Reply reply;
                reply.packet = makePacket(kCmdSetSreg, kSetSregPacketSize);
                putWord(reply.packet, 4, kSregRpcInit);
                putWord(reply.packet, 5, 1u);
                queueReply(std::move(reply));
            }
            break;
        case kCmdSetSreg:
            break; // an IOP-side software register; nothing on the IOP reads it here
        case kCmdResetCmd:
            logEvent("reset", "IOP reset request ignored");
            break;
        case kCmdRpcBind:
            handleBind(runtime, packet, size);
            break;
        case kCmdRpcCall:
            handleCall(rdram, ctx, runtime, packet, size);
            break;
        case kCmdRpcRdata:
            handleOtherData(rdram, runtime, packet, size);
            break;
        default:
            logEvent("unhandled-" + hex(cid), "command " + hex(cid) + " has no IOP handler; ignored");
            break;
        }

        deliverPending(rdram, runtime);
    }

    void deliverPending(uint8_t *rdram, PS2Runtime *runtime)
    {
        Reply reply;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_replies.empty() || g_eePacketBuffer == 0u || !runtime)
                return;
            uint8_t *packetBuffer = eeRam(rdram, g_eePacketBuffer, kCommandBufferSize);
            if (!packetBuffer || packetBuffer[0] != 0u)
                return; // the EE has not consumed the previous packet yet (psize != 0)
            reply = std::move(g_replies.front());
            g_replies.pop_front();
            std::memcpy(packetBuffer, reply.packet.data(), reply.packet.size());
        }

        // The packet buffer is the only flow control: a reply is written only once
        // the EE handler has cleared psize, so an extra interrupt finds it empty.
        const bool queued = runtime->eeScheduler().dispatchIrq(true, kDmacSif0, [rdram, runtime]()
                                                               { deliverPending(rdram, runtime); });
        if (!queued)
        {
            logEvent("no-sif0-handler", "reply " + hex(word(reply.packet.data(), reply.packet.size(), 2)) +
                                            " is waiting in the EE packet buffer: no SIF0 interrupt handler is enabled");
        }
    }
}
