// Tests for FlagSet: add, operator|=, set, clear, serialization.
#include <doctest.h>
#include <note/flag_set.hpp>
#include <note/api/card_attn.hpp>
#include <note/api/card_triangulate.hpp>
#include <string_view>

static constexpr note::FlagDef test_defs[] = {
    {1u << 0, "alpha"},
    {1u << 1, "bravo"},
    {1u << 2, "charlie"},
};

TEST_CASE("FlagSet add single flag") {
    note::FlagSet<3, 32> fs{test_defs};
    fs.add(1u << 0);
    CHECK(fs.str() == "alpha");
    CHECK(fs.bits() == 1u);
}

TEST_CASE("FlagSet add multiple flags preserves order") {
    note::FlagSet<3, 32> fs{test_defs};
    fs.add(1u << 2).add(1u << 0);
    CHECK(fs.str() == "alpha,charlie");
}

TEST_CASE("FlagSet operator|=") {
    note::FlagSet<3, 32> fs{test_defs};
    fs |= (1u << 0 | 1u << 1);
    CHECK(fs.str() == "alpha,bravo");
}

TEST_CASE("FlagSet set replaces all flags") {
    note::FlagSet<3, 32> fs{test_defs};
    fs.add(1u << 0);
    fs.set(1u << 1 | 1u << 2);
    CHECK(fs.str() == "bravo,charlie");
    CHECK(fs.bits() == (1u << 1 | 1u << 2));
}

TEST_CASE("FlagSet clear") {
    note::FlagSet<3, 32> fs{test_defs};
    fs.add(1u << 0);
    fs.clear();
    CHECK(fs.str() == "");
    CHECK(fs.bits() == 0u);
    CHECK_FALSE(static_cast<bool>(fs));
}

TEST_CASE("FlagSet empty state") {
    note::FlagSet<3, 32> fs{test_defs};
    CHECK(fs.str() == "");
    CHECK_FALSE(static_cast<bool>(fs));
}

TEST_CASE("FlagSet operator bool") {
    note::FlagSet<3, 32> fs{test_defs};
    fs.add(1u << 1);
    CHECK(static_cast<bool>(fs));
}

TEST_CASE("FlagSet implicit string_view conversion") {
    note::FlagSet<3, 32> fs{test_defs};
    fs.add(1u << 0).add(1u << 2);
    std::string_view sv = fs;
    CHECK(sv == "alpha,charlie");
}

TEST_CASE("FlagSet all flags") {
    note::FlagSet<3, 32> fs{test_defs};
    fs |= (1u << 0 | 1u << 1 | 1u << 2);
    CHECK(fs.str() == "alpha,bravo,charlie");
}

TEST_CASE("FlagSet duplicate add is idempotent") {
    note::FlagSet<3, 32> fs{test_defs};
    fs.add(1u << 0).add(1u << 0);
    CHECK(fs.str() == "alpha");
}

TEST_CASE("FlagSet returns self from add") {
    note::FlagSet<3, 32> fs{test_defs};
    auto& ref = fs.add(1u << 0);
    CHECK(&ref == &fs);
}

// ── Generated card.attn API ──────────────────────────────────────────

TEST_CASE("CardAttn Arm triggers named methods") {
    note::api::CardAttn::Arm req;
    req.triggers.connected();
    CHECK(std::string_view(*req.triggers) == "connected");
}

TEST_CASE("CardAttn Arm triggers operator|= with namespace constants") {
    note::api::CardAttn::Arm req;
    req.triggers |= note::attn::files;
    CHECK(std::string_view(*req.triggers) == "files");
}

TEST_CASE("CardAttn Arm triggers combined flags") {
    note::api::CardAttn::Arm req;
    req.triggers(note::attn::connected | note::attn::motion);
    CHECK(std::string_view(*req.triggers) == "connected,motion");
}

TEST_CASE("CardAttn triggers raw string assignment still works") {
    note::api::CardAttn::Arm req;
    req.triggers = "connected,env";
    CHECK(std::string_view(*req.triggers) == "connected,env");
}

TEST_CASE("CardTriangulate mode named methods") {
    note::api::CardTriangulate req;
    req.mode.cell().wifi();
    CHECK(std::string_view(*req.mode) == "cell,wifi");
}
