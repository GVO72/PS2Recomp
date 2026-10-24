#include "iop_compat_test_support.h"

#include <limits>

namespace
{
    using namespace iop_test;
    constexpr uint32_t dbcSid = 0x80001300u;
    constexpr uint32_t dbcVersion = 0x80001363u;
    constexpr uint32_t mcSid = 0x80000400u;
    constexpr uint32_t fileioSid = 0x80000001u;

    // A FILEIO call whose argument block is at 0x100 in guest RAM.
    RpcRequest fileioCall(Host &host, uint32_t function, std::span<const uint8_t> arg)
    {
        std::memcpy(host.guest.data() + 0x100u, arg.data(), arg.size());
        RpcRequest call = request(fileioSid, function, 4u);
        call.send = {0x100u, static_cast<uint32_t>(arg.size())};
        return call;
    }

    std::vector<uint8_t> fileioWriteArg(int32_t fd, uint32_t ptr, uint32_t size, const std::string &head)
    {
        std::vector<uint8_t> arg(32u, 0u);
        const uint32_t mis = static_cast<uint32_t>(head.size());
        std::memcpy(arg.data(), &fd, 4u);
        std::memcpy(arg.data() + 4u, &ptr, 4u);
        std::memcpy(arg.data() + 8u, &size, 4u);
        std::memcpy(arg.data() + 12u, &mis, 4u);
        std::memcpy(arg.data() + 16u, head.data(), head.size());
        return arg;
    }

    std::vector<uint8_t> fileioOpenArg(const std::string &name)
    {
        std::vector<uint8_t> arg(4u + 256u, 0u);
        std::memcpy(arg.data() + 4u, name.data(), name.size());
        return arg;
    }

    std::vector<uint8_t> fileioFdArg(int32_t fd)
    {
        std::vector<uint8_t> arg(16u, 0u);
        std::memcpy(arg.data(), &fd, 4u);
        return arg;
    }

    void fileioConsoleWrite()
    {
        Host host;
        IopSubsystem iop(host);
        require(iop.canBindRpc(fileioSid), "FILEIO must be bindable without loading a module");

        // An unaligned buffer: the head up to 16-byte alignment travels in the
        // argument, the rest is read from EE memory.
        const std::string text = "hello, world!\n";
        std::memcpy(host.guest.data() + 0x205u, text.data(), text.size());
        host.fill(0x205u, 11u, 'x'); // the inline head must win over EE memory
        const auto arg = fileioWriteArg(1, 0x205u, static_cast<uint32_t>(text.size()), text.substr(0, 11));
        require(iop.handleRpc(fileioCall(host, 3u, arg)).handled, "write not handled");
        require(host.console == text, "stdout write did not reach the console");
        require(host.word(0x800u) == text.size(), "write must return the byte count");
    }

    void fileioTtyAndErrors()
    {
        Host host;
        IopSubsystem iop(host);

        require(iop.handleRpc(fileioCall(host, 0u, fileioOpenArg("tty0:"))).handled, "open not handled");
        const uint32_t fd = host.word(0x800u);
        require(fd == 3u, "the first tty descriptor should be 3");
        const std::string text = "tty\n";
        std::memcpy(host.guest.data() + 0x300u, text.data(), text.size());
        require(iop.handleRpc(fileioCall(host, 3u, fileioWriteArg(3, 0x300u, 4u, ""))).handled, "write failed");
        require(host.console == text, "a tty write should reach the console");
        require(iop.handleRpc(fileioCall(host, 1u, fileioFdArg(3))).handled && host.word(0x800u) == 0u,
                "closing the tty should succeed");

        const size_t logs = host.logs.size();
        require(iop.handleRpc(fileioCall(host, 3u, fileioWriteArg(3, 0x300u, 4u, ""))).handled, "write failed");
        require(static_cast<int32_t>(host.word(0x800u)) == -9, "a closed descriptor is EBADF");
        require(iop.handleRpc(fileioCall(host, 0u, fileioOpenArg("cdrom0:\\X.BIN;1"))).handled, "open failed");
        require(static_cast<int32_t>(host.word(0x800u)) == -2, "non-tty opens are not implemented");
        require(iop.handleRpc(fileioCall(host, 4u, fileioFdArg(1))).handled, "lseek not handled");
        require(static_cast<int32_t>(host.word(0x800u)) == -5, "unimplemented functions fail");
        require(host.logs.size() == logs + 3u, "each unsupported request must be logged");
        require(host.console == text, "failed requests must not print anything");
    }

    void dbcDefault()
    {
        Host host;
        IopSubsystem iop(host);
        require(!iop.canBindRpc(dbcSid), "unloaded DBCMAN must stay dormant");
        require(iop.loadModule("rom0:DBCMAN").moduleId > 0, "DBCMAN load failed");
        auto query = request(dbcSid, dbcVersion);
        require(iop.handleRpc(query).handled, "version RPC not handled");
        for (uint32_t i = 0u; i < 4u; ++i)
            require(host.word(0x800u + i * 4u) == 0x0310u, "DBCMAN target version changed");
    }



    void dbcResetAndReconfigure()
    {
        Host host;
        IopSubsystem iop(host);
        require(iop.loadModule("rom0:DBCMAN").moduleId > 0, "load failed");
        require(iop.handleRpc(request(dbcSid, dbcVersion)).handled, "RPC failed");
        require(host.word(0x800u) == 0x0310u, "unexpected DBCMAN version");
        iop.reset();
        require(!iop.canBindRpc(dbcSid), "reset retained a module route");
        require(metric(iop, "dbcman", "version_queries") == 0u, "query counter not reset");
        require(iop.loadModule("rom0:DBCMAN").moduleId > 0, "reload failed");
        require(iop.handleRpc(request(dbcSid, dbcVersion)).handled, "RPC failed");
        require(host.word(0x800u) == 0x0310u, "IOP reboot changed target version");
    }


    void dbcReplyBounds()
    {
        Host host;
        IopSubsystem iop(host);
        require(iop.loadModule("rom0:DBCMAN").moduleId > 0, "load failed");
        for (uint32_t size = 0u; size <= 24u; ++size)
        {
            host.fill(0x7FCu, 40u);
            require(iop.handleRpc(request(dbcSid, dbcVersion, size)).handled, "RPC failed");
            const uint32_t written = std::min(size / 4u, 4u) * 4u;
            for (uint32_t offset = written; offset < 32u; ++offset)
                require(host.guest[0x800u + offset] == 0xCCu, "reply wrote past whole-word payload");
            require(host.word(0x7FCu) == 0xCCCCCCCCu, "reply underflow");
        }
        auto query = request(dbcSid, dbcVersion);
        query.receive.address = 0u;
        const size_t writes = host.guestWrites;
        require(iop.handleRpc(query).handled && host.guestWrites == writes, "null reply was written");
    }

    void dbcNoAddressWrap()
    {
        Host host;
        IopSubsystem iop(host);
        require(iop.loadModule("rom0:DBCMAN").moduleId > 0, "load failed");
        auto query = request(dbcSid, dbcVersion);
        query.receive.address = 0xFFFFFFF8u;
        require(iop.handleRpc(query).handled, "RPC failed");
        require(host.word(0u) == 0xCCCCCCCCu && host.word(4u) == 0xCCCCCCCCu,
                "overflowed reply corrupted low guest addresses");
        require(metric(iop, "dbcman", "failed_version_replies") == 1u, "invalid reply not recorded");
    }

    void dbcNoRequestGuessing()
    {
        Host host;
        IopSubsystem iop(host);
        require(iop.loadModule("rom0:DBCMAN").moduleId > 0, "load failed");
        auto query = request(dbcSid, dbcVersion);
        const std::array<uint32_t, 4> randomArguments{0x0310u, 0x00010000u, 0u, 0xFFFFu};
        require(host.writeGuest(0x600u, randomArguments.data(), sizeof(randomArguments)), "write failed");
        query.send = {0x600u, sizeof(randomArguments)};
        require(iop.handleRpc(query).handled, "RPC failed");
        require(host.word(0x800u) == 0x0310u, "send buffer was guessed to be a requested version");
    }

    void dbcPhysicalServerWins()
    {
        Host host;
        IopSubsystem iop(host);
        require(iop.loadModule("rom0:DBCMAN").moduleId > 0, "HLE load failed");
        auto image = rpcServer(dbcSid, 0xDEADBEEFu);
        image.install(host);
        auto physical = iop.loadModuleBuffer(0x1000u);
        require(physical.moduleId > 0 && physical.startResult == 0, "physical IRX failed");
        require(iop.handleRpc(request(dbcSid, dbcVersion)).handled, "physical RPC not handled");
        require(host.word(0x800u) == 0xDEADBEEFu, "HLE overwrote physical server version");
        require(metric(iop, "dbcman", "version_queries") == 0u, "HLE ran after physical service");
        require(iop.stopModule(physical.moduleId), "physical stop failed");
        require(iop.handleRpc(request(dbcSid, dbcVersion)).handled, "HLE fallback not restored");
        require(host.word(0x800u) == 0x0310u, "wrong HLE version after physical stop");
    }

    void mcNewInit()
    {
        Host host;
        IopSubsystem iop(host);
        require(iop.loadModule("rom0:XMCSERV").moduleId > 0, "XMCSERV load failed");
        require(iop.handleRpc(request(mcSid, 0xFEu, 16u)).handled, "init RPC failed");
        require(host.word(0x800u) == 0u && host.word(0x804u) == 0x0205u && host.word(0x808u) == 0x0206u,
                "new memory-card init layout changed");
        require(host.word(0x80Cu) == 0xCCCCCCCCu, "new init wrote beyond 12-byte response");
    }

    void mcOldInit()
    {
        Host host;
        IopSubsystem iop(host);
        require(iop.loadModule("rom0:MCSERV").moduleId > 0, "MCSERV load failed");
        require(iop.handleRpc(request(mcSid, 0x70u, 16u)).handled, "init RPC failed");
        require(host.word(0x800u) == 0u && host.word(0x804u) == 0xCCCCCCCCu,
                "old init leaked extended protocol versions");
    }

    void mcInitFailure()
    {
        Host host;
        IopSubsystem iop(host);
        require(iop.loadModule("rom0:MCSERV").moduleId > 0, "MCSERV load failed");
        host.initResult = -5;
        for (uint32_t operation : {0x70u, 0xFEu})
        {
            require(iop.handleRpc(request(mcSid, operation)).handled, "init RPC failed");
            require(static_cast<int32_t>(host.word(0x800u)) == -5, "init failure reported as success");
        }
    }

    void mcReplyBounds()
    {
        Host host;
        IopSubsystem iop(host);
        require(iop.loadModule("rom0:MCSERV").moduleId > 0, "MCSERV load failed");
        for (uint32_t operation : {0x70u, 0xFEu})
            for (uint32_t size = 0u; size <= 20u; ++size)
            {
                host.fill(0x7FCu, 32u);
                require(iop.handleRpc(request(mcSid, operation, size)).handled, "init RPC failed");
                const uint32_t words = operation == 0xFEu ? 3u : 1u;
                const uint32_t written = std::min(size / 4u, words) * 4u;
                for (uint32_t offset = written; offset < 24u; ++offset)
                    require(host.guest[0x800u + offset] == 0xCCu, "init clobbered response tail");
                require(host.word(0x7FCu) == 0xCCCCCCCCu, "init underflowed buffer");
            }
        auto query = request(mcSid, 0xFEu);
        query.receive.address = 0xFFFFFFF8u;
        require(iop.handleRpc(query).handled, "RPC failed");
        require(host.word(0u) == 0xCCCCCCCCu, "init overflowed guest address");
    }

    void mcShortNamePacket()
    {
        Host host;
        IopSubsystem iop(host);
        require(iop.loadModule("rom0:MCSERV").moduleId > 0, "MCSERV load failed");
        auto query = request(mcSid, 0x02u, 4u);
        query.send = {0x1000u, 20u};
        host.fill(0x1000u, 1044u, 0u);
        const size_t calls = host.cardCalls.size();
        require(iop.handleRpc(query).handled, "RPC failed");
        require(host.cardCalls.size() == calls, "short packet read a filename beyond send.size");
        require(static_cast<int32_t>(host.word(0x800u)) == -5, "short packet not rejected");
    }

    void mcFullNamePacket()
    {
        Host host;
        IopSubsystem iop(host);
        require(iop.loadModule("rom0:MCSERV").moduleId > 0, "MCSERV load failed");
        const std::array<uint32_t, 5> header{1u, 0u, 1u, 0u, 0u};
        host.fill(0x1000u, 1044u, 0u);
        require(host.writeGuest(0x1000u, header.data(), sizeof(header)), "packet header write failed");
        constexpr char name[] = "/save.dat";
        require(host.writeGuest(0x1014u, name, sizeof(name)), "packet filename write failed");
        for (uint32_t operation : {0x02u, 0x71u})
        {
            auto query = request(mcSid, operation, 4u);
            query.send = {0x1000u, 1044u};
            const size_t before = host.cardCalls.size();
            require(iop.handleRpc(query).handled, "open RPC failed");
            require(host.cardCalls.size() == before + 1u, "valid packet not dispatched");
            const auto &call = host.cardCalls.back();
            require(call.operation == MemoryCardOperation::Open &&
                    call.arguments[0] == 1u && call.arguments[1] == 0u &&
                    call.arguments[2] == 0x1014u && call.arguments[3] == 1u,
                    "valid name packet decoded incorrectly");
        }
    }

    void mcStatusBounds()
    {
        Host host;
        IopSubsystem iop(host);
        require(iop.loadModule("rom0:MCSERV").moduleId > 0, "MCSERV load failed");
        for (uint32_t size = 0u; size <= 20u; ++size)
        {
            host.fill(0x7FCu, 32u);
            require(iop.handleRpc(request(mcSid, 0xFFFFFFFFu, size)).handled, "RPC failed");
            const uint32_t written = size >= 4u ? 4u : 0u;
            if (written != 0u)
                require(static_cast<int32_t>(host.word(0x800u)) == -5, "missing error status");
            for (uint32_t offset = written; offset < 24u; ++offset)
                require(host.guest[0x800u + offset] == 0xCCu, "status clobbered receive tail");
            require(host.word(0x7FCu) == 0xCCCCCCCCu, "status underflowed receive buffer");
        }
        auto query = request(mcSid, 0xFFFFFFFFu, 16u);
        query.receive.address = 0xFFFFFFFCu;
        require(iop.handleRpc(query).handled, "RPC failed");
        require(host.word(0u) == 0xCCCCCCCCu && host.word(4u) == 0xCCCCCCCCu,
                "status reply wrapped and zeroed low guest memory");
    }

    void moduleAliases()
    {
        Host host;
        IopSubsystem iop(host);
        for (const char *path : {"rom0:XSIO2MAN", "rom0:XPADMAN", "rom0:XMCMAN"})
        {
            auto result = iop.loadModule(path);
            require(result.moduleId > 0 && result.startResult == 0, "known extended module rejected");
        }
        require(!iop.canBindRpc(mcSid), "XMCMAN alone enabled a memory-card RPC server");
        const auto module = iop.loadModule("CDROM0:\\IOP\\xMcSeRv.IrX;1");
        require(module.moduleId > 0 && iop.canBindRpc(mcSid), "normalized XMCSERV alias not activated");
        require(iop.stopModule(module.moduleId) && !iop.canBindRpc(mcSid), "stopped alias remained active");
    }

    void unknownModulesStayUnknown()
    {
        Host host;
        IopSubsystem iop(host);
        for (const char *name : {"MC2_D.IRX", "DS2U_D.IRX", "CDVDSTM.IRX", "SDRDRV.IRX", "EZPCM.IRX", "ANYTHING_D.IRX"})
        {
            const auto result = iop.loadModule(std::string("host0:IOPModules/") + name);
            require(result.moduleId < 0 && result.startResult < 0, "unsupported module got a fake success");
        }
        require(!iop.canBindRpc(0x19740512u), "game-specific SDRDRV activated globally");
    }

    void moduleLifetime()
    {
        Host host;
        IopSubsystem iop(host);
        const auto a = iop.loadModule("rom0:DBCMAN");
        const auto b = iop.loadModule("rom0:dbcman.irx");
        const auto alias = iop.loadModule("rom0:DBCM");
        require(a.moduleId > 0 && a.moduleId == b.moduleId && alias.moduleId > 0, "module IDs unstable");
        require(iop.stopModule(a.moduleId) && iop.canBindRpc(dbcSid), "first release removed shared route");
        require(iop.stopModule(b.moduleId) && iop.canBindRpc(dbcSid), "remaining alias not honored");
        require(iop.stopModule(alias.moduleId) && !iop.canBindRpc(dbcSid), "last release retained route");
    }

    void loaderDiagnostics()
    {
        Host host;
        IopSubsystem iop(host);
        require(iop.loadModule("host0:LIBSD.IRX").moduleId > 0, "LIBSD fallback failed");
        require(iop.loadModule("host0:MISSING.IRX").moduleId < 0, "unknown load accepted");
        for (unsigned i = 0u; i < 100u; ++i)
            (void)iop.loadModule("host0:MISSING.IRX");
        auto snapshot = iop.debugSnapshot();
        require(snapshot.diagnostics.size() == 2u, "final loader outcomes not deduplicated");
        require(snapshot.diagnostics[0].find("[IOP:HLE]") != std::string::npos, "no fallback diagnostic");
        require(snapshot.diagnostics[1].find("no HLE provider") != std::string::npos, "no final failure diagnostic");
        for (unsigned i = 0u; i < 100u; ++i)
            (void)iop.loadModule("rom0:missing" + std::to_string(i));
        require(iop.debugSnapshot().diagnostics.size() <= 32u, "unbounded module diagnostics");
        iop.reset();
        require(iop.debugSnapshot().diagnostics.empty(), "stale load outcomes survived reset");
    }

    void libsdUnchanged()
    {
        Host host;
        IopSubsystem iop(host);
        require(iop.loadModule("rom0:LIBSD").moduleId > 0, "LIBSD load failed");
        require(iop.handleRpc(request(0x80000701u, 0x8010u)).handled, "LIBSD RPC not handled");
        require(host.audioCalls == 1u, "DBCMAN option intercepted LIBSD RPC");
    }
}

int main()
{
    const Test tests[] = {
        {"DBCMAN default and dormant route", dbcDefault},
        {"DBCMAN reboot and reconfiguration", dbcResetAndReconfigure},
        {"DBCMAN bounded whole-word response", dbcReplyBounds},
        {"DBCMAN rejects wrapping reply addresses", dbcNoAddressWrap},
        {"DBCMAN does not infer version from arbitrary RPC payload", dbcNoRequestGuessing},
        {"Physical DBCMAN server wins over configured HLE", dbcPhysicalServerWins},
        {"XMCSERV init status and two version fields", mcNewInit},
        {"Old MCSERV init is status only", mcOldInit},
        {"MCSERV propagates initialization failure", mcInitFailure},
        {"MCSERV response bounds for both dialects", mcReplyBounds},
        {"MCSERV rejects truncated name packet", mcShortNamePacket},
        {"MCSERV accepts complete name packets in both dialects", mcFullNamePacket},
        {"MCSERV status replies preserve bounds and cannot wrap", mcStatusBounds},
        {"Extended module aliases and activation", moduleAliases},
        {"Unsupported debug and game IRX stay unsupported", unknownModulesStayUnknown},
        {"HLE repeated loads and alias lifetime", moduleLifetime},
        {"Loader outcomes are bounded and resettable", loaderDiagnostics},
        {"LIBSD audio dispatch is unchanged", libsdUnchanged},
        {"FILEIO writes stdout to the console", fileioConsoleWrite},
        {"FILEIO tty descriptors and logged errors", fileioTtyAndErrors},
    };
    return run(tests);
}
