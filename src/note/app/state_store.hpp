#pragma once

#include <functional>
#include <optional>
#include <tuple>
#include <utility>

namespace note::app {

// StaticStateStore<Types...> — type-indexed observable state store.
//
// Each type in the parameter pack gets one std::optional<T> slot and one
// observer callback. Access is O(1) via std::get on a tuple.
//
// Usage:
//   StaticStateStore<AttentionState, SyncStatus> store;
//   store.on_change<SyncStatus>([](const SyncStatus& s) { ... });
//   store.set(SyncStatus{.syncing = true});
//   auto s = store.get<SyncStatus>();  // -> optional<SyncStatus>

template<typename... Types>
class StaticStateStore {
public:
    template<typename T>
    std::optional<T> get() const {
        return std::get<Slot<T>>(slots_).value;
    }

    template<typename T>
    void set(T value) {
        auto& slot = std::get<Slot<T>>(slots_);
        slot.value = std::move(value);
        if (slot.observer) {
            slot.observer(*slot.value);
        }
    }

    template<typename T>
    void invalidate() {
        std::get<Slot<T>>(slots_).value.reset();
    }

    template<typename T>
    void on_change(std::function<void(const T&)> callback) {
        std::get<Slot<T>>(slots_).observer = std::move(callback);
    }

private:
    template<typename T>
    struct Slot {
        std::optional<T> value;
        std::function<void(const T&)> observer;
    };

    std::tuple<Slot<Types>...> slots_;
};


// NullStateStore — no-op implementation.
// get() always returns nullopt. set(), invalidate(), on_change() are silent.
// Useful as a default when state tracking is not needed.

struct NullStateStore {
    template<typename T>
    std::optional<T> get() const { return std::nullopt; }

    template<typename T>
    void set(T) {}

    template<typename T>
    void invalidate() {}

    template<typename T>
    void on_change(std::function<void(const T&)>) {}
};

} // namespace note::app
