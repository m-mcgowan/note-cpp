#pragma once

#include "io.hpp"
#include "json.hpp"
#include "safety.hpp"

namespace note {

class Notecard {
public:
    Notecard(JsonBackend& backend, NotecardIO& io)
        : backend_(backend), io_(io) {}

    // Execute a typed, generated request.
    // RequestT must provide:
    //   static constexpr string_view notecard_request;
    //   static constexpr bool supports_cmd;
    //   static constexpr Safety safety;
    //   using Response = ...;
    //   void build(JsonBuilder&) const;
    template<typename RequestT>
    Result<typename RequestT::Response> execute(const RequestT& req) {
        auto builder = backend_.create_builder();
        builder->add("req", RequestT::notecard_request);
        req.build(*builder);

        auto raw_rsp = io_.request_response(builder->release(), default_timeout_ms_);
        if (!raw_rsp) return Unexpected(raw_rsp.error());

        auto reader = backend_.wrap_response(*raw_rsp);
        if (reader->has_error()) {
            return make_error(Error::Protocol, reader->get_error());
        }
        return RequestT::Response::parse(std::move(reader));
    }

    // Ad-hoc request with a builder callback.
    Result<std::unique_ptr<JsonReader>> request(
            string_view req_type,
            std::function<void(JsonBuilder&)> build_fn = {}) {
        auto builder = backend_.create_builder();
        builder->add("req", req_type);
        if (build_fn) build_fn(*builder);

        auto raw_rsp = io_.request_response(builder->release(), default_timeout_ms_);
        if (!raw_rsp) return Unexpected(raw_rsp.error());

        auto reader = backend_.wrap_response(*raw_rsp);
        if (reader->has_error()) {
            return make_error(Error::Protocol, reader->get_error());
        }
        return reader;
    }

    // Fire-and-forget typed command (generated request types).
    template<typename RequestT>
    Result<void> command_typed(const RequestT& req) {
        auto builder = backend_.create_builder();
        builder->add("cmd", RequestT::notecard_request);
        req.build(*builder);
        return io_.send(builder->release());
    }

    // Fire-and-forget command.
    Result<void> command(string_view cmd_type,
                         std::function<void(JsonBuilder&)> build_fn = {}) {
        auto builder = backend_.create_builder();
        builder->add("cmd", cmd_type);
        if (build_fn) build_fn(*builder);

        return io_.send(builder->release());
    }

    void set_default_timeout(uint32_t ms) { default_timeout_ms_ = ms; }

    JsonBackend& backend() { return backend_; }
    NotecardIO& io() { return io_; }

private:
    JsonBackend& backend_;
    NotecardIO& io_;
    uint32_t default_timeout_ms_ = 10000;
};

} // namespace note
