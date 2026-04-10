#pragma once
/// @file json_fmt.hpp
/// json_fmt — compile-time validated JSON templates with runtime values.
///
///   nc.note.add().file("sensors.qo")
///       .body(note::json_fmt<R"({"temp":{},"name":{}})">(22.5f, "sensor-1").view())
///       .execute();

#include "json_validate.hpp"
#include "json_buf.hpp"

#include <cstddef>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#if __cplusplus >= 202002L

namespace note {

namespace detail {

template<std::size_t N>
struct FixedString {
    char data[N]{};
    constexpr FixedString(const char (&s)[N]) {
        for (std::size_t i = 0; i < N; ++i) data[i] = s[i];
    }
    constexpr std::string_view view() const { return {data, N - 1}; }
    constexpr std::size_t size() const { return N - 1; }
};

enum class PlaceholderType : char { Any, Int, Float, String, Bool };

consteval std::size_t count_placeholders(std::string_view fmt) {
    std::size_t count = 0;
    for (std::size_t i = 0; i + 1 < fmt.size(); ++i) {
        if (fmt[i] == '{') {
            if (fmt[i + 1] == '}') { ++count; ++i; }
            else if (i + 2 < fmt.size() && fmt[i + 2] == '}') { ++count; i += 2; }
        }
    }
    return count;
}

consteval PlaceholderType placeholder_type(std::string_view fmt, std::size_t n) {
    std::size_t count = 0;
    for (std::size_t i = 0; i + 1 < fmt.size(); ++i) {
        if (fmt[i] == '{') {
            if (fmt[i + 1] == '}') {
                if (count == n) return PlaceholderType::Any;
                ++count; ++i;
            } else if (i + 2 < fmt.size() && fmt[i + 2] == '}') {
                if (count == n) {
                    switch (fmt[i + 1]) {
                    case 'i': return PlaceholderType::Int;
                    case 'f': return PlaceholderType::Float;
                    case 's': return PlaceholderType::String;
                    case 'b': return PlaceholderType::Bool;
                    default: break;
                    }
                }
                ++count; i += 2;
            }
        }
    }
    return PlaceholderType::Any;
}

consteval bool validate_fmt_structure(std::string_view fmt) {
    char buf[1024]{};
    std::size_t out = 0;
    bool in_str = false;
    for (std::size_t i = 0; i < fmt.size(); ++i) {
        if (!in_str && fmt[i] == '{' && i + 1 < fmt.size()) {
            if (fmt[i + 1] == '}') {
                if (out < sizeof(buf) - 1) buf[out++] = '0';
                ++i; continue;
            }
            if (i + 2 < fmt.size() && fmt[i + 2] == '}') {
                char t = fmt[i + 1];
                if (t == 's') {
                    if (out + 2 < sizeof(buf)) { buf[out++] = '"'; buf[out++] = 'x'; buf[out++] = '"'; }
                } else if (t == 'i' || t == 'f' || t == 'b') {
                    if (out < sizeof(buf) - 1) buf[out++] = '0';
                } else { return false; }
                i += 2; continue;
            }
        }
        if (fmt[i] == '"' && (i == 0 || fmt[i - 1] != '\\')) in_str = !in_str;
        if (out < sizeof(buf) - 1) buf[out++] = fmt[i];
    }
    JsonValidator v;
    return v.validate_object(std::string_view(buf, out));
}

template<typename T>
consteval bool check_type(PlaceholderType pt) {
    using U = std::remove_cvref_t<T>;
    switch (pt) {
    case PlaceholderType::Any: return true;
    case PlaceholderType::Int: return std::is_integral_v<U> && !std::is_same_v<U, bool>;
    case PlaceholderType::Float: return std::is_floating_point_v<U>;
    case PlaceholderType::String: return std::is_convertible_v<U, std::string_view>;
    case PlaceholderType::Bool: return std::is_same_v<U, bool>;
    }
    return false;
}

template<typename... Args, std::size_t... Is>
consteval bool check_all_types([[maybe_unused]] std::string_view fmt, std::index_sequence<Is...>) {
    return (check_type<Args>(placeholder_type(fmt, Is)) && ...);
}

} // namespace detail

/// Result of json_fmt — owns a fixed-size buffer with the rendered JSON.
/// Use .view() to pass to body(). The temporary must live through execute().
template<std::size_t BufSize>
struct JsonFmtResult {
    char buf[BufSize]{};
    std::size_t len = 0;

    void put(char c) { if (len < BufSize - 1) buf[len++] = c; }
    void puts(std::string_view s) { for (char c : s) put(c); }

    std::string_view view() const { return {buf, len}; }
};

/// Compile-time validated JSON format with runtime value substitution.
template<detail::FixedString Fmt, typename... Args>
auto json_fmt(Args&&... args) {
    constexpr auto fmt = Fmt.view();

    static_assert(detail::count_placeholders(fmt) == sizeof...(Args),
                  "json_fmt: placeholder count does not match argument count");
    static_assert(detail::validate_fmt_structure(fmt),
                  "json_fmt: invalid JSON structure or not a top-level object");
    static_assert(detail::check_all_types<Args...>(
                      fmt, std::index_sequence_for<Args...>{}),
                  "json_fmt: argument type does not match placeholder type");

    constexpr std::size_t buf_size = fmt.size() + sizeof...(Args) * 32 + 1;
    JsonFmtResult<buf_size> result;

    auto values = std::tuple(std::forward<Args>(args)...);
    std::size_t arg_idx = 0;
    bool in_string = false;

    for (std::size_t i = 0; i < fmt.size(); ++i) {
        if (!in_string && fmt[i] == '{' && i + 1 < fmt.size()) {
            std::size_t skip = 0;
            if (fmt[i + 1] == '}') skip = 2;
            else if (i + 2 < fmt.size() && fmt[i + 2] == '}') skip = 3;

            if (skip > 0) {
                std::apply([&](auto&&... all) {
                    std::size_t idx = 0;
                    [[maybe_unused]] auto emit = [&](auto&& v) {
                        if (idx++ != arg_idx) return;
                        using V = std::remove_cvref_t<decltype(v)>;
                        if constexpr (std::is_same_v<V, bool>) {
                            result.puts(v ? "true" : "false");
                        } else if constexpr (std::is_integral_v<V>) {
                            char tmp[12];
                            auto n = detail::itoa(tmp, sizeof(tmp), static_cast<int32_t>(v));
                            for (std::size_t j = 0; j < n; ++j) result.put(tmp[j]);
                        } else if constexpr (std::is_floating_point_v<V>) {
                            char tmp[32];
                            auto n = detail::dtoa(tmp, sizeof(tmp), static_cast<double>(v));
                            for (std::size_t j = 0; j < n; ++j) result.put(tmp[j]);
                        } else if constexpr (std::is_convertible_v<V, std::string_view>) {
                            std::string_view sv(v);
                            result.put('"');
                            for (char c : sv) {
                                if (c == '"') { result.put('\\'); result.put('"'); }
                                else if (c == '\\') { result.put('\\'); result.put('\\'); }
                                else result.put(c);
                            }
                            result.put('"');
                        }
                    };
                    (emit(all), ...);
                }, values);
                ++arg_idx;
                i += skip - 1;
                continue;
            }
        }
        if (fmt[i] == '"' && (i == 0 || fmt[i - 1] != '\\')) in_string = !in_string;
        result.put(fmt[i]);
    }

    return result;
}

} // namespace note

#endif // C++20
