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
        // Implemented: console output (stdout/stderr and descriptors opened on
        // the "tty" device), and opening, reading, seeking and closing files
        // through the host's file layer (rom0:, host:, cdrom0:, ...). Every
        // other request fails with a logged error, never silently.
        constexpr uint32_t kFileioSid = 0x80000001u;

        enum FileioFunction : uint32_t
        {
            kOpen = 0,
            kClose = 1,
            kRead = 2,
            kWrite = 3,
            kLseek = 4,
        };

        constexpr int32_t kErrNoEnt = -2;
        constexpr int32_t kErrIo = -5;
        constexpr int32_t kErrBadFd = -9;
        constexpr uint32_t kPathMax = 256u;          // FIO_PATH_MAX
        constexpr uint32_t kMaxWriteBytes = 0x10000u; // per call, for the console
        constexpr uint32_t kMaxReadBytes = 0x1000000u; // per call (16 MB)
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

        struct ReadArg // struct _fio_read_arg
        {
            int32_t fd;
            uint32_t ptr;
            uint32_t size;
            uint32_t readData; // EE struct _fio_read_data for unaligned edges
        };

        struct LseekArg // struct _fio_lseek_arg
        {
            int32_t fd;
            int32_t offset;
            int32_t whence;
        };

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
                for (const auto &[fd, hostFd] : m_files)
                    (void)m_host.closeGuestFile(hostFd);
                m_files.clear();
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
                case kRead:
                    return read(request);
                case kWrite:
                    return write(request);
                case kLseek:
                    return lseek(request);
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
                int32_t mode = 0;
                std::memcpy(&mode, arg.data(), sizeof(mode));
                const int32_t hostFd = m_host.openGuestFile(name, static_cast<uint32_t>(mode));
                if (hostFd < 0)
                {
                    logOnce("open-failed", "fileio open('" + name + "') failed: no such file");
                    return kErrNoEnt;
                }
                std::lock_guard<std::mutex> lock(m_mutex);
                const int32_t fd = m_nextFd++;
                m_files[fd] = hostFd;
                return fd;
            }

            int32_t close(const RpcRequest &request)
            {
                int32_t fd = 0;
                if (request.send.size < sizeof(fd) || !m_host.readGuest(request.send.address, &fd, sizeof(fd)))
                    return kErrIo;
                std::lock_guard<std::mutex> lock(m_mutex);
                if (fd == 1 || fd == 2)
                    return 0;
                if (const auto file = m_files.find(fd); file != m_files.end())
                {
                    (void)m_host.closeGuestFile(file->second);
                    m_files.erase(file);
                    return 0;
                }
                return m_consoleFds.erase(fd) != 0u ? 0 : kErrBadFd;
            }

            // The host file behind a FILEIO descriptor, or -1.
            int32_t hostFile(int32_t fd)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                const auto file = m_files.find(fd);
                return file != m_files.end() ? file->second : -1;
            }

            int32_t read(const RpcRequest &request)
            {
                ReadArg arg{};
                if (request.send.size < sizeof(arg) || !m_host.readGuest(request.send.address, &arg, sizeof(arg)))
                    return kErrIo;
                const int32_t hostFd = hostFile(arg.fd);
                if (hostFd < 0)
                {
                    logOnce("read-fd", "fileio read from fd " + std::to_string(arg.fd) +
                                           " is not implemented (only opened files are)");
                    return kErrBadFd;
                }
                uint32_t size = arg.size;
                if (size > kMaxReadBytes)
                {
                    logOnce("read-size", "fileio read of " + std::to_string(size) + " bytes truncated to " +
                                             std::to_string(kMaxReadBytes));
                    size = kMaxReadBytes;
                }
                std::vector<uint8_t> data(size);
                const int64_t got = m_host.readGuestFile(hostFd, data.data(), data.size());
                if (got < 0)
                    return kErrIo;
                // The IOP DMAs the aligned middle and hands the unaligned edges to
                // the EE's completion handler; this writes it all directly and tells
                // that handler there are no edges to copy.
                if (got > 0 && !m_host.writeGuest(arg.ptr, data.data(), static_cast<size_t>(got)))
                    return kErrIo;
                if (arg.readData != 0u)
                {
                    const std::array<uint32_t, 2> noEdges{0u, 0u}; // size1, size2
                    (void)m_host.writeGuest(arg.readData, noEdges.data(), sizeof(noEdges));
                }
                return static_cast<int32_t>(got);
            }

            int32_t lseek(const RpcRequest &request)
            {
                LseekArg arg{};
                if (request.send.size < sizeof(arg) || !m_host.readGuest(request.send.address, &arg, sizeof(arg)))
                    return kErrIo;
                const int32_t hostFd = hostFile(arg.fd);
                if (hostFd < 0)
                {
                    logOnce("lseek-fd", "fileio lseek on fd " + std::to_string(arg.fd) +
                                            " is not implemented (only opened files are)");
                    return kErrBadFd;
                }
                const int64_t position = m_host.seekGuestFile(hostFd, arg.offset, arg.whence);
                return position < 0 ? kErrIo : static_cast<int32_t>(position);
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
            std::unordered_map<int32_t, int32_t> m_files; // FILEIO descriptor -> host file
            int32_t m_nextFd = 3;
            std::unordered_map<std::string, uint32_t> m_logCounts;
        };
    }

    std::unique_ptr<IopService> createFileioService(IopHost &host)
    {
        return std::make_unique<FileioService>(host);
    }
}
