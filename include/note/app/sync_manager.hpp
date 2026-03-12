#pragma once

#include <note/api/hub_sync.hpp>
#include <note/api/hub_sync_status.hpp>
#include <note/app/state_store.hpp>

namespace note::app {

struct SyncState {
    std::string status;
    int32_t completed{};
    bool syncing{};
};

template<typename Channel, typename Store = NullStateStore>
class Sync {
public:
    Sync(Channel& ch, Store& store)
        : ch_(ch), store_(store) {}

    // Trigger outbound sync.
    // In NTN mode, sends hub.sync with out:true.
    // In normal mode, sends hub.sync (syncs both directions).
    auto sync_outbound() -> Result<void> {
        api::HubSync req;
        if (ntn_) req.out(true);
        auto r = ch_.execute(req);
        if (!r) return Unexpected(r.error());
        return {};
    }

    // Trigger inbound sync.
    // In NTN mode, sends hub.sync with in:true.
    // In normal mode, sends hub.sync (syncs both directions).
    auto sync_inbound() -> Result<void> {
        api::HubSync req;
        if (ntn_) req.in(true);
        auto r = ch_.execute(req);
        if (!r) return Unexpected(r.error());
        return {};
    }

    // Trigger sync in both directions.
    // In NTN mode, sends two separate requests (out, then in).
    // In normal mode, sends a single hub.sync.
    auto sync() -> Result<void> {
        if (ntn_) {
            auto r = sync_outbound();
            if (!r) return r;
            return sync_inbound();
        }
        auto r = ch_.execute(api::HubSync{});
        if (!r) return Unexpected(r.error());
        return {};
    }

    // Query sync status and update the store.
    auto status() -> ApiResult<api::HubSyncStatus::Response> {
        auto r = ch_.execute(api::HubSyncStatus{});
        if (r) {
            store_.set(SyncState{
                .status = std::string(r.status),
                .completed = r.completed,
                .syncing = r.sync,
            });
        }
        return r;
    }

    // Wait for sync completion by polling hub.sync.status.
    // Returns when status contains "{sync-end}" or on timeout.
    // poll_fn is called between polls to yield (e.g. delay/sleep).
    // Returns Error::Timeout if max_polls is exceeded.
    template<typename PollFn>
    auto wait_for_sync(int max_polls, PollFn poll_fn) -> Result<void> {
        for (int i = 0; i < max_polls; ++i) {
            auto r = status();
            if (!r) return Unexpected(r.error());
            auto s = r.status;
            if (s.find("{sync-end}") != string_view::npos ||
                s.find("completed") != string_view::npos) {
                return {};
            }
            if (i + 1 < max_polls) poll_fn();
        }
        return make_error(Error::ResponseLost, Cause::Timeout, "sync did not complete");
    }

    void set_ntn(bool ntn) { ntn_ = ntn; }
    bool is_ntn() const { return ntn_; }

private:
    Channel& ch_;
    Store& store_;
    bool ntn_ = false;
};

} // namespace note::app
