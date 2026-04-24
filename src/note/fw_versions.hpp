// AUTO-GENERATED from tools/codegen/generate.py — DO NOT EDIT
//
// One `inline constexpr` per unique minimum firmware version observed in
// the Notecard API spec (operation, property, and enum-value thresholds
// alike). Use at Notecard construction sites to declare the minimum
// firmware the device is guaranteed to be running:
//
//   note::arduino::Notecard nc(note::sku::NOTE_ESP, note::fw::v7_5_1);
//
// Note: enum-value-level thresholds (e.g. new `card.aux` modes added in
// 9.3.1) are emitted here so the constant is nameable, but codegen does
// not yet filter specific enum values against a declared constraint.
// That filtering is a separate codegen task; the constant is correct
// regardless.
//
// Naming: `v{major}_{minor}` when patch is zero, else `v{major}_{minor}_{patch}`.

#pragma once

#include <note/target.hpp>

namespace note::fw {

inline constexpr auto v3_2_1 = FwConstraint<3, 2, 1>{};
inline constexpr auto v3_3_1 = FwConstraint<3, 3, 1>{};
inline constexpr auto v3_4_1 = FwConstraint<3, 4, 1>{};
inline constexpr auto v3_5_1 = FwConstraint<3, 5, 1>{};
inline constexpr auto v4_1_1 = FwConstraint<4, 1, 1>{};
inline constexpr auto v5_1_1 = FwConstraint<5, 1, 1>{};
inline constexpr auto v5_3_1 = FwConstraint<5, 3, 1>{};
inline constexpr auto v6_1_1 = FwConstraint<6, 1, 1>{};
inline constexpr auto v6_2_3 = FwConstraint<6, 2, 3>{};
inline constexpr auto v7_2_1 = FwConstraint<7, 2, 1>{};
inline constexpr auto v7_2_2 = FwConstraint<7, 2, 2>{};
inline constexpr auto v7_3_1 = FwConstraint<7, 3, 1>{};
inline constexpr auto v7_4_1 = FwConstraint<7, 4, 1>{};
inline constexpr auto v7_5_1 = FwConstraint<7, 5, 1>{};
inline constexpr auto v7_5_2 = FwConstraint<7, 5, 2>{};
inline constexpr auto v8_1_3 = FwConstraint<8, 1, 3>{};
inline constexpr auto v8_2_1 = FwConstraint<8, 2, 1>{};
inline constexpr auto v9_1_1 = FwConstraint<9, 1, 1>{};
inline constexpr auto v9_3_1 = FwConstraint<9, 3, 1>{};

} // namespace note::fw
