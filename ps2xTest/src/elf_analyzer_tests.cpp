#include "MiniTest.h"
#include "ps2recomp/elf_analyzer.h"
#include "ps2recomp/function_classifier.h"
#include "ps2recomp/instructions.h"
#include "ps2recomp/types.h"

#include <elfio/elfio.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <unordered_map>
#include <vector>

using namespace ps2recomp;

namespace
{
    Instruction makeInstruction(uint32_t address, uint32_t opcode)
    {
        Instruction inst;
        inst.address = address;
        inst.opcode = opcode;
        return inst;
    }

    // .text (RX) plus a .data section that is writable and executable ("WAX"):
    //   0x00100000 jal 0x00200100 (a function inside the WAX section), then a return
    //   0x00100010 and 0x00100020 leaf thunks right after a return, referenced only by
    //              a pointer table in the WAX section
    //   0x00100040 a leaf that only a JAL-shaped data word in the WAX section names
    //   0x00200000 the pointer table (its third entry points back into the section), then words that do not decode as instructions
    //   0x00200100 a function reached by the JAL above
    bool writeElfWithWritableExecutableSection(const std::filesystem::path &elfPath)
    {
        ELFIO::elfio writer;
        writer.create(ELFIO::ELFCLASS32, ELFIO::ELFDATA2LSB);
        writer.set_os_abi(ELFIO::ELFOSABI_NONE);
        writer.set_type(ELFIO::ET_EXEC);
        writer.set_machine(ELFIO::EM_MIPS);
        writer.set_entry(0x00100000u);

        std::array<uint32_t, 32> textWords{};
        textWords[0] = 0x0C080040u; // jal 0x00200100
        textWords[1] = 0x00000000u; // nop
        textWords[2] = 0x03E00008u; // jr ra
        textWords[3] = 0x00000000u; // nop
        textWords[4] = 0x03E00008u; // thunk at 0x00100010: jr ra
        textWords[5] = 0x00000000u;
        textWords[8] = 0x03E00008u; // thunk at 0x00100020: jr ra
        textWords[9] = 0x00000000u;
        textWords[16] = 0x03E00008u; // leaf at 0x00100040: jr ra
        textWords[17] = 0x00000000u;

        ELFIO::section *text = writer.sections.add(".text");
        text->set_type(ELFIO::SHT_PROGBITS);
        text->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_EXECINSTR);
        text->set_addr_align(4);
        text->set_address(0x00100000u);
        text->set_data(reinterpret_cast<const char *>(textWords.data()), textWords.size() * sizeof(uint32_t));

        std::array<uint32_t, 128> dataWords{};
        dataWords[0] = 0x00100010u; // pointer table
        dataWords[1] = 0x00100020u;
        dataWords[2] = 0x00200180u; // points into the section: a table does not prove code there
        dataWords[4] = 0xFFFFFFFFu; // not instructions
        dataWords[5] = 0xFFFF0000u;
        dataWords[6] = 0x7C7C7C7Cu;
        dataWords[7] = 0x0C040010u; // JAL-shaped data word naming the leaf at 0x00100040
        dataWords[8] = 0xFFFFFFFFu;
        dataWords[9] = 0xEFEFEFEFu;
        dataWords[64] = 0x03E00008u; // function at 0x00200100: jr ra
        dataWords[65] = 0x00000000u;
        dataWords[96] = 0x03E00008u; // return at 0x00200180, named only by the table above
        dataWords[97] = 0x00000000u;

        ELFIO::section *data = writer.sections.add(".data");
        data->set_type(ELFIO::SHT_PROGBITS);
        data->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_WRITE | ELFIO::SHF_EXECINSTR);
        data->set_addr_align(4);
        data->set_address(0x00200000u);
        data->set_data(reinterpret_cast<const char *>(dataWords.data()), dataWords.size() * sizeof(uint32_t));

        ELFIO::segment *segment = writer.segments.add();
        segment->set_type(ELFIO::PT_LOAD);
        segment->set_flags(ELFIO::PF_R | ELFIO::PF_W | ELFIO::PF_X);
        segment->set_align(0x1000);
        segment->add_section_index(text->get_index(), text->get_addr_align());
        segment->add_section_index(data->get_index(), data->get_addr_align());

        return writer.save(elfPath.string());
    }
}

void register_elf_analyzer_tests()
{
    MiniTest::Case("ElfAnalyzerHeuristics", [](TestCase &tc)
                   {
                       tc.Run("library-symbol classification table", [](TestCase &t)
                              {
            ElfAnalyzer analyzer("dummy.elf");

            t.IsTrue(analyzer.isLibrarySymbolNameForHeuristics("printf"),
                     "printf should be classified as library");
            t.IsTrue(analyzer.isLibrarySymbolNameForHeuristics("_printf"),
                     "_printf should be classified as library");
            t.IsTrue(analyzer.isLibrarySymbolNameForHeuristics("sceCdRead"),
                     "sce-prefixed PS2 API should be classified as library");
            t.IsTrue(analyzer.isLibrarySymbolNameForHeuristics("SetSyscall"),
                     "SetSyscall kernel wrapper should be classified as library/runtime");
            t.IsTrue(analyzer.isLibrarySymbolNameForHeuristics("SetTLBEntry"),
                     "SetTLBEntry kernel wrapper should be classified as library/runtime");
            t.IsTrue(analyzer.isLibrarySymbolNameForHeuristics("InitTLB"),
                     "InitTLB kernel wrapper should be classified as library/runtime");
            t.IsTrue(analyzer.isLibrarySymbolNameForHeuristics("AddIntcHandler2"),
                     "AddIntcHandler2 kernel wrapper should be classified as library/runtime");
            t.IsTrue(analyzer.isLibrarySymbolNameForHeuristics("SetGsCrt"),
                     "SetGsCrt kernel wrapper should be classified as library/runtime");
            t.IsTrue(analyzer.isLibrarySymbolNameForHeuristics("__sbprintf"),
                     "optional stdio internals should be classified as library functions");

            t.IsFalse(analyzer.isLibrarySymbolNameForHeuristics("bhEne13_Brain"),
                      "named game function should not be classified as library");
            t.IsFalse(analyzer.isLibrarySymbolNameForHeuristics("ScenePrerender"),
                      "game functions beginning with Scene should not be classified as sce SDK APIs");
            t.IsFalse(analyzer.isLibrarySymbolNameForHeuristics("sub_00100C00"),
                      "unreliable auto-generated names should not be classified as library"); });

                       tc.Run("runtime handler filter keeps unsupported SDK names informational", [](TestCase &t)
                              {
            t.IsTrue(FunctionClassifier::hasRuntimeHandler("sceCdRead"),
                     "sceCdRead should resolve to a known runtime stub handler");
            t.IsTrue(FunctionClassifier::hasRuntimeHandler("_printf"),
                     "runtime handler resolution should accept leading underscore aliases");
            t.IsTrue(FunctionClassifier::hasRuntimeHandler("__ieee754_rem_pio2f"),
                     "double-underscore libm helpers should be active stubs only when the runtime knows them");
            t.IsTrue(FunctionClassifier::hasRuntimeHandler("__kernel_cosf"),
                     "runtime-known libm kernel helpers should resolve exactly");
            t.IsTrue(FunctionClassifier::hasRuntimeHandler("_realloc_r"),
                     "reentrant allocator aliases should resolve to runtime stubs");
            t.IsTrue(FunctionClassifier::hasRuntimeHandler("__malloc_lock"),
                     "newlib malloc locks should resolve to runtime stubs");
            t.IsTrue(FunctionClassifier::hasRuntimeHandler("memclr"),
                     "libdma memclr should resolve to a runtime stub");
            t.IsTrue(FunctionClassifier::hasRuntimeHandler("__divdi3"),
                     "libgcc 64-bit division should resolve to a runtime stub");
            t.IsFalse(FunctionClassifier::hasRuntimeHandler("GetRomName"),
                      "ABI-incompatible GetRomName variants must be recompiled instead of name-stubbed");
            t.IsFalse(FunctionClassifier::hasRuntimeHandler("__sbprintf"),
                      "optional stdio internals should not resolve as automatic runtime stubs");
            t.IsFalse(FunctionClassifier::hasRuntimeHandler("__sprint"),
                      "optional stdio internals should be TOML opt-in only");
            t.IsFalse(FunctionClassifier::hasRuntimeHandler("scePP1_Kick"),
                      "SDK functions without runtime handlers should not be active stubs"); });

                       tc.Run("reliable-symbol heuristic filters autogenerated names", [](TestCase &t)
                              {
            t.IsTrue(ElfAnalyzer::isReliableSymbolNameForHeuristics("bhEne13_Brain"),
                     "expected game symbol to be considered reliable");
            t.IsTrue(ElfAnalyzer::isReliableSymbolNameForHeuristics("SetupSoundDriver"),
                     "expected named function to be considered reliable");
            t.IsTrue(ElfAnalyzer::isReliableSymbolNameForHeuristics("sceCdRead"),
                     "expected PS2 API symbol to be considered reliable");

            t.IsFalse(ElfAnalyzer::isReliableSymbolNameForHeuristics("sub_00100C00"),
                      "sub_ prefix should be treated as unreliable");
            t.IsFalse(ElfAnalyzer::isReliableSymbolNameForHeuristics("func_1ABC"),
                      "func_ prefix should be treated as unreliable");
            t.IsFalse(ElfAnalyzer::isReliableSymbolNameForHeuristics("entry_001000"),
                      "entry_ prefix should be treated as unreliable");
            t.IsFalse(ElfAnalyzer::isReliableSymbolNameForHeuristics("LAB_00001234"),
                      "LAB_ prefix should be treated as unreliable");
            t.IsFalse(ElfAnalyzer::isReliableSymbolNameForHeuristics("0x00100ABC"),
                      "pure hex-style symbol should be treated as unreliable"); });

                       tc.Run("entry-point mapping handles exact inside and fallback", [](TestCase &t)
                              {
            Function f1;
            f1.name = "funcA";
            f1.start = 0x1000;
            f1.end = 0x1100;

            Function f2;
            f2.name = "funcB";
            f2.start = 0x1100;
            f2.end = 0x1200;

            Function f3;
            f3.name = "fallbackA";
            f3.start = 0x100000;
            f3.end = 0x100100;

            std::vector<Function> functions{f1, f2, f3};

            t.Equals(ElfAnalyzer::findEntryFunctionIndexForHeuristics(functions, 0x1100), 1,
                     "exact entry should map to function start");
            t.Equals(ElfAnalyzer::findEntryFunctionIndexForHeuristics(functions, 0x10F0), 0,
                     "entry inside range should map to containing function");
            t.Equals(ElfAnalyzer::findEntryFunctionIndexForHeuristics(functions, 0x2000), -1,
                     "unknown entry should return no mapping");
            t.Equals(ElfAnalyzer::findFallbackEntryFunctionIndexForHeuristics(functions), 2,
                     "fallback should find 0x100000 entry");

            Function fallbackB;
            fallbackB.name = "fallbackB";
            fallbackB.start = 0x80100000;
            fallbackB.end = 0x80100100;

            std::vector<Function> fallbackOnly{fallbackB};
            t.Equals(ElfAnalyzer::findFallbackEntryFunctionIndexForHeuristics(fallbackOnly), 0,
                     "fallback should also accept 0x80100000"); });

                       tc.Run("risk signal detection reports hardware io mmi and self modifying code", [](TestCase &t)
                              {
            // Hardware I/O signal via LUI upper address in I/O region.
            Instruction hw = makeInstruction(0x1000, OPCODE_LUI);
            hw.immediate = 0x1002; // 0x10020000
            std::vector<Instruction> hwInst{hw};
            const bool hasHardwareIO = ElfAnalyzer::hasHardwareIOSignalForHeuristics(hwInst);
            t.IsTrue(hasHardwareIO, "hardware I/O signal should be detected");

            // Large + complex MMI signal.
            std::vector<Instruction> largeMmi(501);
            largeMmi[250] = makeInstruction(0x2000, OPCODE_MMI);
            largeMmi[250].isMMI = true;
            largeMmi[250].function = MMI_MMI1;
            const bool hasLargeComplexMMI = ElfAnalyzer::hasLargeComplexMMISignalForHeuristics(largeMmi);
            t.IsTrue(hasLargeComplexMMI, "large complex MMI signal should be detected");

            // Self-modifying signal: SW into a code section, with base from preceding LUI.
            Instruction lui = makeInstruction(0x3000, OPCODE_LUI);
            lui.rt = 9;
            lui.immediate = 0x1000; // base 0x10000000
            Instruction sw = makeInstruction(0x3004, OPCODE_SW);
            sw.rs = 9;
            sw.immediate = 0x2000; // target 0x10002000
            
            std::vector<Instruction> smcInst{lui, sw};
            Section code{};
            code.name = ".text";
            code.address = 0x10002000;
            code.size = 0x100;
            code.isCode = true;
            std::vector<Section> sections{code};
            const bool hasSelfModifying = ElfAnalyzer::hasSelfModifyingSignalForHeuristics(smcInst, sections);
            t.IsTrue(hasSelfModifying, "self-modifying signal should be detected");

            (void)hasHardwareIO;
            (void)hasLargeComplexMMI;
            (void)hasSelfModifying; });

                       tc.Run("jump-table detection finds canonical sltiu/bne/lw/jr pattern", [](TestCase &t)
                              {
            // sltiu -> bne/beq bounds check -> ... -> lui/addiu base -> lw -> jr loadedReg
            Instruction sltiu = makeInstruction(0x4000, OPCODE_SLTIU);
            sltiu.immediate = 3; // number of entries
            Instruction bne = makeInstruction(0x4004, OPCODE_BNE);
            Instruction filler = makeInstruction(0x4008, OPCODE_ADDIU);
            Instruction jtLui = makeInstruction(0x400C, OPCODE_LUI);
            jtLui.rt = 8;
            jtLui.immediate = 0x2000;
            Instruction jtAddiu = makeInstruction(0x4010, OPCODE_ADDIU);
            jtAddiu.rs = 8;
            jtAddiu.rt = 9; // load base register
            jtAddiu.immediate = 0x0100;
            Instruction jtLoad = makeInstruction(0x4014, OPCODE_LW);
            jtLoad.rs = 9;
            jtLoad.rt = 10;
            Instruction jtJump = makeInstruction(0x4018, OPCODE_SPECIAL);
            jtJump.function = SPECIAL_JR;
            jtJump.rs = 10;

            std::vector<Instruction> instructions{sltiu, bne, filler, jtLui, jtAddiu, jtLoad, jtJump};

            const uint32_t base = (0x2000u << 16) | 0x0100u;
            std::unordered_map<uint32_t, uint32_t> tableMemory{
                {base + 0, 0x101000},
                {base + 4, 0x102000},
                {base + 8, 0x103000},
            };

            auto readWord = [&tableMemory](uint32_t address, uint32_t &outWord) -> bool
            {
                auto it = tableMemory.find(address);
                if (it == tableMemory.end())
                {
                    return false;
                }
                outWord = it->second;
                return true;
            };

            auto jumpTables = ElfAnalyzer::detectJumpTablesForHeuristics(instructions, std::vector<Section>(), readWord);
            t.Equals(jumpTables.size(), static_cast<size_t>(1), "one jump table should be detected");
            if (!jumpTables.empty())
            {
                t.Equals(jumpTables[0].address, base, "jump table base address should match LUI/ADDIU pattern");
                t.Equals(jumpTables[0].baseRegister, static_cast<uint32_t>(9), "base register should match LW base");
                t.Equals(jumpTables[0].entries.size(), static_cast<size_t>(3), "entry count should match SLTIU bound");
                t.Equals(jumpTables[0].entries[0].target, static_cast<uint32_t>(0x101000), "entry 0 target should match");
                t.Equals(jumpTables[0].entries[1].target, static_cast<uint32_t>(0x102000), "entry 1 target should match");
                t.Equals(jumpTables[0].entries[2].target, static_cast<uint32_t>(0x103000), "entry 2 target should match");
            }

            Instruction invalid = sltiu;
            invalid.immediate = 1001; // rejected by guard
            auto invalidTables = ElfAnalyzer::detectJumpTablesForHeuristics(
                std::vector<Instruction>{invalid, bne, filler, jtLui, jtAddiu, jtLoad, jtJump}, std::vector<Section>(),
                readWord);
            t.Equals(invalidTables.size(), static_cast<size_t>(0),
                     "bounds over guard limit should not produce a jump table"); }); });

    MiniTest::Case("ElfAnalyzerWritableExecutableSection", [](TestCase &tc)
                   {
                       tc.Run("writable executable section is scanned for pointers and not decoded as code", [](TestCase &t)
                              {
            const auto uniqueSuffix = std::to_string(
                static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
            const std::filesystem::path elfPath =
                std::filesystem::temp_directory_path() / ("ps2recomp-wax-" + uniqueSuffix + ".elf");

            const bool writeOk = writeElfWithWritableExecutableSection(elfPath);
            t.IsTrue(writeOk, "temporary ELF should be generated");
            if (!writeOk)
            {
                return;
            }

            ElfAnalyzer analyzer(elfPath.string());
            const bool analyzeOk = analyzer.analyze();
            t.IsTrue(analyzeOk, "generated ELF should analyze");
            if (analyzeOk)
            {
                const auto &functions = analyzer.getFunctions();
                auto hasStart = [&functions](uint32_t start)
                {
                    return std::any_of(functions.begin(), functions.end(),
                                       [start](const Function &function)
                                       { return function.start == start; });
                };

                t.IsTrue(hasStart(0x00100000u), "the entry point should be a function");
                t.IsTrue(hasStart(0x00100010u),
                         "a thunk named only by a pointer table in a writable executable section should be discovered");
                t.IsTrue(hasStart(0x00100020u),
                         "the second thunk of that pointer table should be discovered");
                t.IsTrue(hasStart(0x00200100u),
                         "a function in a writable executable section reached by a JAL from .text should be kept");
                t.IsFalse(hasStart(0x00100040u),
                          "a JAL-shaped data word in a writable executable section must not name a function");

                size_t startsInWax = 0;
                for (const Function &function : functions)
                {
                    if (function.start >= 0x00200000u && function.start < 0x00200200u)
                    {
                        ++startsInWax;
                    }
                }
                t.Equals(startsInWax, static_cast<size_t>(1),
                         "only the reached function in a writable executable section should be a function");
            }

            std::error_code removeError;
            std::filesystem::remove(elfPath, removeError); }); });
}
