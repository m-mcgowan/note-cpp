#pragma once

/// @file parse.hpp
/// Convenience functions for lexer-based streaming JSON parsing.
///
/// sax_lex_streaming() is the drop-in replacement for sax_parse_streaming()
/// using the new zero-buffer lexer pipeline:
///   HAL → ReadFn → JsonLexer → SaxAdapter → JsonSink

#include <note/lexer/json_lexer.hpp>
#include <note/lexer/sax_adapter.hpp>
#include <note/json_sax_streaming.hpp>  // SaxStreamBuf
#include <note/types.hpp>

namespace note {

/// Parse JSON from a streaming byte source using the lexer pipeline.
/// ReadFn: Result<size_t>(uint8_t* buf, size_t max, uint32_t timeout_ms)
///
/// With caller-provided buffers (key + value scratch for the adapter):
template<typename ReadFn, typename SinkT = JsonSink>
string_view sax_lex_streaming(ReadFn&& read, uint32_t timeout_ms,
                               SaxStreamBuf& buf, SinkT& sink) {
    SaxAdapter<SinkT> adapter(buf, sink);

    DefaultLexer lexer;

    // Read loop: pull bytes, push into lexer
    for (;;) {
        // Read into the SaxStreamBuf's read buffer for chunked I/O
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

        for (size_t i = 0; i < n; ++i) {
            lexer.feed(buf.rbuf[i], [&](LexerEvent ev) {
                adapter.on_event(ev);
            });
            if (lexer.has_error()) {
                return adapter.error() ? adapter.error() : NOTE_ERR("parse error");
            }
            if (lexer.is_done()) return {};
        }
    }
    if (!lexer.is_done()) return NOTE_ERR("incomplete JSON");
    return {};
}

/// Parse with default stack buffers.
template<typename ReadFn, typename SinkT = JsonSink>
string_view sax_lex_streaming(ReadFn&& read, uint32_t timeout_ms, SinkT& sink) {
    char storage[384];  // 64 read + 64 key + 256 value (same default as old parser)
    SaxStreamBuf buf(storage);
    return sax_lex_streaming(std::forward<ReadFn>(read), timeout_ms, buf, sink);
}

/// Parse a complete JSON string (buffer-based, for testing).
template<typename SinkT = JsonSink>
string_view sax_lex(const char* json, size_t len, SinkT& sink) {
    char storage[384];
    SaxStreamBuf buf(storage);
    SaxAdapter<SinkT> adapter(buf, sink);
    DefaultLexer lexer;

    for (size_t i = 0; i < len; ++i) {
        lexer.feed(static_cast<uint8_t>(json[i]), [&](LexerEvent ev) {
            adapter.on_event(ev);
        });
        if (lexer.has_error())
            return adapter.error() ? adapter.error() : NOTE_ERR("parse error");
    }
    if (!lexer.is_done()) return NOTE_ERR("incomplete JSON");
    return {};
}

template<typename SinkT = JsonSink>
string_view sax_lex(string_view json, SinkT& sink) {
    return sax_lex(json.data(), json.size(), sink);
}

} // namespace note
