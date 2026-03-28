#pragma once
/// @file firmware_version.hpp
/// Notecard firmware version probing and test suite gating.
///
/// Probes the connected Notecard for its firmware version at test startup,
/// then auto-excludes doctest test suites tagged with version requirements
/// above what the firmware supports.
///
/// Tests are tagged by wrapping them in TEST_SUITE("fw>=X.Y.Z"):
///
///   TEST_SUITE("fw>=5.1.1") {
///   TEST_CASE("feature that needs 5.1.1+") { ... }
///   }
///
/// The exclude list is built from VERSION_GATES[] and applied to the
/// doctest context before tests run.

#include <note/types.hpp>

#include <cstdlib>
#include <string>

/// Parse "notecard-7.2.1.12345" → NOTE_VERSION(7,2,1) = 70201.
/// Returns 0 if the string can't be parsed.
inline int parse_firmware_version(note::string_view s) {
    // Skip prefix up to the first digit
    size_t i = 0;
    while (i < s.size() && (s[i] < '0' || s[i] > '9')) ++i;
    if (i >= s.size()) return 0;

    // Parse major.minor.patch
    int parts[3] = {0, 0, 0};
    for (int p = 0; p < 3 && i < s.size(); ++p) {
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            parts[p] = parts[p] * 10 + (s[i] - '0');
            ++i;
        }
        if (i < s.size() && s[i] == '.') ++i;  // skip dot
    }
    return NOTE_VERSION(parts[0], parts[1], parts[2]);
}

/// Known version gates used in test suites.
/// Add new entries as firmware features require gating.
struct VersionGate {
    int version;
    const char* suite;
};

inline constexpr VersionGate VERSION_GATES[] = {
    { NOTE_VERSION(3, 2, 1), "fw>=3.2.1" },
    { NOTE_VERSION(3, 3, 1), "fw>=3.3.1" },
    { NOTE_VERSION(3, 4, 1), "fw>=3.4.1" },
    { NOTE_VERSION(3, 5, 1), "fw>=3.5.1" },
    { NOTE_VERSION(4, 1, 1), "fw>=4.1.1" },
    { NOTE_VERSION(5, 1, 1), "fw>=5.1.1" },
    { NOTE_VERSION(5, 3, 1), "fw>=5.3.1" },
    { NOTE_VERSION(6, 2, 3), "fw>=6.2.3" },
    { NOTE_VERSION(7, 2, 1), "fw>=7.2.1" },
    { NOTE_VERSION(7, 2, 2), "fw>=7.2.2" },
    { NOTE_VERSION(7, 3, 1), "fw>=7.3.1" },
    { NOTE_VERSION(7, 5, 1), "fw>=7.5.1" },
    { NOTE_VERSION(7, 5, 2), "fw>=7.5.2" },
    { NOTE_VERSION(8, 2, 1), "fw>=8.2.1" },
    { NOTE_VERSION(9, 1, 1), "fw>=9.1.1" },
};

/// Build comma-separated exclude pattern for suites above fw_version.
/// Returns empty string if all suites are supported.
inline std::string build_version_excludes(int fw_version) {
    std::string excludes;
    for (const auto& gate : VERSION_GATES) {
        if (fw_version < gate.version) {
            if (!excludes.empty()) excludes += ',';
            excludes += '*';
            excludes += gate.suite;
            excludes += '*';
        }
    }
    return excludes;
}

/// Parse --fw-version X.Y.Z from a string (for host-driven override).
/// Returns 0 if not found.
inline int parse_fw_version_arg(const char* args) {
    const char* p = strstr(args, "--fw-version");
    if (!p) return 0;
    p += 12;  // skip "--fw-version"
    while (*p == ' ') ++p;
    return parse_firmware_version(note::string_view(p, strlen(p)));
}

/// Check if --fw-strict is present (fail instead of skip).
inline bool parse_fw_strict_arg(const char* args) {
    return strstr(args, "--fw-strict") != nullptr;
}
