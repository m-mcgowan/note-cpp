#pragma once

/// @file parse.hpp
/// Convenience functions for lexer-based streaming JSON parsing.
///
/// sax_lex_streaming() is the drop-in replacement for sax_parse_streaming()
/// using the new zero-buffer lexer pipeline:
///   HAL → ReadFn → JsonLexer → SaxAdapter → SaxDispatch → SinkT

#include <note/lexer/json_lexer.hpp>
#include <note/lexer/sax_adapter.hpp>
#include <note/json_sax_streaming.hpp>  // SaxStreamBuf
#include <note/types.hpp>

namespace note {

namespace detail {

/// Shared feed loop: pushes bytes through the lexer into the adapter.
/// Not templated on SinkT — one instantiation for all sink types.
inline string_view lex_feed_loop(DefaultLexer& lexer, const uint8_t* data,
                                 size_t n, SaxAdapter& adapter) {
    for (size_t i = 0; i < n; ++i) {
        lexer.feed(data[i], adapter);
        if (lexer.has_error())
            return adapter.error() ? adapter.error() : NOTE_ERR("parse error");
        if (lexer.is_done()) return {};
    }
    return {};
}

} // namespace detail

/// Parse JSON from a streaming byte source using the lexer pipeline.
/// ReadFn: Result<size_t>(uint8_t* buf, size_t max, uint32_t timeout_ms)
///
/// With caller-provided buffers (key + value scratch for the adapter):
template<typename ReadFn, typename SinkT = JsonSink>
string_view sax_lex_streaming(ReadFn&& read, uint32_t timeout_ms,
                               SaxStreamBuf& buf, SinkT& sink) {
    auto dispatch = make_sax_dispatch(sink);
    return sax_lex_streaming(std::forward<ReadFn>(read), timeout_ms, buf, dispatch);
}

/// Parse with default stack buffers.
template<typename ReadFn, typename SinkT = JsonSink>
string_view sax_lex_streaming(ReadFn&& read, uint32_t timeout_ms, SinkT& sink) {
    char storage[384];  // 64 read + 64 key + 256 value (same default as old parser)
    SaxStreamBuf buf(storage);
    return sax_lex_streaming(std::forward<ReadFn>(read), timeout_ms, buf, sink);
}

/// SaxDispatch overload — takes a pre-built dispatch table (no per-SinkT template).
/// This is the core implementation; the SinkT overloads above delegate here.
template<typename ReadFn>
string_view sax_lex_streaming(ReadFn&& read, uint32_t timeout_ms,
                               SaxStreamBuf& buf, SaxDispatch dispatch) {
    SaxAdapter adapter(buf, dispatch);
    DefaultLexer lexer;

    for (;;) {
        auto r = read(buf.rbuf, buf.rbuf_size, timeout_ms);
        if (!r) {
            if (lexer.is_done()) break;
            return NOTE_ERR("read error");
        }
        size_t n = *r;
        if (n == 0) {
            if (lexer.is_done()) break;
            return NOTE_ERR("unexpected end of input");
        }

        auto err = detail::lex_feed_loop(lexer, buf.rbuf, n, adapter);
        if (!err.empty() || lexer.is_done()) return err;
    }
    if (!lexer.is_done()) return NOTE_ERR("incomplete JSON");
    return {};
}

/// SaxDispatch overload with default stack buffers.
template<typename ReadFn>
string_view sax_lex_streaming(ReadFn&& read, uint32_t timeout_ms,
                               SaxDispatch dispatch) {
    char storage[384];
    SaxStreamBuf buf(storage);
    return sax_lex_streaming(std::forward<ReadFn>(read), timeout_ms, buf, dispatch);
}

/// Parse a complete JSON string (buffer-based, for testing).
template<typename SinkT = JsonSink>
string_view sax_lex(const char* json, size_t len, SinkT& sink) {
    char storage[384];
    SaxStreamBuf buf(storage);
    auto dispatch = make_sax_dispatch(sink);
    SaxAdapter adapter(buf, dispatch);
    DefaultLexer lexer;

    auto err = detail::lex_feed_loop(lexer, reinterpret_cast<const uint8_t*>(json), len, adapter);
    if (!err.empty()) return err;
    if (!lexer.is_done()) return NOTE_ERR("incomplete JSON");
    return {};
}

template<typename SinkT = JsonSink>
string_view sax_lex(string_view json, SinkT& sink) {
    return sax_lex(json.data(), json.size(), sink);
}

} // namespace note
