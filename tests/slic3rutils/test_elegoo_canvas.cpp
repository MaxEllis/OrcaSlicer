#include <catch2/catch_all.hpp>

#include "slic3r/Utils/ElegooCanvas.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>

using namespace Slic3r::ElegooCanvas;
using nlohmann::json;

namespace {
json tray(int id, int status, const char *type, const char *name, const char *color)
{
    return json{{"tray_id", id},
                {"brand", "Elegoo"},
                {"filament_type", type},
                {"filament_name", name},
                {"filament_code", "0x1234"},
                {"filament_color", color},
                {"min_nozzle_temp", 190},
                {"max_nozzle_temp", 230},
                {"status", status}};
}

json one_tray_info(const json &t)
{
    return json{{"active_canvas_id", 0},
                {"active_tray_id", 1},
                {"auto_refill", false},
                {"canvas_list", json::array({json{{"canvas_id", 0}, {"connected", 1}, {"tray_list", json::array({t})}}})}};
}
} // namespace

TEST_CASE("parse_canvas_info reads a normal loaded tray", "[elegoo_canvas]")
{
    Info info = parse_canvas_info(one_tray_info(tray(1, 2, "PLA", "PLA Basic", "#BCBCBC")));
    REQUIRE(info.units.size() == 1);
    CHECK(info.connected());
    CHECK(info.active_canvas_id == 0);
    CHECK(info.active_tray_id == 1);
    CHECK_FALSE(info.auto_refill);
    const Tray *t = info.find(0, 1);
    REQUIRE(t != nullptr);
    CHECK(t->ready());
    CHECK(t->label() == "A1");
    CHECK(t->status == TrayStatus::Loaded);
    CHECK(t->filament_type == "PLA");
    CHECK(t->filament_color == "#BCBCBC");
    CHECK(t->min_nozzle_temp == 190);
    CHECK(t->max_nozzle_temp == 230);
    CHECK_FALSE(t->unknown_filament);
    CHECK(info.find(0, 2) == nullptr);
    CHECK(info.find(3, 1) == nullptr);
}

TEST_CASE("preloaded tray with empty name is unknown and not ready", "[elegoo_canvas]")
{
    Info info = parse_canvas_info(one_tray_info(tray(2, 1, "PLA", "", "#BCBCBC")));
    const Tray *t = info.find(0, 2);
    REQUIRE(t != nullptr);
    CHECK(t->status == TrayStatus::Preloaded);
    CHECK(t->unknown_filament);
    CHECK_FALSE(t->ready());
}

TEST_CASE("colour normalisation", "[elegoo_canvas]")
{
    auto parse = [](const char *c) { return parse_canvas_info(one_tray_info(tray(0, 2, "PLA", "PLA Basic", c))).units[0].trays[0]; };
    CHECK_FALSE(parse("").ready());
    CHECK(parse("").filament_color.empty());
    CHECK_FALSE(parse("bcbcbc").ready());
    CHECK(parse("bcbcbc").filament_color.empty());
    CHECK(parse("#bcbcbc").filament_color == "#BCBCBC");
    CHECK(parse("#bcbcbc").ready());
    CHECK(parse("#BCBCB").filament_color.empty());
    CHECK(parse("#BCBCBCB").filament_color.empty());
    CHECK(parse("#GGGGGG").filament_color.empty());
}

TEST_CASE("tray status mapping and readiness", "[elegoo_canvas]")
{
    auto st = [](int s) { return parse_canvas_info(one_tray_info(tray(0, s, "PLA", "PLA Basic", "#000000"))).units[0].trays[0]; };
    CHECK(st(0).status == TrayStatus::Disconnected);
    CHECK_FALSE(st(0).ready());
    CHECK(st(1).status == TrayStatus::Preloaded);
    CHECK(st(1).ready());
    CHECK(st(2).status == TrayStatus::Loaded);
    CHECK(st(3).status == TrayStatus::Error);
    CHECK_FALSE(st(3).ready());
    CHECK(st(-1).status == TrayStatus::Error);
}

TEST_CASE("missing fields take defaults, missing canvas_list throws", "[elegoo_canvas]")
{
    Info info = parse_canvas_info(json{{"canvas_list", json::array({json{{"canvas_id", 1}, {"connected", true}, {"tray_list", json::array({json::object()})}}})}});
    CHECK(info.active_canvas_id == -1);
    CHECK(info.active_tray_id == -1);
    REQUIRE(info.units.size() == 1);
    CHECK(info.units[0].connected);
    CHECK(info.units[0].canvas_id == 1);
    REQUIRE(info.units[0].trays.size() == 1);
    const Tray &t = info.units[0].trays[0];
    CHECK(t.tray_id == 0);
    CHECK(t.filament_type.empty());
    CHECK(t.unknown_filament);
    CHECK_FALSE(t.ready());
    CHECK(t.label() == "A0"); // letter follows list order, not canvas_id
    CHECK(info.units[0].canvas_id == 1);

    CHECK_THROWS_AS(parse_canvas_info(json::object()), std::invalid_argument);
    CHECK_THROWS_AS(parse_canvas_info(json{{"canvas_list", "x"}}), std::invalid_argument);
    CHECK_THROWS_AS(parse_canvas_info(json{{"canvas_list", json::object()}}), std::invalid_argument);
    Info empty = parse_canvas_info(json{{"canvas_list", json::array()}});
    CHECK_FALSE(empty.connected());
}

TEST_CASE("label letters follow unit order", "[elegoo_canvas]")
{
    json b = json{{"canvas_list",
                   json::array({json{{"canvas_id", 0}, {"connected", 1}, {"tray_list", json::array()}},
                                json{{"canvas_id", 1}, {"connected", 0}, {"tray_list", json::array({tray(3, 2, "PETG", "PETG", "#112233")})}}})}};
    Info info = parse_canvas_info(b);
    CHECK(info.connected());
    const Tray *t = info.find(1, 3);
    REQUIRE(t != nullptr);
    CHECK(t->label() == "B3");
    CHECK(t->canvas_id == 1);
}

TEST_CASE("slot_map_json", "[elegoo_canvas]")
{
    json j = slot_map_json({{0, 0, 1}, {1, 0, 3}});
    CHECK(j == json::parse(R"([{"t":0,"canvas_id":0,"tray_id":1},{"t":1,"canvas_id":0,"tray_id":3}])"));
    CHECK(slot_map_json({}) == json::array());
}

TEST_CASE("info_json has the API shape", "[elegoo_canvas]")
{
    Info info = parse_canvas_info(one_tray_info(tray(1, 2, "PLA", "PLA Basic", "#bcbcbc")));
    info.firmware = "1.2.3";
    json j = info_json(info);
    CHECK(j["connected"] == true);
    CHECK(j["active"]["canvas_id"] == 0);
    CHECK(j["active"]["tray_id"] == 1);
    CHECK(j["auto_refill"] == false);
    CHECK(j["firmware"] == "1.2.3");
    REQUIRE(j["trays"].size() == 1);
    const json &t = j["trays"][0];
    CHECK(t["canvas_id"] == 0);
    CHECK(t["tray_id"] == 1);
    CHECK(t["label"] == "A1");
    CHECK(t["status"] == "loaded");
    CHECK(t["ready"] == true);
    CHECK(t["unknown_filament"] == false);
    CHECK(t["filament_type"] == "PLA");
    CHECK(t["filament_name"] == "PLA Basic");
    CHECK(t["brand"] == "Elegoo");
    CHECK(t["filament_color"] == "#BCBCBC");
    CHECK(t["min_nozzle_temp"] == 190);
    CHECK(t["max_nozzle_temp"] == 230);

    auto word = [](int s) { return info_json(parse_canvas_info(one_tray_info(tray(0, s, "PLA", "x", "#000000"))))["trays"][0]["status"]; };
    CHECK(word(0) == "disconnected");
    CHECK(word(1) == "preloaded");
    CHECK(word(2) == "loaded");
    CHECK(word(9) == "error");
}
