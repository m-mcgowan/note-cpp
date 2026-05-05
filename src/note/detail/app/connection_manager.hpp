#pragma once

#include <note/api/hub_set.hpp>
#include <note/api/hub_status.hpp>
#include <note/detail/app/state_store.hpp>

namespace note::detail::app {

struct ConnectionState {
    bool connected{};
    std::string status;
};

template<typename Channel, typename Store = NullStateStore>
class Connection {
public:
    Connection(Channel& ch, Store& store)
        : ch_(ch), store_(store) {}

    // Send hub.set with the given configuration.
    auto configure(const api::HubSet& config) -> Result<void> {
        auto r = ch_.execute(config);
        if (!r) return Unexpected(r.error());
        return {};
    }

    // Query hub.status and update the store.
    auto status() -> ApiResult<api::HubStatus::Response> {
        auto r = ch_.execute(api::HubStatus{});
        if (r) {
            store_.set(ConnectionState{
                .connected = r.connected,
                .status = std::string(r.status),
            });
        }
        return r;
    }

    // Check connectivity (reads from store if available, else queries).
    auto is_connected() -> Result<bool> {
        if (auto s = store_.template get<ConnectionState>()) {
            return s->connected;
        }
        auto r = status();
        if (!r) return Unexpected(r.error());
        return r.connected;
    }

private:
    Channel& ch_;
    Store& store_;
};

} // namespace note::detail::app
