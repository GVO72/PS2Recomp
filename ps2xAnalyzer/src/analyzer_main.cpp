#include "ps2recomp/elf_analyzer.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

void printUsage()
{
    std::cout << "PS2 ELF Analyzer\n";
    std::cout << "A tool to analyze PS2 ELF files and generate TOML configuration for PS2Recomp\n\n";
    std::cout << "Usage: ps2_analyzer [--functions-tsv <path>] <input_elf> <output_toml> [sce_symbol_db_dir]\n";
    std::cout << "  input_elf    Path to the PS2 ELF file\n";
    std::cout << "  output_toml  Path to output TOML configuration file\n";
    std::cout << "  sce_symbol_db_dir  Optional override directory containing symbols.json and tree.json\n";
    std::cout << "                     If omitted, the embedded SCE symbol database is used\n";
    std::cout << "  --functions-tsv    Also write every discovered function (start, end, name) as TSV\n";
}

// One line per function, sorted by start address: start, end (exclusive), name.
static bool writeFunctionsTsv(const std::string &path, const std::vector<ps2recomp::Function> &functions)
{
    std::vector<const ps2recomp::Function *> sorted;
    sorted.reserve(functions.size());
    for (const auto &function : functions)
    {
        sorted.push_back(&function);
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto *a, const auto *b)
              { return a->start != b->start ? a->start < b->start : a->end < b->end; });

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        return false;
    }
    out << "start\tend\tname\n";
    char addresses[32];
    for (const auto *function : sorted)
    {
        std::snprintf(addresses, sizeof(addresses), "0x%08X\t0x%08X\t", function->start, function->end);
        out << addresses << function->name << "\n";
    }
    return static_cast<bool>(out.flush());
}

int main(int argc, char *argv[])
{
    std::string functionsTsvPath;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--functions-tsv")
        {
            if (i + 1 >= argc)
            {
                printUsage();
                return 1;
            }
            functionsTsvPath = argv[++i];
        }
        else
        {
            positional.push_back(arg);
        }
    }
    if (positional.size() < 2)
    {
        printUsage();
        return 1;
    }

    std::string elfPath = positional[0];
    std::string tomlPath = positional[1];
    std::string sceSymbolDbPath = positional.size() >= 3 ? positional[2] : "";

    std::cout << "PS2 ELF Analyzer\n";
    std::cout << "----------------\n";
    std::cout << "Input ELF: " << elfPath << "\n";
    std::cout << "Output TOML: " << tomlPath << "\n\n";
    if (!sceSymbolDbPath.empty())
    {
        std::cout << "SCE symbol DB: " << sceSymbolDbPath << "\n\n";
    }
    else
    {
        std::cout << "SCE symbol DB: embedded\n\n";
    }

    try
    {
        ps2recomp::ElfAnalyzer analyzer(elfPath);
        if (!sceSymbolDbPath.empty())
        {
            analyzer.setSceSymbolDatabasePath(sceSymbolDbPath);
        }
 
        if (!analyzer.analyze())
        {
            std::cerr << "Failed to analyze ELF file\n";
            return 1;
        }
 
        if (!analyzer.generateToml(tomlPath))
        {
            std::cerr << "Failed to generate TOML configuration\n";
            return 1;
        }

        if (!functionsTsvPath.empty())
        {
            if (!writeFunctionsTsv(functionsTsvPath, analyzer.getFunctions()))
            {
                std::cerr << "Failed to write the function list to " << functionsTsvPath << "\n";
                return 1;
            }
            std::cout << "Function list has been written to: " << functionsTsvPath << "\n";
        }

        std::cout << "\nAnalysis complete\n";
        std::cout << "TOML configuration has been written to: " << tomlPath << "\n";
        std::cout << "\nYou can now use this configuration with PS2Recomp:\n";
        std::cout << "  ps2recomp " << tomlPath << "\n";

        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
