#pragma once

#include <note/api/note_template.hpp>
#include <note/app/state_store.hpp>

#include <string>
#include <vector>

namespace note::app {

// Describes one Notefile template to be registered.
struct TemplateEntry {
    std::string file;
    BodyValue body;
    int32_t port = 0;          // NTN: 1-100
    bool compact = false;      // NTN: format:"compact"
    bool registered = false;
};

template<typename Channel, typename Store = NullStateStore>
class Templates {
public:
    Templates(Channel& ch, Store& store)
        : ch_(ch), store_(store) {}

    // Declare a template with an existing BodyValue (e.g. from template_of<T>()).
    void declare(string_view file, BodyValue body,
                 int32_t port = 0, bool compact = false) {
        entries_.push_back(TemplateEntry{
            .file = std::string(file),
            .body = std::move(body),
            .port = port,
            .compact = compact,
        });
    }

    // Register all declared templates that haven't been registered yet.
    // Sends note.template for each. In NTN mode, validates port and compact.
    auto register_all() -> Result<void> {
        for (auto& entry : entries_) {
            if (entry.registered) continue;

            if (ntn_) {
                if (entry.port <= 0 || entry.port > 100) {
                    return make_error(Error::InvalidArg,
                        "NTN requires port 1-100 for template");
                }
                if (!entry.compact) {
                    return make_error(Error::InvalidArg,
                        "NTN requires compact format for template");
                }
            }

            api::NoteTemplate::Set req;
            req.file = entry.file;
            req.body(std::move(entry.body));
            if (entry.port > 0) req.port(entry.port);
            if (entry.compact) req.format("compact");

            auto r = ch_.execute(req);
            if (!r) return Unexpected(r.error());

            entry.registered = true;
        }
        return {};
    }

    // Check if a file's template has been registered.
    bool is_registered(string_view file) const {
        for (const auto& entry : entries_) {
            if (entry.file == file) return entry.registered;
        }
        return false;
    }

    // Reset registration state (e.g. after factory reset).
    void reset() {
        for (auto& entry : entries_) {
            entry.registered = false;
        }
    }

    void set_ntn(bool ntn) { ntn_ = ntn; }
    bool is_ntn() const { return ntn_; }

    size_t count() const { return entries_.size(); }

private:
    Channel& ch_;
    Store& store_;
    std::vector<TemplateEntry> entries_;
    bool ntn_ = false;
};

} // namespace note::app
