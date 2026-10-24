#pragma once

#include "ps2x/iop/iop_types.h"

#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct PS2RomProfile
{
    std::string id;
    std::string provider = "application";
    ps2x::iop::GameMatcher matcher;
    std::unordered_map<std::string, std::vector<uint8_t>> files;
};

class PS2RomDevice
{
public:
    PS2RomDevice();

    static void registerProfile(PS2RomProfile profile);

    // True for a 14-character ROMVER: 4 digits (version), a region letter, a type letter and 8
    // digits (build date), e.g. the layout of the BIOS ROM's ROMVER entry.
    [[nodiscard]] static bool isValidRomVersion(std::string_view romver) noexcept;

    // Sets the ROMVER that rom0:ROMVER returns, in place of the built-in default. It survives
    // configure() and wins over a profile's own ROMVER. Returns false (and sets *error) when
    // the shape is invalid; the current value then stays.
    bool setRomVersion(std::string_view romver, std::string *error = nullptr);
    [[nodiscard]] bool hasRomVersionOverride() const noexcept { return !m_romVersion.empty(); }

    bool configure(const ps2x::iop::GameIdentity &identity, std::string *error = nullptr);
    [[nodiscard]] bool readFile(std::string_view ps2Path, std::vector<uint8_t> &bytes) const;
    [[nodiscard]] bool fileSize(std::string_view ps2Path, uint64_t &size) const;
    [[nodiscard]] bool contains(std::string_view ps2Path) const;
    [[nodiscard]] std::string_view activeProfile() const noexcept { return m_activeProfile; }
    [[nodiscard]] std::string_view activeProvider() const noexcept { return m_activeProvider; }

private:
    static std::string normalizePath(std::string_view path);
    void mountBaseProfile();
    void mountFiles(const std::unordered_map<std::string, std::vector<uint8_t>> &files);

    std::unordered_map<std::string, std::vector<uint8_t>> m_files;
    std::string m_activeProfile;
    std::string m_activeProvider;
    std::string m_romVersion; // empty: the built-in default
};
