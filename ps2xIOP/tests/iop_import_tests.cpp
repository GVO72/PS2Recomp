#include "emulator/core/iop_cpu.h"
#include "emulator/core/iop_kernel.h"
#include "emulator/core/iop_memory.h"
#include "emulator/imports/iop_cdvd.h"
#include "emulator/imports/iop_imports.h"
#include "emulator/imports/iop_loadcore.h"
#include "emulator/imports/iop_timrman.h"
#include "emulator/services/iop_rpc.h"
#include "ps2x/iop/iop_host.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <chrono>
#include <cstring>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using namespace ps2x::iop;
    using namespace ps2x::iop::detail;

    constexpr uint32_t kExportMagic = 0x41C00000u;
    constexpr int32_t kLibraryNotFound = -213;
    constexpr int32_t kIllegalLibrary = -214;

    class NullHost : public IopHost
    {
    public:
        bool readGuest(uint32_t, void *, size_t) const override { return false; }
        bool writeGuest(uint32_t, const void *, size_t) override { return false; }
        bool zeroGuest(uint32_t, size_t) override { return false; }
        bool normalizeGuestAddress(uint32_t, uint32_t &) const override { return false; }
        uint32_t allocateIopHandle(IopHandleKind) override { return 1u; }
        uint32_t allocateGuest(uint32_t, uint32_t) override { return 0u; }
        void freeGuest(uint32_t) override {}
        void audioCommand(uint32_t, uint32_t, GuestBuffer, GuestBuffer) override {}
        std::string hostPath(HostPathKind) const override { return {}; }
        std::string translateGuestPath(std::string_view path) const override { return std::string(path); }
        uint64_t openHostFile(std::string_view) override { return 0u; }
        bool hostFileSize(uint64_t, uint64_t &) const override { return false; }
        bool readHostFile(uint64_t, uint64_t, void *, size_t, size_t &) override { return false; }
        void closeHostFile(uint64_t) override {}
        int32_t memoryCard(const MemoryCardRequest &) override { return 0; }
        bool hasGuestFunction(uint32_t) const override { return false; }
        bool invokeGuestFunction(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *) override { return false; }
        void log(LogLevel, std::string_view) override {}
    };

    class CdRootHost final : public NullHost
    {
    public:
        explicit CdRootHost(std::filesystem::path rootPath)
            : root(std::move(rootPath))
        {
        }

        std::string hostPath(HostPathKind kind) const override
        {
            return kind == HostPathKind::CdRoot ? root.string() : std::string{};
        }

    private:
        std::filesystem::path root;
    };

    class RecordingExecutor final : public IopGuestExecutor
    {
    public:
        uint32_t executeGuestFunction(uint32_t address,
                                      uint32_t a0,
                                      uint32_t,
                                      uint32_t,
                                      uint32_t,
                                      uint32_t gp) override
        {
            ++calls;
            lastAddress = address;
            lastArgument = a0;
            lastGp = gp;
            return callbackResult;
        }

        uint32_t callbackResult = 0u;
        uint32_t calls = 0u;
        uint32_t lastAddress = 0u;
        uint32_t lastArgument = 0u;
        uint32_t lastGp = 0u;
    };

    bool expect(bool condition, std::string_view message)
    {
        if (condition)
            return true;
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }

    bool testLoadcoreRebootLibraryMode()
    {
        IopMemory memory;
        IopImportRegistry imports(memory);
        IopLoadcore loadcore(memory, imports);

        IopCpuState cpu{};
        cpu.gpr[4] = 0u;
        cpu.gpr[5] = 2u;
        if (!expect(loadcore.dispatchImport(27u, cpu), "loadcore:27 was not handled") ||
            !expect(static_cast<int32_t>(cpu.gpr[2]) == kIllegalLibrary,
                    "loadcore:27 did not reject a null export table"))
            return false;

        constexpr uint32_t table = 0x1000u;
        memory.write32(table, kExportMagic);
        memory.write16(table + 8u, 0x0101u);
        memory.write16(table + 10u, 0x1234u);
        const char name[8] = {'t', 'e', 's', 't', 'l', 'i', 'b', '\0'};
        (void)memory.writeRam(table + 12u, name, sizeof(name));
        memory.write32(table + 20u, 0u);

        cpu = {};
        cpu.gpr[4] = table;
        cpu.gpr[5] = 2u;
        if (!expect(loadcore.dispatchImport(27u, cpu), "loadcore:27 rejected a valid export table") ||
            !expect(cpu.gpr[2] == 0u, "loadcore:27 returned an error for a valid export table") ||
            !expect(memory.read16(table + 10u) == 0x1232u,
                    "loadcore:27 did not replace only export mode bits 1 and 2"))
            return false;

        constexpr uint32_t invalidTable = 0x1100u;
        memory.write32(invalidTable, 0xDEADBEEFu);
        cpu = {};
        cpu.gpr[4] = invalidTable;
        cpu.gpr[5] = 6u;
        if (!expect(loadcore.dispatchImport(27u, cpu), "loadcore:27 did not consume an invalid-table call") ||
            !expect(static_cast<int32_t>(cpu.gpr[2]) == kLibraryNotFound,
                    "loadcore:27 returned the wrong invalid-table error"))
            return false;

        if (!expect(imports.registerExportTable(table), "test export table did not register"))
            return false;
        memory.write32(table, 0u);
        cpu = {};
        cpu.gpr[4] = table;
        cpu.gpr[5] = 6u;
        return expect(loadcore.dispatchImport(27u, cpu), "loadcore:27 rejected a registered table") &&
               expect(cpu.gpr[2] == 0u, "loadcore:27 returned an error for a registered table") &&
               expect(memory.read16(table + 10u) == 0x1236u,
                      "loadcore:27 did not update a registered table's mode");
    }

    bool pollEvent(IopKernel &kernel, int eventId, uint32_t bits, uint32_t resultAddress, int32_t expected)
    {
        IopCpuState cpu{};
        cpu.gpr[4] = static_cast<uint32_t>(eventId);
        cpu.gpr[5] = bits;
        cpu.gpr[6] = 0u; // WEF_AND
        cpu.gpr[7] = resultAddress;
        return expect(kernel.dispatchEventImport(11u, cpu), "PollEventFlag was not handled") &&
               expect(static_cast<int32_t>(cpu.gpr[2]) == expected, "PollEventFlag returned an unexpected result");
    }

    bool testCdvdSpecialControl()
    {
        NullHost host;
        IopMemory memory;
        IopKernel kernel(memory);
        kernel.reset();
        IopCdvd cdvd(host, memory, kernel);
        cdvd.reset();

        constexpr uint32_t param = 0x2000u;
        constexpr uint32_t eventResult = 0x2010u;
        IopCpuState cpu{};
        cpu.gpr[4] = static_cast<uint32_t>(-11); // sceCdSC: return cdvdman interrupt event flag
        cpu.gpr[5] = param;
        if (!expect(cdvd.dispatchImport(50u, cpu), "cdvdman:50 was not handled") ||
            !expect(static_cast<int32_t>(cpu.gpr[2]) > 0, "sceCdSC(-11) did not return a valid event flag"))
            return false;
        const int eventId = static_cast<int>(cpu.gpr[2]);

        if (!pollEvent(kernel, eventId, 0x29u, eventResult, 0) ||
            !expect(memory.read32(eventResult) == 0x29u, "cdvdman event flag did not start with bits 0x29"))
            return false;

        IopCpuState clear{};
        clear.gpr[4] = static_cast<uint32_t>(eventId);
        clear.gpr[5] = ~0x29u;
        if (!expect(kernel.dispatchEventImport(8u, clear), "ClearEventFlag was not handled") ||
            !pollEvent(kernel, eventId, 0x29u, eventResult, -418))
            return false;

        cpu = {};
        cpu.gpr[4] = 0x12345u;
        if (!expect(cdvd.dispatchImport(7u, cpu), "sceCdSeek was not handled") ||
            !pollEvent(kernel, eventId, 0x29u, eventResult, 0))
            return false;

        memory.write8(param, 0x30u);
        cpu = {};
        cpu.gpr[4] = static_cast<uint32_t>(-2);
        cpu.gpr[5] = param;
        if (!expect(cdvd.dispatchImport(50u, cpu), "sceCdSC(-2) was not handled") ||
            !expect(cpu.gpr[2] == 0x30u, "sceCdSC(-2) did not store the low-byte error"))
            return false;

        memory.write32(param, 0u);
        cpu = {};
        cpu.gpr[4] = static_cast<uint32_t>(-1);
        cpu.gpr[5] = param;
        if (!expect(cdvd.dispatchImport(50u, cpu), "sceCdSC(-1) was not handled") ||
            !expect(cpu.gpr[2] == 0u, "sceCdSC(-1) returned the wrong initial stream state") ||
            !expect(memory.read32(param) == 0x30u, "sceCdSC(-1) did not publish the last error"))
            return false;

        cpu = {};
        cpu.gpr[4] = 2u;
        cpu.gpr[5] = param;
        if (!expect(cdvd.dispatchImport(50u, cpu), "sceCdSC(2) was not handled") ||
            !expect(cpu.gpr[2] == 2u, "sceCdSC(2) did not update the stream state"))
            return false;

        cpu = {};
        cpu.gpr[4] = static_cast<uint32_t>(-1);
        cpu.gpr[5] = param;
        return expect(cdvd.dispatchImport(50u, cpu), "second sceCdSC(-1) was not handled") &&
               expect(cpu.gpr[2] == 2u, "sceCdSC(-1) did not preserve the stream state");
    }

    bool testCdvdSearchFile()
    {
        const auto suffix = std::to_string(
            static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path root =
            std::filesystem::temp_directory_path() / ("ps2x-iop-cdvd-search-" + suffix);
        const std::filesystem::path movieDirectory = root / "MOVIE";
        const std::filesystem::path moviePath = movieDirectory / "OPENING.PSS";
        std::error_code error;
        std::filesystem::create_directories(movieDirectory, error);
        if (!expect(!error, "could not create the temporary CD root"))
            return false;
        {
            std::ofstream movie(moviePath, std::ios::binary);
            movie.write("PSS!", 4);
        }

        CdRootHost host(root);
        IopMemory memory;
        IopKernel kernel(memory);
        kernel.reset();
        IopCdvd cdvd(host, memory, kernel);
        cdvd.reset();

        constexpr uint32_t resultAddress = 0x2400u;
        constexpr uint32_t pathAddress = 0x2480u;
        const char path[] = "cdrom0:\\movie\\opening.pss;1";
        (void)memory.writeRam(pathAddress, path, sizeof(path));

        IopCpuState cpu{};
        cpu.gpr[4] = resultAddress;
        cpu.gpr[5] = pathAddress;
        const bool handled = cdvd.dispatchImport(10u, cpu);
        const bool passed =
            expect(handled, "cdvdman:10 was not handled") &&
            expect(cpu.gpr[2] == 1u, "sceCdSearchFile did not find a case-insensitive ISO path") &&
            expect(memory.read32(resultAddress) >= 20u, "sceCdSearchFile returned an invalid LSN") &&
            expect(memory.read32(resultAddress + 4u) == 4u, "sceCdSearchFile returned the wrong size") &&
            expect(memory.readString(resultAddress + 8u, 16u) == "OPENING.PSS",
                   "sceCdSearchFile returned the wrong file name");

        std::filesystem::remove_all(root, error);
        return passed;
    }

    // In-memory ISO9660 image with hand-written descriptors and directory records.
    class ImageHost final : public NullHost
    {
    public:
        std::string hostPath(HostPathKind kind) const override
        {
            return kind == HostPathKind::CdImage ? "synthetic.iso" : std::string{};
        }
        uint64_t openHostFile(std::string_view) override { return 1u; }
        bool readHostFile(uint64_t, uint64_t offset, void *destination, size_t size, size_t &bytesRead) override
        {
            bytesRead = 0u;
            if (offset > image.size())
                return false;
            bytesRead = std::min<size_t>(size, image.size() - static_cast<size_t>(offset));
            std::memcpy(destination, image.data() + offset, bytesRead);
            return true;
        }
        void log(LogLevel, std::string_view message) override { logs.emplace_back(message); }

        std::vector<uint8_t> image;
        std::vector<std::string> logs;
    };

    struct TestRecord
    {
        std::string name;
        uint32_t lsn;
        uint32_t size;
        bool directory;
    };

    void putBoth32(uint8_t *destination, uint32_t value)
    {
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            destination[i] = static_cast<uint8_t>(value >> (i * 8u));
            destination[4u + i] = static_cast<uint8_t>(value >> ((3u - i) * 8u));
        }
    }

    size_t putRecord(uint8_t *destination, const TestRecord &record)
    {
        const size_t unpadded = 33u + record.name.size();
        const size_t length = unpadded + (unpadded & 1u);
        std::memset(destination, 0, length);
        destination[0] = static_cast<uint8_t>(length);
        putBoth32(destination + 2u, record.lsn);
        putBoth32(destination + 10u, record.size);
        destination[18] = 99u; // 1999-12-31 23:59:58
        destination[19] = 12u;
        destination[20] = 31u;
        destination[21] = 23u;
        destination[22] = 59u;
        destination[23] = 58u;
        destination[25] = record.directory ? 2u : 0u;
        destination[32] = static_cast<uint8_t>(record.name.size());
        std::memcpy(destination + 33u, record.name.data(), record.name.size());
        return length;
    }

    void writeDirectory(std::vector<uint8_t> &image, uint32_t lsn, uint32_t sectors, const std::vector<TestRecord> &records)
    {
        uint8_t *base = image.data() + static_cast<size_t>(lsn) * 2048u;
        size_t offset = 0u;
        for (const TestRecord &record : records)
        {
            const size_t unpadded = 33u + record.name.size();
            const size_t length = unpadded + (unpadded & 1u);
            if (offset % 2048u + length > 2048u)
                offset = (offset / 2048u + 1u) * 2048u;
            offset += putRecord(base + offset, record);
        }
        (void)sectors;
    }

    // Layout: root at 20; /GAMES at 21..22 (two sectors, target spills into the second);
    // /GAMES/DATA at 23; files at 30 and 31.
    void buildImage(ImageHost &host)
    {
        host.image.assign(40u * 2048u, 0u);
        uint8_t *pvd = host.image.data() + 16u * 2048u;
        pvd[0] = 1u;
        std::memcpy(pvd + 1u, "CD001", 5u);
        pvd[6] = 1u;
        putRecord(pvd + 156u, TestRecord{std::string(1u, '\0'), 20u, 2048u, true});
        writeDirectory(host.image, 20u, 1u,
                       {{std::string(1u, '\0'), 20u, 2048u, true},
                        {std::string(1u, '\1'), 20u, 2048u, true},
                        {"GAMES", 21u, 4096u, true},
                        {"BOOT.ELF;1", 30u, 1234u, false}});
        std::vector<TestRecord> games = {{std::string(1u, '\0'), 21u, 4096u, true},
                                         {std::string(1u, '\1'), 20u, 2048u, true}};
        for (int i = 0; i < 60; ++i)
            games.push_back({"FILLER" + std::to_string(10 + i) + ".DAT;1", 31u, 8u, false});
        games.push_back({"DATA", 23u, 2048u, true});
        games.push_back({"LATE.CFG;1", 31u, 77u, false});
        writeDirectory(host.image, 21u, 2u, games);
        writeDirectory(host.image, 23u, 1u,
                       {{std::string(1u, '\0'), 23u, 2048u, true},
                        {std::string(1u, '\1'), 21u, 4096u, true},
                        {"CONFIG.BIN;1", 30u, 4242u, false}});
    }

    struct SearchResult
    {
        bool found = false;
        uint32_t lsn = 0u;
        uint32_t size = 0u;
        std::string name;
        std::array<uint8_t, 8u> date{};
    };

    SearchResult searchPath(IopCdvd &cdvd, IopMemory &memory, const std::string &path)
    {
        constexpr uint32_t resultAddress = 0x2400u;
        constexpr uint32_t pathAddress = 0x2480u;
        (void)memory.writeRam(pathAddress, path.c_str(), path.size() + 1u);
        IopCpuState cpu{};
        cpu.gpr[4] = resultAddress;
        cpu.gpr[5] = pathAddress;
        SearchResult result;
        if (!cdvd.dispatchImport(10u, cpu))
            return result;
        result.found = cpu.gpr[2] == 1u;
        if (result.found)
        {
            result.lsn = memory.read32(resultAddress);
            result.size = memory.read32(resultAddress + 4u);
            result.name = memory.readString(resultAddress + 8u, 16u);
            for (uint32_t i = 0u; i < 8u; ++i)
                result.date[i] = memory.read8(resultAddress + 24u + i);
        }
        return result;
    }

    size_t countNotFound(const ImageHost &host, std::string_view path)
    {
        return static_cast<size_t>(std::count(host.logs.begin(), host.logs.end(),
                                              "[cdvd] sceCdSearchFile: not found: " + std::string(path)));
    }

    bool testCdvdSearchFileInImage()
    {
        ImageHost host;
        buildImage(host);
        IopMemory memory;
        IopKernel kernel(memory);
        kernel.reset();
        IopCdvd cdvd(host, memory, kernel);
        cdvd.reset();

        for (const char *path : {"cdrom0:\\GAMES\\DATA\\CONFIG.BIN;1", "cdrom0:/games/data/config.bin",
                                 "cdrom:\\Games/Data\\Config.Bin;1", "\\games\\data\\config.bin;1",
                                 "CDROM0:///GAMES/DATA/CONFIG.BIN"})
        {
            const SearchResult r = searchPath(cdvd, memory, path);
            if (!expect(r.found, std::string("image lookup failed: ") + path) ||
                !expect(r.lsn == 30u && r.size == 4242u, "image lookup returned the wrong lsn or size") ||
                !expect(r.name == "CONFIG.BIN;1", "image lookup did not return the stored leaf name"))
                return false;
        }

        const SearchResult late = searchPath(cdvd, memory, "cdrom0:\\GAMES\\LATE.CFG");
        if (!expect(late.found && late.lsn == 31u && late.size == 77u,
                    "entry in the second sector of a directory was not found") ||
            !expect(late.date[0] == 0u && late.date[1] == 58u && late.date[2] == 59u && late.date[3] == 23u &&
                        late.date[4] == 31u && late.date[5] == 12u && late.date[6] == (1999u & 0xFFu) &&
                        late.date[7] == (1999u >> 8u),
                    "date bytes do not follow the directory record"))
            return false;

        const SearchResult dir = searchPath(cdvd, memory, "cdrom0:\\GAMES");
        if (!expect(dir.found && dir.lsn == 21u && dir.name == "GAMES" && dir.date[0] == 2u,
                    "directory lookup failed"))
            return false;
        const SearchResult root = searchPath(cdvd, memory, "cdrom0:BOOT.ELF;1");
        if (!expect(root.found && root.lsn == 30u && root.size == 1234u, "root file lookup failed"))
            return false;

        for (const char *path : {"cdrom0:\\GAMES\\MISSING.BIN", "cdrom0:\\NOPE\\CONFIG.BIN",
                                 "cdrom0:\\BOOT.ELF\\X", "cdrom0:\\GAMES\\..\\BOOT.ELF"})
        {
            if (!expect(!searchPath(cdvd, memory, path).found, std::string("missing path was found: ") + path))
                return false;
            (void)searchPath(cdvd, memory, path);
            if (!expect(countNotFound(host, path) == 1u, std::string("not-found was not logged exactly once: ") + path))
                return false;
        }
        return true;
    }

    bool testCdvdSearchFileMalformedImage()
    {
        ImageHost host;
        buildImage(host);
        host.image[static_cast<size_t>(21u) * 2048u + 100u] = 0xFFu; // bogus record length in /GAMES
        IopMemory memory;
        IopKernel kernel(memory);
        kernel.reset();
        IopCdvd cdvd(host, memory, kernel);
        cdvd.reset();
        if (!expect(!searchPath(cdvd, memory, "cdrom0:\\GAMES\\DATA\\CONFIG.BIN").found,
                    "malformed directory was not refused") ||
            !expect(searchPath(cdvd, memory, "cdrom0:BOOT.ELF").found, "root lookup broke after a bad subdirectory"))
            return false;

        ImageHost noPvd;
        noPvd.image.assign(40u * 2048u, 0u);
        IopCdvd cdvd2(noPvd, memory, kernel);
        cdvd2.reset();
        return expect(!searchPath(cdvd2, memory, "cdrom0:BOOT.ELF").found, "an image without a PVD was accepted");
    }

    bool testTimrmanPeriodicCallback()
    {
        IopTimrman timrman;
        timrman.reset();
        IopCpuState cpu{};

        cpu.gpr[4] = 1u; // SYSCLK
        cpu.gpr[5] = 32u;
        cpu.gpr[6] = 1u;
        if (!expect(timrman.dispatchImport(4u, cpu, 100u), "AllocHardTimer was not handled") ||
            !expect(static_cast<int32_t>(cpu.gpr[2]) > 0, "AllocHardTimer did not allocate a 32-bit timer"))
            return false;
        const uint32_t timerId = cpu.gpr[2];

        cpu = {};
        cpu.gpr[4] = timerId;
        cpu.gpr[5] = 100u;
        cpu.gpr[6] = 0x12340u;
        cpu.gpr[7] = 0x45670u;
        cpu.gpr[28] = 0x89AB0u;
        if (!expect(timrman.dispatchImport(20u, cpu, 100u), "SetTimerHandler was not handled") ||
            !expect(cpu.gpr[2] == 0u, "SetTimerHandler failed"))
            return false;

        cpu = {};
        cpu.gpr[4] = timerId;
        cpu.gpr[5] = 1u;
        cpu.gpr[6] = 0u;
        cpu.gpr[7] = 1u;
        if (!expect(timrman.dispatchImport(22u, cpu, 100u), "SetupHardTimer was not handled") ||
            !expect(cpu.gpr[2] == 0u, "SetupHardTimer failed"))
            return false;

        cpu = {};
        cpu.gpr[4] = timerId;
        if (!expect(timrman.dispatchImport(23u, cpu, 100u), "StartHardTimer was not handled") ||
            !expect(cpu.gpr[2] == 0u, "StartHardTimer failed") ||
            !expect(timrman.nextEventCycle(1000u) == 200u, "timer compare was scheduled at the wrong cycle"))
            return false;

        RecordingExecutor executor;
        executor.callbackResult = 100u;
        timrman.serviceDue(199u, executor);
        if (!expect(executor.calls == 0u, "timer callback ran too early"))
            return false;
        timrman.serviceDue(200u, executor);
        return expect(executor.calls == 1u, "timer callback did not run") &&
               expect(executor.lastAddress == 0x12340u, "timer called the wrong handler") &&
               expect(executor.lastArgument == 0x45670u, "timer passed the wrong common argument") &&
               expect(executor.lastGp == 0x89AB0u, "timer callback lost the registering module GP") &&
               expect(timrman.nextEventCycle(1000u) == 300u, "timer callback return did not rearm compare");
    }
}

int main()
{
    if (!testLoadcoreRebootLibraryMode() || !testCdvdSpecialControl() || !testCdvdSearchFile() ||
        !testCdvdSearchFileInImage() || !testCdvdSearchFileMalformedImage() ||
        !testTimrmanPeriodicCallback())
        return 1;
    std::cout << "ps2xIOP import tests passed\n";
    return 0;
}
