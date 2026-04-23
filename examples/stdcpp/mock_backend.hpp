// Mock JSON backend for examples.
//
// In production, you'd use a real JSON library (cJSON, nlohmann-json,
// RapidJSON, etc.) wrapped in note-cpp's JsonBackend interface. This mock
// builds valid JSON for requests and returns empty responses — enough to
// demonstrate the API without any external dependencies.
//
// See tests/integration/ for real backend examples (cJSON, nlohmann, jsmn).

#pragma once

#include <note/notecard.hpp>
#include <memory>
#include <string>

struct MockBuilder : note::JsonBuilder {
    using JsonBuilder::add;
    using JsonBuilder::add_element;

    std::string buf_ = "{";
    bool first_ = true;
    void sep() { if (!first_) buf_ += ','; first_ = false; }

    MockBuilder& add(note::string_view k, bool v) override {
        sep(); buf_ += '"'; buf_ += k; buf_ += "\":"; buf_ += v ? "true" : "false"; return *this;
    }
    MockBuilder& add(note::string_view k, note::json_int_t v) override {
        sep(); buf_ += '"'; buf_ += k; buf_ += "\":"; buf_ += std::to_string(v); return *this;
    }
    MockBuilder& add(note::string_view k, double v) override {
        sep(); buf_ += '"'; buf_ += k; buf_ += "\":"; buf_ += std::to_string(v); return *this;
    }
    MockBuilder& add(note::string_view k, note::string_view v) override {
        sep(); buf_ += '"'; buf_ += k; buf_ += "\":\""; buf_ += v; buf_ += '"'; return *this;
    }
    MockBuilder& add_raw(note::string_view k, note::string_view json_fragment) override {
        sep(); buf_ += '"'; buf_ += k; buf_ += "\":"; buf_ += json_fragment; return *this;
    }
    MockBuilder& begin_object(note::string_view k) override {
        sep(); buf_ += '"'; buf_ += k; buf_ += "\":{"; first_ = true; return *this;
    }
    MockBuilder& end_object() override { buf_ += '}'; first_ = false; return *this; }
    MockBuilder& begin_array(note::string_view k) override {
        sep(); buf_ += '"'; buf_ += k; buf_ += "\":["; first_ = true; return *this;
    }
    MockBuilder& end_array() override { buf_ += ']'; first_ = false; return *this; }

    // Array element serialization — the JsonBuilder defaults are no-ops, so
    // without these overrides every ArrayField serializes as `[]`.
    MockBuilder& add_element(bool v) override {
        sep(); buf_ += v ? "true" : "false"; return *this;
    }
    MockBuilder& add_element(note::json_int_t v) override {
        sep(); buf_ += std::to_string(v); return *this;
    }
    MockBuilder& add_element(double v) override {
        sep(); buf_ += std::to_string(v); return *this;
    }
    MockBuilder& add_element(note::string_view v) override {
        sep(); buf_ += '"'; buf_ += v; buf_ += '"'; return *this;
    }

    note::string_view to_view() override { buf_ += '}'; return buf_; }
};

struct MockReader : note::JsonReader {
    bool has(note::string_view) const override { return false; }
    bool get_bool(note::string_view, bool d) const override { return d; }
    note::json_int_t get_int(note::string_view, note::json_int_t d) const override { return d; }
    double get_double(note::string_view, double d) const override { return d; }
    note::string_view get_string(note::string_view, note::string_view d) const override { return d; }
    std::unique_ptr<note::JsonReader> get_object(note::string_view) const override { return nullptr; }
    bool has_error() const override { return false; }
    note::string_view get_error() const override { return {}; }
};

struct MockBackend : note::JsonBackend {
    std::unique_ptr<note::JsonBuilder> create_builder() override {
        return std::make_unique<MockBuilder>();
    }
    std::unique_ptr<note::JsonReader> parse_response(note::string_view) override {
        return std::make_unique<MockReader>();
    }
};

/// Mock transport for examples — returns empty JSON responses.
struct MockTransport : note::ITransport {
    std::string last_request;
    std::string response = "{}";

    note::Result<note::string_view> transact(note::string_view request, uint32_t) override {
        last_request.assign(request.data(), request.size());
        return note::string_view(response);
    }
    note::Result<void> send(note::string_view request) override {
        last_request.assign(request.data(), request.size());
        return {};
    }
    void reset() override {}
    void abort() override {}
    uint32_t millis() override { return 0; }
    void delay(uint32_t) override {}
};

