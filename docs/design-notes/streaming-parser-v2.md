# Streaming Parser v2 — Zero-Buffer Architecture

Contributor documentation for the next-generation streaming JSON parser.

## Terminology

- **JsonLexer**: low-level, zero-buffer, push-based. Consumes bytes,
  emits `LexerEvent` via callback. Validates JSON grammar.
- **SaxParser**: high-level, optional buffers. Consumes `LexerEvent`
  stream, accumulates keys/values, delivers `JsonSink` callbacks.
- **JsonSink**: application-level interface (unchanged from today).

## Problem

The current `StreamingSaxParser` combines lexing and parsing into one
pull-based class that requires three fixed buffers (384 bytes default):

- **Read window** (64 bytes): sliding window over HAL reads
- **Key scratch** (64 bytes): accumulates full key before dispatch
- **Value scratch** (256 bytes): accumulates full value before delivery

These impose artificial length limits (silent truncation), waste memory
on small responses, and are insufficient for large payloads (e.g.
`dfu.get` returns 4KB base64 chunks).

## Design Principle

All buffers are **performance optimizations**, not correctness
requirements. A zero-buffer configuration must work correctly. The
developer controls the memory trade-off.

## Architecture

### JsonLexer (zero buffers, ~20 bytes state)

A push-based state machine. The caller feeds one byte at a time; the
lexer validates JSON grammar and emits events via callback. Multiple
events may fire from a single byte (e.g. `}` after a number terminates
the number AND closes the object).

```cpp
lexer.feed(byte, [](LexerEvent ev) {
    // handle event
});
```

**Events:**

```cpp
struct LexerEvent {
    enum Tag : uint8_t {
        None,           // internal — not emitted
        ObjectBegin,    // {
        ObjectEnd,      // }
        ArrayBegin,     // [
        ArrayEnd,       // ]
        KeyChar,        // one decoded key character
        KeyEnd,         // end of key (before colon)
        StringChar,     // one decoded value string character
        StringEnd,      // end of string value
        Integer,        // complete integer value
        Float,          // complete floating-point value
        Bool,           // true or false
        Null,           // null
        Error,          // parse error
    } tag;

    union {
        char ch;            // KeyChar, StringChar
        int32_t integer;    // Integer
        double floating;    // Float
        bool boolean;       // Bool
        const char* error;  // Error — static string, not allocated
    };
};
```

### Templated strategies

The lexer is composed from independently testable strategy types.
Templates ensure zero overhead — each strategy inlines completely.

```cpp
template<typename Stack, typename NumberParser, typename EscapeDecoder>
class JsonLexer {
    State state_;           // grammar state enum
    Stack stack_;           // nesting validation
    NumberParser number_;   // incremental number parsing
    EscapeDecoder escape_;  // \n, \uXXXX → UTF-8
    // ...
};
```

**Stack** — validates nesting, tracks object vs array at each level:

```cpp
template<typename Word = uint32_t>
struct BitStack {
    Word bits = 0;
    uint8_t depth = 0;

    static constexpr uint8_t max_depth = sizeof(Word) * 8;

    bool push_object();     // push 1, return false on overflow
    bool push_array();      // push 0, return false on overflow
    bool pop_object();      // verify top is 1, pop, return false on mismatch
    bool pop_array();       // verify top is 0, pop, return false on mismatch
    bool empty() const;
    bool in_object() const; // top bit is 1
    bool in_array() const;  // top bit is 0
};
```

- Production: `BitStack<uint32_t>` — 32 levels, 5 bytes
- Deep nesting: `BitStack<uint64_t>` — 64 levels
- Test: verify push/pop sequences, overflow, mismatch detection

**NumberParser** — builds numbers incrementally from digits:

```cpp
struct IncrementalNumber {
    int32_t integer_acc = 0;
    double float_acc = 0.0;
    double frac_divisor = 1.0;
    int16_t exp_acc = 0;
    int8_t sign = 1;
    int8_t exp_sign = 1;
    bool is_float = false;
    bool has_digits = false;

    void reset();
    void set_negative();
    void add_digit(uint8_t d);     // integral part
    void start_fraction();         // saw '.'
    void add_frac_digit(uint8_t d);
    void start_exponent();         // saw 'e'/'E'
    void set_exp_negative();
    void add_exp_digit(uint8_t d);

    bool is_integer() const;
    int32_t to_integer() const;    // apply sign
    double to_float() const;       // apply sign, fraction, exponent
};
```

- Testable in isolation: feed digit sequences, verify results
- Edge cases: leading minus, zero, overflow, max int32, tiny fractions
- No buffer — pure accumulator state

**EscapeDecoder** — decodes JSON string escapes:

```cpp
struct Utf8EscapeDecoder {
    uint16_t unicode_acc = 0;
    uint8_t hex_remaining = 0;

    void reset();

    // Feed one character after backslash. Calls emit(char) for each
    // decoded byte (1 byte for \n, up to 3 for \uXXXX UTF-8).
    template<typename EmitFn>
    bool feed(char c, EmitFn emit);  // returns false on invalid escape

    // Feed hex digit for \uXXXX. Calls emit when complete.
    template<typename EmitFn>
    bool feed_hex(char c, EmitFn emit);
};
```

- Testable: `\n` → `'\n'`, `\\` → `'\\'`, `\u0041` → `'A'`,
  `\u00E9` → 2-byte UTF-8, `\u4E16` → 3-byte UTF-8
- Surrogate pairs (`\uD800\uDC00`) → replacement char or error

**Default lexer type:**

```cpp
using DefaultLexer = JsonLexer<BitStack<uint32_t>, IncrementalNumber, Utf8EscapeDecoder>;
```

### SaxParser (high-level, optional buffers)

Consumes `LexerEvent` stream. Accumulates `KeyChar` events into a
key buffer and `StringChar` events into a value buffer, then delivers
`on_string(key, value)` etc. to a `JsonSink`.

Buffer sizes controlled by the developer:

```cpp
char buf[512];
SaxStreamBuf sbuf(buf);
SaxParser parser(sbuf, sink);
// feed LexerEvents from the lexer...
```

This is the compatibility layer for existing `JsonSink` implementations
and codegen'd response sinks.

### Codegen'd sinks

Generated response sinks can target either level:

| Mode | Buffers | Key matching | Value delivery | Best for |
|------|---------|-------------|----------------|----------|
| SaxParser + JsonSink | Developer-sized | Full string compare | Complete (key, value) | Simple, moderate memory |
| LexerEvent + trie | Zero | Incremental trie walk | Character stream | Minimum memory |

### Parse loop

The full streaming pipeline:

```cpp
DefaultLexer lexer;
SaxParser parser(buf, sink);

while (!frame_done) {
    uint8_t byte;
    auto r = frame_read(&byte, 1, timeout);
    if (!r || *r == 0) break;

    lexer.feed(byte, [&](LexerEvent ev) {
        parser.on_event(ev);
    });
}
```

With optional read buffer for throughput:

```cpp
uint8_t rbuf[64];  // developer-provided
while (!frame_done) {
    auto r = frame_read(rbuf, sizeof(rbuf), timeout);
    if (!r) break;
    for (size_t i = 0; i < *r; ++i) {
        lexer.feed(rbuf[i], [&](LexerEvent ev) {
            parser.on_event(ev);
        });
    }
}
```

### Body capture

With the lexer, body capture receives structural and character events
incrementally. The sink serializes them into a caller-provided buffer.
No `std::string` dependency.

### Error handling

- Lexer emits `LexerEvent::Error` with a static error string
- Lexer stops after error (subsequent `feed()` calls are no-ops)
- SaxParser propagates error to caller
- Frame drain still runs after parse error to clean the wire

## Migration path

1. Implement strategies: `BitStack`, `IncrementalNumber`, `Utf8EscapeDecoder`
2. Implement `JsonLexer` — templated on strategies, unit tested
3. Implement `SaxParser` adapter — drives `JsonSink` from `LexerEvent`
4. Wire into `Protocol::receive_streaming()` as alternative
5. Codegen compatible with both old and new parser
6. Current `StreamingSaxParser` remains for backward compatibility
7. Future: codegen low-level trie sinks for zero-buffer endpoints
