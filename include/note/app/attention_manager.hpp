#pragma once

#include <note/api/card_attn.hpp>
#include <note/app/state_store.hpp>

#include <algorithm>
#include <string>

namespace note::app {

struct AttentionState {
    bool set{};
    std::string mode;
};

template<typename Channel, typename Store = NullStateStore>
class Attention {
public:
    Attention(Channel& ch, Store& store)
        : ch_(ch), store_(store) {}

    // Add a keyword to the desired mode set (e.g. "files", "sleep", "arm").
    void enable(string_view keyword) {
        if (has_keyword(keyword)) return;
        if (!desired_mode_.empty()) desired_mode_ += ',';
        desired_mode_ += keyword;
    }

    // Remove a keyword from the desired mode set.
    void disable(string_view keyword) {
        std::string result;
        string_view rest(desired_mode_);
        while (!rest.empty()) {
            auto pos = rest.find(',');
            auto token = (pos != string_view::npos)
                ? rest.substr(0, pos)
                : rest;
            if (token != keyword) {
                if (!result.empty()) result += ',';
                result += token;
            }
            rest = (pos != string_view::npos)
                ? rest.substr(pos + 1)
                : string_view{};
        }
        desired_mode_ = std::move(result);
    }

    // Arm attention with the current desired mode set.
    auto arm(Seconds timeout = {}) -> Result<void> {
        api::CardAttn::Arm req;
        if (!desired_mode_.empty()) req.triggers(desired_mode_);
        if (timeout.count > 0) req.seconds(timeout);
        auto r = ch_.execute(req);
        if (!r) return Unexpected(r.error());
        store_.set(AttentionState{
            .set = r.set,
            .mode = desired_mode_,
        });
        return {};
    }

    // Query current attention state.
    auto query() -> ApiResult<api::CardAttn::Query::Response> {
        api::CardAttn::Query req;
        req.verify(true);
        auto r = ch_.execute(req);
        if (r) {
            store_.set(AttentionState{
                .set = r.set,
                .mode = desired_mode_,
            });
        }
        return r;
    }

    // Check if the attention pin is set (HIGH).
    auto triggered() -> Result<bool> {
        api::CardAttn::Query req;
        auto r = ch_.execute(req);
        if (!r) return Unexpected(r.error());
        return r.set;
    }

    const std::string& mode() const { return desired_mode_; }

private:
    Channel& ch_;
    Store& store_;
    std::string desired_mode_;

    bool has_keyword(string_view keyword) const {
        string_view rest(desired_mode_);
        while (!rest.empty()) {
            auto pos = rest.find(',');
            auto token = (pos != string_view::npos)
                ? rest.substr(0, pos)
                : rest;
            if (token == keyword) return true;
            rest = (pos != string_view::npos)
                ? rest.substr(pos + 1)
                : string_view{};
        }
        return false;
    }
};

} // namespace note::app
