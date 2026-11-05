#include <catch2/catch_all.hpp>

#include "slic3r/Utils/ElegooCanvasMatch.hpp"

#include <filesystem>
#include <fstream>

using namespace Slic3r::ElegooCanvas;

namespace {

Tray make_tray(int canvas, int id, const std::string &brand, const std::string &type, const std::string &name,
               const std::string &color, TrayStatus st = TrayStatus::Loaded)
{
    Tray t;
    t.canvas_id = canvas;
    t.tray_id = id;
    t.brand = brand;
    t.filament_type = type;
    t.filament_name = name;
    t.filament_color = color;
    t.status = st;
    t.unit_connected = true;
    return t;
}

Info make_info(std::vector<Tray> trays)
{
    Info info;
    Unit u;
    u.canvas_id = 0;
    u.connected = true;
    u.trays = std::move(trays);
    info.units.push_back(u);
    return info;
}

ProjectFilament make_fil(int index, const std::string &vendor, const std::string &alias, const std::string &type,
                         const std::string &color)
{
    ProjectFilament f;
    f.index = index;
    f.vendor = vendor;
    f.name = alias;
    f.alias = alias;
    f.type = type;
    f.color = color;
    return f;
}

std::filesystem::path temp_file(const std::string &name)
{
    const char *dir = std::getenv("ELEGOO_TEST_TMP");
    std::filesystem::path d = dir ? dir : std::filesystem::temp_directory_path();
    d /= "elegoo_match_test";
    std::filesystem::create_directories(d);
    auto p = d / name;
    std::filesystem::remove(p);
    return p;
}

} // namespace

TEST_CASE("color_bucket names buckets from the palette", "[elegoo][canvas]")
{
    CHECK(color_bucket("#FF0000") == "red");
    CHECK(color_bucket("ff0000") == "red");
    CHECK(color_bucket("#000000") == "black");
    CHECK(color_bucket("#FFFFFF") == "white");
    CHECK(color_bucket("#F72221") == "red");
    CHECK(color_bucket("") == "");
    CHECK(color_bucket("#12345") == "");
    CHECK(color_bucket("#GGGGGG") == "");
}

TEST_CASE("auto_match picks a tray in the same colour bucket", "[elegoo][canvas]")
{
    MappingStore store(boost::filesystem::path(temp_file("none.json").string()));
    Info info = make_info({make_tray(0, 0, "Elegoo", "PLA", "Elegoo PLA", "#0000FF"),
                           make_tray(0, 1, "Elegoo", "PLA", "Elegoo PLA", "#FF0000")});
    auto m = auto_match({make_fil(0, "Elegoo", "Elegoo PLA", "PLA", "#FA2020")}, info, store);
    REQUIRE(m.size() == 1);
    REQUIRE(m[0].tray.has_value());
    CHECK(*m[0].tray == std::make_pair(0, 1));
    CHECK(m[0].source == MatchSource::Matched);
    CHECK(m[0].warnings.empty());
}

TEST_CASE("auto_match takes the first matching tray in order", "[elegoo][canvas]")
{
    MappingStore store(boost::filesystem::path(temp_file("none.json").string()));
    // Both trays match; ElegooSlicer takes the first one scanned, vendor is not ranked across trays.
    Info info = make_info({make_tray(0, 0, "Generic", "PLA", "Generic PLA", "#FF0000"),
                           make_tray(0, 1, "Elegoo", "PLA", "Elegoo PLA", "#FF0000")});
    auto m = auto_match({make_fil(0, "Elegoo", "Elegoo PLA", "PLA", "#FF0000")}, info, store);
    REQUIRE(m[0].tray.has_value());
    CHECK(*m[0].tray == std::make_pair(0, 0));

    Info swapped = make_info({make_tray(0, 0, "Elegoo", "PLA", "Elegoo PLA", "#FF0000"),
                              make_tray(0, 1, "Generic", "PLA", "Generic PLA", "#FF0000")});
    auto m2 = auto_match({make_fil(0, "Elegoo", "Elegoo PLA", "PLA", "#FF0000")}, swapped, store);
    CHECK(*m2[0].tray == std::make_pair(0, 0));

    // a type-only match that comes first beats a later exact-name match
    Info order = make_info({make_tray(0, 0, "Acme", "PLA", "Silk Rainbow", "#FF0000"),
                            make_tray(0, 1, "Elegoo", "PLA", "Elegoo PLA", "#FF0000")});
    auto m3 = auto_match({make_fil(0, "Elegoo", "Elegoo PLA", "PLA", "#FF0000")}, order, store);
    CHECK(*m3[0].tray == std::make_pair(0, 0));
}

TEST_CASE("names compare with dashes equal to spaces", "[elegoo][canvas]")
{
    MappingStore store(boost::filesystem::path(temp_file("none.json").string()));
    // Different types so only the standardised-name rule can match.
    Info info = make_info({make_tray(0, 0, "Acme", "PETG", "PLA CF", "#FF0000")});
    auto m = auto_match({make_fil(0, "Elegoo", "PLA-CF", "ABS", "#FF0000")}, info, store);
    REQUIRE(m[0].tray.has_value());
    CHECK(m[0].warnings == std::vector<std::string>{"type_mismatch"});
    Info other = make_info({make_tray(0, 0, "Acme", "PETG", "PLA CFX", "#FF0000")});
    CHECK_FALSE(auto_match({make_fil(0, "Elegoo", "PLA-CF", "ABS", "#FF0000")}, other, store)[0].tray.has_value());
}

TEST_CASE("auto_match falls back to type only, and warns on a mismatching type", "[elegoo][canvas]")
{
    MappingStore store(boost::filesystem::path(temp_file("none.json").string()));
    Info info = make_info({make_tray(0, 0, "Acme", "PLA", "Silk Rainbow", "#FF0000")});
    auto m = auto_match({make_fil(0, "Elegoo", "Elegoo PLA", "PLA", "#FF0000")}, info, store);
    REQUIRE(m[0].tray.has_value());
    CHECK(m[0].source == MatchSource::Matched);

    auto none = auto_match({make_fil(0, "Elegoo", "Elegoo PETG", "PETG", "#FF0000")}, info, store);
    CHECK_FALSE(none[0].tray.has_value());
    CHECK(none[0].source == MatchSource::None);

    // same standardised name but different type: still matched, flagged
    Info info2 = make_info({make_tray(0, 0, "Elegoo", "PETG", "Elegoo Basic", "#FF0000")});
    auto warn = auto_match({make_fil(0, "Elegoo", "Elegoo Basic", "PLA", "#FF0000")}, info2, store);
    REQUIRE(warn[0].tray.has_value());
    REQUIRE(warn[0].warnings.size() == 1);
    CHECK(warn[0].warnings[0] == "type_mismatch");
}

TEST_CASE("auto_match never uses unknown or not-ready trays", "[elegoo][canvas]")
{
    MappingStore store(boost::filesystem::path(temp_file("none.json").string()));
    Tray unknown = make_tray(0, 0, "Elegoo", "PLA", "Elegoo PLA", "#FF0000");
    unknown.unknown_filament = true;
    Tray empty = make_tray(0, 1, "Elegoo", "PLA", "Elegoo PLA", "#FF0000", TrayStatus::Disconnected);
    Tray nocolour = make_tray(0, 2, "Elegoo", "PLA", "Elegoo PLA", "");
    Info info = make_info({unknown, empty, nocolour});
    auto m = auto_match({make_fil(0, "Elegoo", "Elegoo PLA", "PLA", "#FF0000")}, info, store);
    CHECK_FALSE(m[0].tray.has_value());
    CHECK(m[0].source == MatchSource::None);

    auto bad = auto_match({make_fil(0, "Elegoo", "Elegoo PLA", "PLA", "not a colour")}, info, store);
    CHECK_FALSE(bad[0].tray.has_value());
}

TEST_CASE("a saved choice wins over a closer colour", "[elegoo][canvas]")
{
    auto file = temp_file("saved.json");
    MappingStore store(boost::filesystem::path(file.string()));
    // The user once mapped this red filament to a differently named, slightly different red spool.
    Tray chosen = make_tray(0, 1, "Acme", "PLA", "Acme Red", "#F04040");
    store.remember(make_fil(0, "Elegoo", "Elegoo PLA", "PLA", "#FF0000"), chosen);
    Info info = make_info({make_tray(0, 0, "Elegoo", "PLA", "Elegoo PLA", "#FF0000"), chosen});
    auto m = auto_match({make_fil(0, "Elegoo", "Elegoo PLA", "PLA", "#FF0000")}, info, store);
    REQUIRE(m[0].tray.has_value());
    CHECK(*m[0].tray == std::make_pair(0, 1));
    CHECK(m[0].source == MatchSource::Saved);

    auto lowercase = chosen;
    lowercase.filament_color = "#f04040";
    Info info2 = make_info({make_tray(0, 0, "Elegoo", "PLA", "Elegoo PLA", "#FF0000"), lowercase});
    auto m2 = auto_match({make_fil(0, "Elegoo", "Elegoo PLA", "PLA", "#FF0000")}, info2, store);
    CHECK(m2[0].source == MatchSource::Saved);

    // saved spool is not loaded any more: normal matching takes over
    Info info3 = make_info({make_tray(0, 0, "Elegoo", "PLA", "Elegoo PLA", "#FF0000")});
    auto m3 = auto_match({make_fil(0, "Elegoo", "Elegoo PLA", "PLA", "#FF0000")}, info3, store);
    CHECK(m3[0].source == MatchSource::Matched);
}

TEST_CASE("MappingStore round trips with ElegooSlicer's file shape", "[elegoo][canvas]")
{
    auto file = temp_file("roundtrip.json");
    ProjectFilament f = make_fil(0, "Elegoo", "Elegoo PLA", "PLA", "#FF1010");
    Tray t = make_tray(0, 2, "Acme", "PLA", "Acme Red", "#F04040");
    {
        MappingStore s(boost::filesystem::path(file.string()));
        s.remember(f, t);
        s.save();
    }
    std::ifstream in(file);
    nlohmann::json j = nlohmann::json::parse(in);
    const auto &e = j.at("PLA").at("Elegoo PLA").at("#F72221");
    CHECK(e.at("mappedFilamentType") == "PLA");
    CHECK(e.at("mappedFilamentName") == "Acme Red");
    CHECK(e.at("mappedFilamentColor") == "#F04040");
    CHECK(e.at("mappedFilamentStandardColor") == "#F72221");
    CHECK(e.at("mappedFilamentStandardColorName") == "red");
    CHECK(e.at("filamentStandardColor") == "#F72221");
    CHECK(e.at("filamentStandardColorName") == "red");

    MappingStore s2(boost::filesystem::path(file.string()));
    CHECK_FALSE(s2.lookup(f).has_value()); // not loaded yet
    s2.load();
    auto got = s2.lookup(f);
    REQUIRE(got.has_value());
    CHECK(got->first == "Acme Red");
    CHECK(got->second == "#F04040");
    // another shade of red shares the bucket key
    CHECK(s2.lookup(make_fil(1, "Elegoo", "Elegoo PLA", "PLA", "#EE3030")).has_value());
    CHECK_FALSE(s2.lookup(make_fil(1, "Elegoo", "Elegoo PLA", "PLA", "#0000FF")).has_value());

    // a second remember keeps earlier entries
    s2.remember(make_fil(1, "Elegoo", "Elegoo PLA", "PLA", "#0000FF"), make_tray(0, 0, "Elegoo", "PLA", "Elegoo Blue", "#0000FF"));
    s2.save();
    MappingStore s3(boost::filesystem::path(file.string()));
    s3.load();
    CHECK(s3.lookup(f).has_value());
    CHECK(s3.lookup(make_fil(1, "Elegoo", "Elegoo PLA", "PLA", "#0000FF")).has_value());
}

TEST_CASE("MappingStore: missing file and corrupt file give an empty store", "[elegoo][canvas]")
{
    ProjectFilament f = make_fil(0, "Elegoo", "Elegoo PLA", "PLA", "#FF0000");
    MappingStore missing(boost::filesystem::path(temp_file("absent.json").string()));
    missing.load();
    CHECK_FALSE(missing.lookup(f).has_value());

    auto bad = temp_file("corrupt.json");
    { std::ofstream(bad) << "{ this is not json"; }
    MappingStore corrupt(boost::filesystem::path(bad.string()));
    corrupt.load();
    CHECK_FALSE(corrupt.lookup(f).has_value());

    auto arr = temp_file("array.json");
    { std::ofstream(arr) << "[1,2,3]"; }
    MappingStore wrong(boost::filesystem::path(arr.string()));
    wrong.load();
    CHECK_FALSE(wrong.lookup(f).has_value());

    // save creates missing directories
    auto nested = std::filesystem::path(temp_file("x.json").parent_path()) / "sub" / "dir" / "m.json";
    std::filesystem::remove_all(nested.parent_path().parent_path());
    MappingStore s(boost::filesystem::path(nested.string()));
    s.remember(f, make_tray(0, 0, "Elegoo", "PLA", "Elegoo PLA", "#FF0000"));
    s.save();
    CHECK(std::filesystem::exists(nested));
}

TEST_CASE("remember ignores trays it cannot describe", "[elegoo][canvas]")
{
    auto file = temp_file("ignored.json");
    MappingStore s(boost::filesystem::path(file.string()));
    ProjectFilament f = make_fil(0, "Elegoo", "Elegoo PLA", "PLA", "#FF0000");
    s.remember(f, make_tray(0, 0, "Elegoo", "PLA", "", "#FF0000"));
    s.remember(f, make_tray(0, 0, "Elegoo", "PLA", "Elegoo PLA", ""));
    s.remember(make_fil(0, "Elegoo", "Elegoo PLA", "PLA", "junk"), make_tray(0, 0, "Elegoo", "PLA", "Elegoo PLA", "#FF0000"));
    CHECK_FALSE(s.lookup(f).has_value());
}

TEST_CASE("validate reports unmapped, not-ready and shared trays", "[elegoo][canvas]")
{
    Info info = make_info({make_tray(0, 0, "Elegoo", "PLA", "Elegoo PLA", "#FF0000"),
                           make_tray(0, 1, "Elegoo", "PLA", "Elegoo PLA", "#00FF00", TrayStatus::Disconnected),
                           make_tray(0, 2, "Elegoo", "PLA", "Elegoo PLA", "#0000FF")});
    Mapping a; a.index = 0; a.tray = std::make_pair(0, 0); a.source = MatchSource::Matched;
    Mapping b; b.index = 1; b.tray = std::make_pair(0, 1); b.source = MatchSource::Matched;
    Mapping c; c.index = 2; c.tray = std::make_pair(0, 0); c.source = MatchSource::User;
    Mapping d; d.index = 3;

    CHECK(validate({a}, info).empty());

    auto msgs = validate({a, b, c, d}, info);
    REQUIRE(msgs.size() == 3);
    CHECK(msgs[0] == "Filament 2 is mapped to tray A1, which is not ready");
    CHECK(msgs[1] == "Filament 4 is not mapped to a tray");
    CHECK(msgs[2] == "Filaments 1 and 3 are both mapped to tray A0");

    Mapping gone; gone.index = 0; gone.tray = std::make_pair(5, 5);
    CHECK(validate({gone}, info).size() == 1);

    Mapping e = a; e.index = 4;
    Mapping f = a; f.index = 5;
    auto three = validate({a, e, f}, info);
    REQUIRE(three.size() == 1);
    CHECK(three[0] == "Filaments 1, 5 and 6 are both mapped to tray A0");
}

TEST_CASE("remember survives a hand-edited file with non-object nodes", "[elegoo][canvas]")
{
    ProjectFilament f = make_fil(0, "Elegoo", "Elegoo PLA", "PLA", "#FF0000");
    Tray t = make_tray(0, 0, "Elegoo", "PLA", "Elegoo PLA", "#FF0000");
    for (const char *body : {"{\"PLA\": 5}", "{\"PLA\": {\"Elegoo PLA\": [1]}}"}) {
        auto file = temp_file("handedit.json");
        { std::ofstream(file) << body; }
        MappingStore s(boost::filesystem::path(file.string()));
        s.load();
        CHECK_FALSE(s.lookup(f).has_value());
        CHECK_NOTHROW(s.remember(f, t));
        CHECK(s.lookup(f).has_value());
    }
}

TEST_CASE("match_tray_preset resolves a tray to a filament preset", "[elegoo][canvas]")
{
    auto preset = [](const std::string &vendor, const std::string &name, const std::string &alias, const std::string &type) {
        ProjectFilament p;
        p.vendor = vendor;
        p.name   = name;
        p.alias  = alias;
        p.type   = type;
        return p;
    };
    const std::vector<ProjectFilament> presets = {
        preset("OrcaFilamentLibrary", "Generic PLA @System", "Generic PLA", "PLA"),       // 0
        preset("Elegoo", "Elegoo PLA @ECC2", "Elegoo PLA", "PLA"),                         // 1
        preset("OrcaFilamentLibrary", "Generic PETG @System", "Generic PETG", "PETG"),    // 2
        preset("Acme", "Acme PLA Silk", "Acme PLA Silk", "PLA"),                           // 3
        preset("Elegoo", "Elegoo PLA Silk @ECC2", "Elegoo PLA Silk", "PLA"),               // 4
        preset("", "My TPU", "", "TPU"),                                                    // 5
    };

    SECTION("standardised name wins over type; the tray's brand first")
    {
        // "PLA Silk" names both Acme's and Elegoo's silk preset; the tray's brand picks Acme.
        CHECK(match_tray_preset(make_tray(0, 0, "Acme", "PLA", "Acme PLA Silk", "#FF0000"), presets) == std::optional<size_t>(3));
        CHECK(match_tray_preset(make_tray(0, 0, "Elegoo", "PLA", "PLA Silk", "#FF0000"), presets) == std::optional<size_t>(4));
        // No brand: Elegoo before the others.
        CHECK(match_tray_preset(make_tray(0, 0, "", "PLA", "PLA Silk", "#FF0000"), presets) == std::optional<size_t>(4));
    }
    SECTION("name equals type: Elegoo before generic")
    {
        // "PLA" standardises to "PLA" = the generic preset's standardised name and the type of several presets.
        CHECK(match_tray_preset(make_tray(0, 0, "Elegoo", "PLA", "PLA", "#FF0000"), presets) == std::optional<size_t>(1));
        CHECK(match_tray_preset(make_tray(0, 0, "Sunlu", "PETG", "PETG", "#FF0000"), presets) == std::optional<size_t>(2));
    }
    SECTION("type fallback")
    {
        CHECK(match_tray_preset(make_tray(0, 0, "Sunlu", "PETG", "PETG Rapid", "#FF0000"), presets) == std::optional<size_t>(2));
        CHECK(match_tray_preset(make_tray(0, 0, "Sunlu", "TPU", "Flex 95A", "#FF0000"), presets) == std::optional<size_t>(5));
    }
    SECTION("no match")
    {
        CHECK_FALSE(match_tray_preset(make_tray(0, 0, "Sunlu", "PEEK", "PEEK Pro", "#FF0000"), presets).has_value());
        CHECK_FALSE(match_tray_preset(make_tray(0, 0, "Elegoo", "PLA", "", "#FF0000"), presets).has_value());
        CHECK_FALSE(match_tray_preset(make_tray(0, 0, "Sunlu", "", "Mystery", "#FF0000"), presets).has_value());
        CHECK_FALSE(match_tray_preset(make_tray(0, 0, "Elegoo", "PLA", "PLA", "#FF0000"), {}).has_value());
    }
}

// Candidates as Sidebar::load_canvas_list builds them for a Centauri Carbon 2 (sorted by name, as the preset collection
// is): the printer vendor's ECC2 presets, the vendor's generic preset and Orca filament library presets. Vendor = the
// preset's filament_vendor; `library` marks Orca filament library presets. Types are the profiles' own (Elegoo's
// PLA-CF and PETG-CF profiles inherit plain PLA / PETG), plus a PLA-CF typed generic.
TEST_CASE("match_tray_preset on the Centauri Carbon 2 filament set", "[elegoo][canvas]")
{
    auto preset = [](const std::string &vendor, const std::string &name, const std::string &type, bool library = false) {
        ProjectFilament p;
        p.vendor  = vendor;
        p.name    = name;
        p.alias   = name.substr(0, name.find(" @"));
        p.type    = type;
        p.library = library;
        return p;
    };
    const std::vector<ProjectFilament> ecc2 = {
        preset("Elegoo", "Elegoo PET-CF @ECC2", "PET-CF"),             // 0
        preset("Elegoo", "Elegoo PET-CF @System", "PET-CF", true),     // 1
        preset("Elegoo", "Elegoo PETG @ECC2", "PETG"),                 // 2
        preset("Elegoo", "Elegoo PETG @System", "PETG", true),         // 3
        preset("Elegoo", "Elegoo PETG-CF @ECC2", "PETG"),              // 4
        preset("Elegoo", "Elegoo PLA @ECC2", "PLA"),                   // 5
        preset("Elegoo", "Elegoo PLA @System", "PLA", true),           // 6
        preset("Elegoo", "Elegoo PLA Basic @ECC2", "PLA"),             // 7
        preset("Elegoo", "Elegoo PLA+ @ECC2", "PLA"),                  // 8
        preset("Elegoo", "Elegoo PLA-CF @ECC2", "PLA"),                // 9
        preset("Generic", "Generic PET-CF @Elegoo Centauri", "PET-CF"), // 10
        preset("Generic", "Generic PLA @Elegoo Centauri", "PLA"),      // 11
        preset("Generic", "Generic PLA @System", "PLA", true),         // 12
        preset("Generic", "Generic PLA-CF @System", "PLA-CF", true),   // 13
    };
    auto hit = [&](const std::string &brand, const std::string &type, const std::string &name) -> std::string {
        const auto i = match_tray_preset(make_tray(0, 0, brand, type, name, "#FF0000"), ecc2);
        return i ? ecc2[*i].name : std::string("<none>");
    };

    SECTION("the rig's trays (fake CC2): exact names")
    {
        CHECK(hit("Elegoo", "PLA", "Elegoo PLA") == "Elegoo PLA @ECC2");
        CHECK(hit("Elegoo", "PETG", "Elegoo PETG") == "Elegoo PETG @ECC2");
    }
    SECTION("name with or without the vendor word, any case, dashes as spaces")
    {
        CHECK(hit("Elegoo", "PLA", "PLA") == "Elegoo PLA @ECC2");
        CHECK(hit("", "PLA", "Elegoo PLA") == "Elegoo PLA @ECC2");
        CHECK(hit("ELEGOO", "pla", "elegoo pla") == "Elegoo PLA @ECC2");
        CHECK(hit("Elegoo", "PLA", "Elegoo PLA+") == "Elegoo PLA+ @ECC2");
        CHECK(hit("Elegoo", "PET-CF", "Elegoo PET CF") == "Elegoo PET-CF @ECC2");
        CHECK(hit("Elegoo", "PETG", "Elegoo PETG CF") == "Elegoo PETG-CF @ECC2");
    }
    SECTION("an exact name keeps within the type family (Elegoo labels its CF profiles PLA / PETG)")
    {
        CHECK(hit("Elegoo", "PLA-CF", "Elegoo PLA-CF") == "Elegoo PLA-CF @ECC2");
        CHECK(hit("Elegoo", "PETG-CF", "Elegoo PETG-CF") == "Elegoo PETG-CF @ECC2");
        // A name that belongs to another type never wins: PETG is not the PET family.
        CHECK(hit("Elegoo", "PETG", "Elegoo PET-CF") == "Elegoo PETG @ECC2");
        CHECK(hit("Elegoo", "PLA", "Elegoo PETG") == "Elegoo PLA @ECC2");
    }
    SECTION("type tier: the tray's brand, then generic; exact types only")
    {
        CHECK(hit("Elegoo", "PLA", "Mystery Rainbow") == "Elegoo PLA @ECC2");
        CHECK(hit("", "PLA", "Mystery Rainbow") == "Generic PLA @Elegoo Centauri");
        CHECK(hit("Sunlu", "PLA", "Sunlu Rainbow") == "Generic PLA @Elegoo Centauri");
        CHECK(hit("Sunlu", "PETG", "Sunlu Rapid") == "Elegoo PETG @ECC2"); // no generic PETG here: first PETG
        CHECK(hit("", "PLA-CF", "Something CF") == "Generic PLA-CF @System");
        CHECK(hit("", "PET-CF", "Something") == "Generic PET-CF @Elegoo Centauri");
    }
    SECTION("no candidate of the tray's type")
    {
        CHECK(hit("Elegoo", "ASA", "Elegoo ASA") == "<none>");
        CHECK(hit("", "ASA", "ASA") == "<none>");
    }
    SECTION("every hit has the tray's type or its family")
    {
        for (const std::string type : {"PLA", "PETG", "PET-CF", "PLA-CF"})
            for (const std::string name : {"PLA", "PETG", "PET-CF", "Elegoo PLA", "Elegoo PETG", "Elegoo PET-CF", "X"}) {
                const auto i = match_tray_preset(make_tray(0, 0, "Elegoo", type, name, "#FF0000"), ecc2);
                if (!i)
                    continue;
                const std::string &pt = ecc2[*i].type;
                INFO(type << " / " << name << " -> " << ecc2[*i].name);
                CHECK((pt == type || type.rfind(pt + "-", 0) == 0 || pt.rfind(type + "-", 0) == 0));
            }
    }
}

TEST_CASE("match_tray_preset: dashed types and the family exception's direction", "[elegoo][canvas]")
{
    auto preset = [](const std::string &vendor, const std::string &name, const std::string &type) {
        ProjectFilament p;
        p.vendor = vendor;
        p.name   = name;
        p.alias  = name.substr(0, name.find(" @"));
        p.type   = type;
        return p;
    };
    auto hit = [](const std::vector<ProjectFilament> &ps, const std::string &brand, const std::string &type,
                  const std::string &name) -> std::string {
        const auto i = match_tray_preset(make_tray(0, 0, brand, type, name, "#FF0000"), ps);
        return i ? ps[*i].name : std::string("<none>");
    };

    SECTION("name-equals-type works for a dashed type (pass 2 beats a plain same-type preset)")
    {
        const std::vector<ProjectFilament> ps = {preset("Acme", "Acme Carbon Pro @X", "PLA-CF"),
                                                 preset("Acme", "Acme PLA-CF @X", "PLA-CF")};
        CHECK(hit(ps, "", "PLA-CF", "Mystery") == "Acme PLA-CF @X");
        CHECK(hit(ps, "", "pla-cf", "Mystery") == "Acme PLA-CF @X");
    }
    SECTION("a plain tray never takes a more specific (CF) preset by name")
    {
        const std::vector<ProjectFilament> ps = {preset("Generic", "Generic PLA-CF @System", "PLA-CF"),
                                                 preset("Generic", "Generic PLA @System", "PLA")};
        CHECK(hit(ps, "", "PLA", "PLA-CF") == "Generic PLA @System");
        // the documented direction still works: a CF tray takes the base-typed Elegoo CF profile
        const std::vector<ProjectFilament> elegoo = {preset("Elegoo", "Elegoo PLA-CF @ECC2", "PLA")};
        CHECK(hit(elegoo, "Elegoo", "PLA-CF", "Elegoo PLA-CF") == "Elegoo PLA-CF @ECC2");
    }
}
