# Changelog

All notable changes to this project will be documented in this file.
Follows [Keep a Changelog](https://keepachangelog.com/) conventions.

## [Unreleased]

### Added
- `make_api()` factory function and `target<Product>()` / `target<Rat>()` helpers for target-constrained APIs
- `Target<Rat, Strict>` type system — unsupported endpoints get `[[deprecated]]` or are removed via `requires`
- Per-endpoint `static constexpr Skus skus` with RAT bitfield from `x-skus` spec metadata
- `Rat`, `Product`, `Skus`, `Firmware`, `Unconstrained` types in `<note/target.hpp>`
- Compile-fail CI tests for strict-mode rejection of unsupported endpoints
- Raw transport method exposure for channel variants (serial/I2C read/write/available)
- Property extensions sideband with schema source tracking for codegen
- Compile-time required field enforcement with factory functions per endpoint
- C++20 support via `tl::expected` polyfill (GCC 12+, Clang 15+)
- VoltageVariable builder with `x-format` codegen support
- Protocol policy for transport tuning (retry counts, timeouts, chunk sizes)
- Type-safe duration units (seconds, minutes, hours) for time-valued fields
- Native Notecard transport layer — serial and I2C with CRC32, retry, and chunking
- Qt-style property accessors and codegen rewrite for fluent builder API
- JsonBuf constexpr builder for compile-time JSON construction with auto-sizing
- ApiResult dot-access for response field chaining
- NOTE_BODY macro for typed request/response body serialization (C++17 deserialization)
- 74 auto-generated endpoint types from OpenAPI 3.1 spec
- 195 wire-format sample tests from spec examples
- Core abstractions: Notecard coordinator, JsonBackend, Result<T>, Error, Field<T>
- Code generation tooling (Jinja2) with OpenAPI round-trip verification
- Code size report script for binary footprint benchmarking
- Examples: getting started, sending notes, hub configuration, attention pin, location tracking

### Fixed
- 98.5% line / 99.9% function / 98.9% branch coverage with GCC 13 + lcov 2.x
- `set_body()` ambiguity between typed and untyped overloads
- Quote-agnostic grep for version gating strict mode check

### Internal
- Coverage threshold checks and PR summary reporting in CI
- `LCOV_EXCL` markers for consteval functions (non-executable in coverage)
- `.clangd` configuration for IDE diagnostics with C++20
- Pre-push hook and CI improvements
