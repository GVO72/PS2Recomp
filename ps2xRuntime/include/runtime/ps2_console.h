#ifndef PS2_CONSOLE_H
#define PS2_CONSOLE_H

// The guest's console output, split by the processor that printed it.
//
// A console sink (PS2Runtime::setConsoleSink) receives every completed line, with the '\n'
// removed and everything else (a '\r', other control bytes, text of any encoding) unchanged. It
// is called on whichever thread printed the text (the game thread, in every mode the runner
// uses), under a lock, so it must not call back into the console.
//
// Which source a path belongs to follows where PCSX2 classifies the same text on real hardware
// (its "EEout"/"DECI2" console for the EE, "IOPout" for the IOP):
//
//   path                                                      source
//   --------------------------------------------------------  ------
//   IOP stdout/stderr through FILEIO, the tty device          Iop
//     (PS2IopHostAdapter::writeConsole)
//   guest libc printf and puts stubs (ps2_stubs::printf,      Iop   a guest's stdout is the
//     ps2_stubs::puts)                                              IOP's FILEIO stdout
//   kernel Deci2Call kputs (syscall 0x7C, code 0x10)          Ee    PCSX2 "DECI2"
//   scePrintf                                                 Ee    EE kernel text (SIO)
//
// Not routed, because the runtime has no text to give: sceDeci2ReqSend (PCSX2 prints the packet
// payload; the runtime ignores it), sceDeci2ExSend (PCSX2 prints nothing; the runtime only logs
// the bytes), the EE SIO registers (not emulated) and the kernel's SysPrintOut syscall (not
// implemented). The Deci2/kputs and scePrintf paths do not echo to stdout, as before.

#include <filesystem>
#include <functional>
#include <string_view>

enum class ConsoleSource
{
    Ee,
    Iop,
};

using ConsoleSink = std::function<void(ConsoleSource source, std::string_view line)>;

namespace ps2_console
{
    // Appends `text` (any number of lines and partial lines) to the source's pending line. Each
    // '\n' completes a line and calls the sink. Iop lines are also echoed to stdout as
    // "[IOP tty] <line>" and written to the file set by PS2Runtime::setIopConsolePath.
    void write(ConsoleSource source, std::string_view text);
}

#endif // PS2_CONSOLE_H
