#pragma once

#include <note/api/hub_set.hpp>
#include <note/api/card_location_mode.hpp>
#include <note/app/connection_manager.hpp>
#include <note/app/sync_manager.hpp>
#include <note/app/template_manager.hpp>
#include <note/units.hpp>

#include <optional>
#include <utility>

namespace note::app {

template<typename Channel, typename Store = NullStateStore>
class Setup {
public:
    Setup(Channel& ch, Store& store)
        : ch_(ch), store_(store)
        , conn_(ch, store)
        , templates_(ch, store)
        , sync_(ch, store) {}

    // Fluent configuration.
    Setup& product(string_view p) { product_ = p; return *this; }
    Setup& mode(string_view m) { mode_ = m; return *this; }
    Setup& outbound(Minutes m) { outbound_ = m; return *this; }
    Setup& inbound(Minutes m) { inbound_ = m; return *this; }

    Setup& fixed_location(double lat, double lon) {
        fixed_loc_ = {lat, lon};
        return *this;
    }

    // Enable NTN mode — enforces compact templates, directional sync.
    Setup& ntn() {
        ntn_ = true;
        templates_.set_ntn(true);
        sync_.set_ntn(true);
        return *this;
    }

    // Declare a template (forwarded to Templates).
    Setup& template_(string_view file, BodyValue body,
                     int32_t port = 0, bool compact = false) {
        if (ntn_ && !compact) compact = true;
        templates_.declare(file, std::move(body), port, compact);
        return *this;
    }

    // Execute the full setup sequence:
    //   1. hub.set — mode, product, sync intervals
    //   2. note.template — for each declared template
    //   3. hub.sync — initial sync (NTN: waits for completion)
    //   4. card.location.mode — if fixed location configured
    auto run() -> Result<void> {
        // Step 1: hub.set
        {
            api::HubSet req;
            if (!product_.empty()) req.product(product_);
            if (!mode_.empty()) req.mode(mode_);
            if (outbound_.count > 0) req.outbound(outbound_);
            if (inbound_.count > 0) req.inbound(inbound_);

            auto r = conn_.configure(req);
            if (!r) return r;
        }

        // Step 2: note.template for each declared template
        {
            auto r = templates_.register_all();
            if (!r) return r;
        }

        // Step 3: initial sync
        {
            auto r = sync_.sync();
            if (!r) return r;
        }

        // Step 4: fixed location
        if (fixed_loc_) {
            api::CardLocationMode::Set req;
            req.mode("fixed");
            req.lat(fixed_loc_->first);
            req.lon(fixed_loc_->second);
            auto r = ch_.execute(req);
            if (!r) return Unexpected(r.error());
        }

        return {};
    }

    // Access sub-managers for post-setup use.
    Connection<Channel, Store>& connection() { return conn_; }
    Templates<Channel, Store>& templates() { return templates_; }
    Sync<Channel, Store>& sync() { return sync_; }

private:
    Channel& ch_;
    Store& store_;
    Connection<Channel, Store> conn_;
    Templates<Channel, Store> templates_;
    Sync<Channel, Store> sync_;

    std::string product_;
    std::string mode_;
    Minutes outbound_{};
    Minutes inbound_{};
    bool ntn_ = false;
    std::optional<std::pair<double, double>> fixed_loc_;
};

} // namespace note::app
