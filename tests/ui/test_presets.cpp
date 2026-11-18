// Presets (ui/presets.hpp): scanning, order, stepping, names and atomic file writes.
#include "amq/ui/presets.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <random>

namespace fs = std::filesystem;
using namespace amq;

namespace {
fs::path tempRoot() {
    std::mt19937 rng(std::random_device{}());
    const fs::path p = fs::temp_directory_path() / ("amq-presets-test-" + std::to_string(rng()));
    fs::create_directories(p);
    return p;
}
void touch(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::string err;
    REQUIRE(ui::writeFileBytes(p, {1, 2, 3}, &err));
}
}  // namespace

TEST_CASE("Presets: Init first, then by category and name, case-insensitively") {
    const fs::path root = tempRoot();
    touch(root / "zeta.amqpreset");
    touch(root / "Alpha.amqpreset");
    touch(root / "Vocals" / "air.amqpreset");
    touch(root / "drums" / "Snap.AMQPRESET");
    touch(root / "notes.txt");  // ignored
    const auto list = ui::scanPresets(root);
    REQUIRE(list.size() == 5);
    CHECK(list[0].name == "Init");
    CHECK(list[0].builtIn());
    CHECK(list[1].name == "Alpha");
    CHECK(list[1].category.empty());
    CHECK(list[2].name == "zeta");
    CHECK(list[3].category == "drums");
    CHECK(list[3].name == "Snap");
    CHECK(list[4].category == "Vocals");
    CHECK(list[4].name == "air");
    // A missing folder gives Init only.
    CHECK(ui::scanPresets(root / "missing").size() == 1);
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("Presets: stepping wraps; names are made file-safe; writes replace whole files") {
    CHECK(ui::stepPreset(4, 0, +1) == 1);
    CHECK(ui::stepPreset(4, 3, +1) == 0);
    CHECK(ui::stepPreset(4, 0, -1) == 3);
    CHECK(ui::stepPreset(4, -1, +1) == 0);
    CHECK(ui::stepPreset(4, -1, -1) == 3);
    CHECK(ui::stepPreset(0, 0, +1) == -1);
    CHECK(ui::sanitizePresetName(" Bright: vocal/air? ") == "Bright- vocal-air-");
    CHECK(ui::sanitizePresetName("...") == "Preset");
    CHECK(ui::sanitizePresetName("Init") == "Init");

    const fs::path root = tempRoot();
    const fs::path p = root / "sub" / "a.amqpreset";
    std::string err;
    REQUIRE(ui::writeFileBytes(p, {9, 8, 7, 6}, &err));
    REQUIRE(ui::writeFileBytes(p, {5}, &err));  // replaces, never appends
    std::vector<uint8_t> back;
    REQUIRE(ui::readFileBytes(p, back, &err));
    CHECK(back == std::vector<uint8_t>{5});
    CHECK_FALSE(fs::exists(p.string() + ".tmp"));
    CHECK_FALSE(ui::readFileBytes(root / "none.amqpreset", back, &err));
    CHECK(err.find("none.amqpreset") != std::string::npos);
    std::error_code ec;
    fs::remove_all(root, ec);
}
