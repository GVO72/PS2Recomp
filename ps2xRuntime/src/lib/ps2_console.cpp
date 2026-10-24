#include "runtime/ps2_console.h"
#include "ps2_runtime.h"

#include <fstream>
#include <iostream>
#include <mutex>
#include <string>

namespace
{
    std::mutex g_consoleMutex;
    std::string g_pending[2];
    ConsoleSink g_sink;
    std::ofstream g_iopConsoleFile;

    std::string &pendingOf(ConsoleSource source)
    {
        return g_pending[source == ConsoleSource::Ee ? 0 : 1];
    }

    void emitLineLocked(ConsoleSource source)
    {
        std::string &line = pendingOf(source);
        if (source == ConsoleSource::Iop)
        {
            std::cout << "[IOP tty] " << line << std::endl;
            if (g_iopConsoleFile.is_open())
            {
                // Flushed per line: the runner may end the process with _Exit.
                g_iopConsoleFile << line << '\n';
                g_iopConsoleFile.flush();
            }
        }
        if (g_sink)
        {
            g_sink(source, line);
        }
        line.clear();
    }
}

namespace ps2_console
{
    void write(ConsoleSource source, std::string_view text)
    {
        std::lock_guard<std::mutex> lock(g_consoleMutex);
        for (const char c : text)
        {
            if (c == '\n')
            {
                emitLineLocked(source);
            }
            else
            {
                pendingOf(source).push_back(c);
            }
        }
    }
}

void PS2Runtime::setConsoleSink(ConsoleSink sink)
{
    std::lock_guard<std::mutex> lock(g_consoleMutex);
    g_sink = std::move(sink);
}

void PS2Runtime::flushConsole()
{
    std::lock_guard<std::mutex> lock(g_consoleMutex);
    for (const ConsoleSource source : {ConsoleSource::Ee, ConsoleSource::Iop})
    {
        if (!pendingOf(source).empty())
        {
            emitLineLocked(source);
        }
    }
}

void PS2Runtime::setIopConsolePath(const std::filesystem::path &path)
{
    std::lock_guard<std::mutex> lock(g_consoleMutex);
    if (g_iopConsoleFile.is_open())
    {
        g_iopConsoleFile.close();
    }
    g_iopConsoleFile.clear();
    if (!path.empty())
    {
        g_iopConsoleFile.open(path, std::ios::binary | std::ios::trunc);
        if (!g_iopConsoleFile.is_open())
        {
            std::cerr << "[ps2xIOP:warning] cannot write the IOP console to " << path << std::endl;
        }
    }
}
