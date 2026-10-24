#include "MiniTest.h"
#include "ps2_runtime.h"
#include "ps2_stubs.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace
{
    constexpr uint32_t kTextAddr = 0x2000u;

    using Lines = std::vector<std::pair<ConsoleSource, std::string>>;

    // Collects the lines of the global console sink while it is alive.
    struct SinkCapture
    {
        Lines lines;

        SinkCapture()
        {
            PS2Runtime::setConsoleSink([this](ConsoleSource source, std::string_view line)
                                       { lines.emplace_back(source, std::string(line)); });
        }

        ~SinkCapture()
        {
            PS2Runtime::setConsoleSink({});
        }
    };

    void callStub(void (*stub)(uint8_t *, R5900Context *, PS2Runtime *), const std::string &text)
    {
        std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0u);
        PS2Runtime runtime;
        R5900Context ctx{};
        std::memcpy(rdram.data() + kTextAddr, text.c_str(), text.size() + 1);
        ctx.r[4] = _mm_set_epi64x(0, static_cast<int64_t>(kTextAddr));
        stub(rdram.data(), &ctx, &runtime);
    }
}

void register_ps2_console_tests()
{
    MiniTest::Case("PS2Console", [](TestCase &tc)
    {
        tc.Run("the sink gets whole lines per source, with partial lines kept apart", [](TestCase &t)
        {
            SinkCapture capture;
            ps2_console::write(ConsoleSource::Iop, "one\r\ntw");
            ps2_console::write(ConsoleSource::Ee, "ee a");
            ps2_console::write(ConsoleSource::Iop, "o\n\nthree");
            ps2_console::write(ConsoleSource::Ee, "\n");
            const Lines want = {{ConsoleSource::Iop, "one\r"},
                                {ConsoleSource::Iop, "two"},
                                {ConsoleSource::Iop, ""},
                                {ConsoleSource::Ee, "ee a"}};
            t.IsTrue(capture.lines == want, "lines complete per source on '\\n' only");

            PS2Runtime::flushConsole();
            t.Equals(capture.lines.size(), static_cast<size_t>(5), "flush emits the trailing partial line");
            t.IsTrue(capture.lines.back() == std::make_pair(ConsoleSource::Iop, std::string("three")),
                     "the partial IOP line is flushed");
            PS2Runtime::flushConsole();
            t.Equals(capture.lines.size(), static_cast<size_t>(5), "a second flush emits nothing");
        });

        tc.Run("scePrintf is EE text; the libc printf and puts stubs are IOP text", [](TestCase &t)
        {
            SinkCapture capture;
            callStub(ps2_stubs::scePrintf, "from the ee kernel\n");
            callStub(ps2_stubs::printf, "from printf\n");
            callStub(ps2_stubs::puts, "from puts");
            const Lines want = {{ConsoleSource::Ee, "from the ee kernel"},
                                {ConsoleSource::Iop, "from printf"},
                                {ConsoleSource::Iop, "from puts"}};
            t.IsTrue(capture.lines == want, "each stub reaches the sink with its source");
        });

        tc.Run("setIopConsolePath still gets IOP lines only, and an empty sink stops the sink", [](TestCase &t)
        {
            const std::filesystem::path file =
                std::filesystem::temp_directory_path() / "ps2x-console-test-iop.txt";
            PS2Runtime::setIopConsolePath(file);
            {
                SinkCapture capture;
                ps2_console::write(ConsoleSource::Iop, "iop line\n");
                ps2_console::write(ConsoleSource::Ee, "ee line\n");
                t.Equals(capture.lines.size(), static_cast<size_t>(2), "both sources reach the sink");
            }
            ps2_console::write(ConsoleSource::Iop, "after\n"); // no sink now: must not crash
            PS2Runtime::setIopConsolePath({});
            std::ifstream in(file, std::ios::binary);
            const std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            t.Equals(contents, std::string("iop line\nafter\n"), "the file holds the IOP lines only");
            std::error_code ignored;
            std::filesystem::remove(file, ignored);
        });
    });
}
