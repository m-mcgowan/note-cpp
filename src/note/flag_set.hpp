#pragma once

/// Compact bitfield for comma-separated flag strings.
///
/// Stores flags as a uint32_t bitmask; serializes to comma-delimited
/// string on demand.  Each flag maps a bit position to a wire name.
///
///   static constexpr note::FlagDef defs[] = {
///       {1u << 0, "arm"}, {1u << 1, "connected"}, {1u << 2, "files"},
///   };
///   note::FlagSet<std::size(defs), 32> fs{defs};
///   fs.add(1u << 0).add(1u << 1);   // "arm,connected"
///
/// BufSize should be large enough for all flag names plus commas.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace note {

struct FlagDef {
    uint32_t bit;
    std::string_view name;
};

/// @tparam N       Number of flag definitions.
/// @tparam BufSize Size of the internal serialization buffer.
template<std::size_t N, std::size_t BufSize>
class FlagSet {
public:
    constexpr FlagSet(const FlagDef (&defs)[N]) : defs_(&defs[0]) {}

    FlagSet& add(uint32_t bit) {
        bits_ |= bit;
        rebuild();
        return *this;
    }

    FlagSet& operator|=(uint32_t bits) {
        bits_ |= bits;
        rebuild();
        return *this;
    }

    FlagSet& set(uint32_t bits) {
        bits_ = bits;
        rebuild();
        return *this;
    }

    FlagSet& clear() {
        bits_ = 0;
        len_ = 0;
        return *this;
    }

    std::string_view str() const { return {buf_, len_}; }
    operator std::string_view() const { return str(); }
    explicit operator bool() const { return bits_ != 0; }
    uint32_t bits() const { return bits_; }

private:
    void rebuild() {
        len_ = 0;
        for (std::size_t i = 0; i < N; ++i) {
            if (!(bits_ & defs_[i].bit)) continue;
            if (len_ > 0 && len_ < BufSize) buf_[len_++] = ',';
            for (char c : defs_[i].name) {
                if (len_ >= BufSize) break;
                buf_[len_++] = c;
            }
        }
    }

    const FlagDef* defs_;
    uint32_t bits_{};
    char buf_[BufSize]{};
    std::size_t len_{};
};

} // namespace note
