// Target filtering: shows how constrained Api targets provide
// compile-time feedback when using endpoints unsupported by your hardware.
//
// Build: clang++ -std=c++20 -fsyntax-only -I include examples/target_filtering.cpp

#include <note/api.hpp>

// Mock backend for compilation testing (same as smoke.cpp)
struct MockBuilder : note::JsonBuilder {
    MockBuilder& add(note::string_view, bool) override { return *this; }
    MockBuilder& add(note::string_view, int32_t) override { return *this; }
    MockBuilder& add(note::string_view, double) override { return *this; }
    MockBuilder& add(note::string_view, note::string_view) override { return *this; }
    MockBuilder& begin_object(note::string_view) override { return *this; }
    MockBuilder& end_object() override { return *this; }
    MockBuilder& begin_array(note::string_view) override { return *this; }
    MockBuilder& end_array() override { return *this; }
    std::string to_string() override { return "{}"; }
};

struct MockReader : note::JsonReader {
    bool has(note::string_view) const override { return false; }
    bool get_bool(note::string_view, bool def) const override { return def; }
    int32_t get_int(note::string_view, int32_t def) const override { return def; }
    double get_double(note::string_view, double def) const override { return def; }
    note::string_view get_string(note::string_view, note::string_view def) const override { return def; }
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

int main() {
    MockBackend backend;
    note::Notecard nc(backend,
        [](note::string_view, uint32_t) -> note::Result<note::string_view> {
            return note::string_view("{}");
        });

    // --- 1. Unconstrained API: all endpoints available ---
    note::Api api(nc);
    api.execute(api.card.sleep());    // OK: WiFi-only, but unconstrained
    api.execute(api.hub.set());       // OK: universal

#if __cplusplus >= 202002L
    // --- 2. WiFi target: WiFi-specific endpoints allowed ---
    note::Api wifi_api(nc, note::target<note::Product::WiFi>());
    wifi_api.execute(wifi_api.card.sleep());   // OK: WiFi supports card.sleep
    wifi_api.execute(wifi_api.card.wifi());    // OK: WiFi supports card.wifi
    wifi_api.execute(wifi_api.hub.set());      // OK: universal

    // --- 3. Skylo target with custom RAT composition ---
    // Product::Cell + Rat::Ntn gives Cell|Ntn — same RATs as a custom combo
    constexpr auto skylo_rats = note::Product::Cell + note::Rat::Ntn;
    note::Api skylo_api(nc, note::target<note::Product::Skylo>());
    skylo_api.execute(skylo_api.card.wifi());  // OK: Skylo has WiFi RAT
    skylo_api.execute(skylo_api.hub.set());    // OK: universal

    // --- 4. Endpoint SKU introspection ---
    static_assert(note::api::CardSleep::skus.supports(note::Rat::WiFi));
    static_assert(!note::api::CardSleep::skus.supports(note::Rat::LoRa));
    static_assert(note::api::HubSet::skus.supports(note::Rat::LoRa)); // universal

    (void)skylo_rats;
#endif
}
