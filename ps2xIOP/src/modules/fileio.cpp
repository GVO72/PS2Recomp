#include "module_factories.h"
#include "rpc_reply.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace ps2x::iop::detail
{
    namespace
    {
        // The FILEIO RPC server the IOP ROM provides (EE client: fioOpen,
        // fioWrite, ... in the PS2SDK, sceOpen and friends in Sony's SDK).
        // Only console output is implemented: writes to stdout/stderr and to
        // descriptors opened on the "tty" device reach the host console. Every
        // other request fails with a logged error, never silently.
        constexpr uint32_t kFileioSid = 0x80000001u;

        enum FileioFunction : uint32_t
        {
            kOpen = 0,
            kClose = 1,
            kWrite = 3,
        };

        constexpr int32_t kErrNoEnt = -2;
        constexpr int32_t kErrIo = -5;
        constexpr int32_t kErrBadFd = -9;
        constexpr uint32_t kPathMax = 256u;          // FIO_PATH_MAX
        constexpr uint32_t kMaxWriteBytes = 0x10000u; // per call, for the console
        constexpr uint32_t kMaxLogsPerKind = 16u;

        struct WriteArg // struct _fio_write_arg
        {
            int32_t fd;
            uint32_t ptr;
            uint32_t size;
            uint32_t mis;
            uint8_t aligned[16];
        };
        static_assert(sizeof(WriteArg) == 32u, "Unexpected _fio_write_arg size");

        class FileioService final : public IopService
        {
        public:
            explicit FileioService(IopHost &host)
                : m_host(host)
            {
                reset();
            }

            [[nodiscard]] std::string_view name() const override
            {
                return "fileio";
            }

            [[nodiscard]] std::span<const uint32_t> sids() const override
            {
                return kSids;
            }

            void reset() override
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_consoleFds = {1, 2};
                m_nextFd = 3;
                m_logCounts.clear();
            }

            [[nodiscard]] RpcResult handleRpc(const RpcRequest &request) override
            {
                RpcResult result;
                if (request.sid != kFileioSid)
                    return result;

                result.handled = true;
                result.resultAddress = request.receive.address;
                const int32_t status = dispatch(request);
                const std::array<uint32_t, 1> reply{static_cast<uint32_t>(status)};
                (void)writeRpcWords(m_host, request.receive, reply);
                return result;
            }

        private:
            int32_t dispatch(const RpcRequest &request)
            {
                switch (request.function)
                {
                case kOpen:
                    return open(request);
                case kClose:
                    return close(request);
                case kWrite:
                    return write(request);
                default:
                    logOnce("function-" + std::to_string(request.function),
                            "fileio function " + std::to_string(request.function) + " is not implemented");
                    return kErrIo;
                }
            }

            int32_t open(const RpcRequest &request)
            {
                // struct _fio_open_arg: int mode; char name[256].
                std::vector<char> arg(4u + kPathMax, '\0');
                const size_t size = std::min<size_t>(arg.size(), request.send.size);
                if (size <= 4u || !m_host.readGuest(request.send.address, arg.data(), size))
                    return kErrIo;
                const std::string name(arg.data() + 4, strnlen(arg.data() + 4, kPathMax));
                if (name.rfind("tty", 0) == 0)
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    const int32_t fd = m_nextFd++;
                    m_consoleFds.insert(fd);
                    return fd;
                }
                logOnce("open", "fileio open('" + name + "') is not implemented (only tty is)");
                return kErrNoEnt;
            }

            int32_t close(const RpcRequest &request)
            {
                int32_t fd = 0;
                if (request.send.size < sizeof(fd) || !m_host.readGuest(request.send.address, &fd, sizeof(fd)))
                    return kErrIo;
                std::lock_guard<std::mutex> lock(m_mutex);
                if (fd == 1 || fd == 2)
                    return 0;
                return m_consoleFds.erase(fd) != 0u ? 0 : kErrBadFd;
            }

            int32_t write(const RpcRequest &request)
            {
                WriteArg arg{};
                if (request.send.size < sizeof(arg) || !m_host.readGuest(request.send.address, &arg, sizeof(arg)))
                    return kErrIo;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (!m_consoleFds.contains(arg.fd))
                    {
                        logOnceLocked("write-fd", "fileio write to fd " + std::to_string(arg.fd) +
                                                      " is not implemented (only the console is)");
                        return kErrBadFd;
                    }
                }

                uint32_t size = arg.size;
                if (size > kMaxWriteBytes)
                {
                    logOnce("write-size", "fileio console write of " + std::to_string(size) +
                                              " bytes truncated to " + std::to_string(kMaxWriteBytes));
                    size = kMaxWriteBytes;
                }
                // The first `mis` bytes (up to 16-byte alignment) travel in the
                // argument; the rest is read from EE memory, as the IOP would by DMA.
                const uint32_t inline_bytes = std::min<uint32_t>({arg.mis, size, 16u});
                std::string text(reinterpret_cast<const char *>(arg.aligned), inline_bytes);
                if (size > inline_bytes)
                {
                    std::string rest(size - inline_bytes, '\0');
                    if (!m_host.readGuest(arg.ptr + inline_bytes, rest.data(), rest.size()))
                        return kErrIo;
                    text += rest;
                }
                m_host.writeConsole(text);
                return static_cast<int32_t>(arg.size);
            }

            void logOnce(const std::string &kind, const std::string &message)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                logOnceLocked(kind, message);
            }

            void logOnceLocked(const std::string &kind, const std::string &message)
            {
                const uint32_t count = ++m_logCounts[kind];
                if (count <= kMaxLogsPerKind)
                    m_host.log(LogLevel::Warning, "[FILEIO:HLE] " + message);
                if (count == kMaxLogsPerKind)
                    m_host.log(LogLevel::Warning, "[FILEIO:HLE] further '" + kind + "' messages are suppressed");
            }

            inline static constexpr std::array<uint32_t, 1> kSids{kFileioSid};

            IopHost &m_host;
            std::mutex m_mutex;
            std::set<int32_t> m_consoleFds;
            int32_t m_nextFd = 3;
            std::unordered_map<std::string, uint32_t> m_logCounts;
        };
    }

    std::unique_ptr<IopService> createFileioService(IopHost &host)
    {
        return std::make_unique<FileioService>(host);
    }
}
