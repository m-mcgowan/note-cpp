// note-c bridge transport — incremental migration pattern.
//
// Projects that already use note-c (or note-arduino) can adopt note-cpp's
// typed API without replacing the transport layer. The bridge implements
// IBufferedTransport by delegating each request to note-c's
// NoteRequestResponseJSON(). Both libraries share the single underlying
// Notecard connection — no hardware conflicts, and existing J* code keeps
// working alongside new typed-API code.
//
// This example stubs NoteRequestResponseJSON so it compiles without note-c
// installed. In a real project, link against note-c and delete the stub.

#include "mock_backend.hpp"
#include <note/api.hpp>
#include <note/notecard.hpp>
#include <note/transport.hpp>

#include <cstdlib>
#include <cstring>
#include <string>

// readme:bridge-extern
extern "C" char* NoteRequestResponseJSON(const char* reqJSON);
// readme:end

// Stub — delete when linking against note-c.
extern "C" char* NoteRequestResponseJSON(const char*) {
    char* rsp = static_cast<char*>(std::malloc(3));
    std::strcpy(rsp, "{}");
    return rsp;
}

// readme:bridge-transport
/// Delegates every request to note-c's NoteRequestResponseJSON so note-c
/// owns the serial/I2C bus and note-cpp sits on top with its typed API.
class NoteCTransport : public note::IBufferedTransport {
    std::string rsp_buf_;
public:
    note::Result<note::string_view> transact(note::string_view req, uint32_t) override {
        std::string req_str(req.data(), req.size());
        char* rsp = NoteRequestResponseJSON(req_str.c_str());
        if (rsp == nullptr) {
            return note::make_error(note::Error::ResponseLost, "no response");
        }
        rsp_buf_ = rsp;
        std::free(rsp);
        return note::string_view(rsp_buf_);
    }
    note::Result<void> send(note::string_view req) override {
        auto r = transact(req, 0);
        if (!r) return note::Unexpected(r.error());
        return {};
    }
    void reset() override {}
    void abort() override {}

    // Minimal Hal stub — note-c owns the actual hardware, so the bridge's
    // Hal is purely a placeholder so the inherited Notecard timing path
    // has something valid to call. Returning 0/no-op is safe because all
    // wire bytes go through NoteRequestResponseJSON above.
    struct NoopHal : note::Hal {
        bool transmit(const uint8_t*, size_t) override { return true; }
        note::Result<size_t> read(uint8_t*, size_t, uint32_t) override { return note::Result<size_t>{size_t{0}}; }
        bool reset() override { return true; }
        bool write_line_terminator() override { return true; }
        uint32_t millis() override { return 0; }
        void delay(uint32_t) override {}
    } hal_;
    note::Hal& hal() override { return hal_; }
};
// readme:end

// readme:bridge-wiring
int main() {
    MockBackend backend;
    NoteCTransport transport;
    note::Notecard nc(backend, transport);
    note::Api api(nc);

    // Typed API calls route through note-c's existing transport.
    api.hub.set().product("com.example.app").mode("periodic").execute();
    return 0;
}
// readme:end
