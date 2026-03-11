#pragma once

#include "json.hpp"
#include "safety.hpp"

#include <functional>
#include <string>

namespace note {

class Notecard {
public:
    // Transport callables:
    //   RequestFn: send a JSON request string, receive a JSON response string.
    //   SendFn:    send a JSON command string (fire-and-forget, no response).
    using RequestFn = std::function<Result<std::string>(string_view request, uint32_t timeout_ms)>;
    using SendFn    = std::function<Result<void>(string_view request)>;

    Notecard(JsonBackend& backend, RequestFn request_fn, SendFn send_fn = {})
        : backend_(backend)
        , request_fn_(std::move(request_fn))
        , send_fn_(std::move(send_fn))
    {
        // If no send function provided, derive one from request (discard response).
        if (!send_fn_) {
            send_fn_ = [this](string_view req) -> Result<void> {
                auto r = request_fn_(req, default_timeout_ms_);
                if (!r) return Unexpected(r.error());
                return {};
            };
        }
    }

    // Execute a typed, generated request.
    // RequestT must provide:
    //   static constexpr string_view notecard_request;
    //   static constexpr bool supports_cmd;
    //   static constexpr Safety safety;
    //   using Response = ...;
    //   void build(JsonBuilder&) const;
    template<typename RequestT>
    ApiResult<typename RequestT::Response> execute(const RequestT& req) {
        auto builder = backend_.create_builder();
        builder->add("req", RequestT::notecard_request);
        req.build(*builder);

        auto rsp = request_fn_(builder->to_string(), default_timeout_ms_);
        if (!rsp) return Unexpected(rsp.error());

        auto reader = backend_.parse_response(*rsp);
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

        auto rsp = request_fn_(builder->to_string(), default_timeout_ms_);
        if (!rsp) return Unexpected(rsp.error());

        auto reader = backend_.parse_response(*rsp);
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
        return send_fn_(builder->to_string());
    }

    // Fire-and-forget command.
    Result<void> command(string_view cmd_type,
                         std::function<void(JsonBuilder&)> build_fn = {}) {
        auto builder = backend_.create_builder();
        builder->add("cmd", cmd_type);
        if (build_fn) build_fn(*builder);
        return send_fn_(builder->to_string());
    }

    void set_default_timeout(uint32_t ms) { default_timeout_ms_ = ms; }
    uint32_t default_timeout() const { return default_timeout_ms_; }

    // Raw transport access — used by note-cpp-app channel variants.
    Result<std::string> transact(string_view json, uint32_t timeout_ms) {
        return request_fn_(json, timeout_ms);
    }
    Result<void> send(string_view json) {
        return send_fn_(json);
    }

    JsonBackend& backend() { return backend_; }

private:
    JsonBackend& backend_;
    RequestFn request_fn_;
    SendFn send_fn_;
    uint32_t default_timeout_ms_ = 10000;
};

} // namespace note
