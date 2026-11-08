#include "ElegooCanvas.hpp"

#include <cctype>
#include <stdexcept>

namespace Slic3r { namespace ElegooCanvas {

using nlohmann::json;

namespace {

int get_int(const json &o, const char *key, int def)
{
    auto it = o.find(key);
    if (it == o.end())
        return def;
    if (it->is_number_integer() || it->is_number_unsigned())
        return it->get<int>();
    if (it->is_number_float())
        return static_cast<int>(it->get<double>());
    if (it->is_boolean())
        return it->get<bool>() ? 1 : 0;
    return def;
}

bool get_bool(const json &o, const char *key, bool def)
{
    auto it = o.find(key);
    if (it == o.end())
        return def;
    if (it->is_boolean())
        return it->get<bool>();
    if (it->is_number())
        return it->get<double>() != 0.0;
    return def;
}

std::string get_str(const json &o, const char *key)
{
    auto it = o.find(key);
    return (it != o.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

std::string normalize_color(const std::string &c)
{
    if (c.size() != 7 || c[0] != '#')
        return {};
    std::string out = "#";
    for (size_t i = 1; i < c.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(c[i]);
        if (!std::isxdigit(ch))
            return {};
        out += static_cast<char>(std::toupper(ch));
    }
    return out;
}

TrayStatus to_status(int s)
{
    switch (s) {
    case 0: return TrayStatus::Disconnected;
    case 1: return TrayStatus::Preloaded;
    case 2: return TrayStatus::Loaded;
    default: return TrayStatus::Error;
    }
}

const char *status_word(TrayStatus s)
{
    switch (s) {
    case TrayStatus::Disconnected: return "disconnected";
    case TrayStatus::Preloaded: return "preloaded";
    case TrayStatus::Loaded: return "loaded";
    default: return "error";
    }
}

} // namespace

bool Tray::ready() const
{
    return unit_connected && (status == TrayStatus::Preloaded || status == TrayStatus::Loaded) && !filament_type.empty() &&
           !filament_name.empty() && !filament_color.empty();
}

std::string Tray::label() const
{
    std::string s(1, static_cast<char>('A' + (unit_index < 0 ? 0 : unit_index % 26)));
    return s + std::to_string(tray_id);
}

bool Info::connected() const
{
    for (const Unit &u : units)
        if (u.connected)
            return true;
    return false;
}

const Tray *Info::find(int canvas_id, int tray_id) const
{
    for (const Unit &u : units)
        if (u.canvas_id == canvas_id)
            for (const Tray &t : u.trays)
                if (t.tray_id == tray_id)
                    return &t;
    return nullptr;
}

Info parse_canvas_info(const json &canvas_info)
{
    if (!canvas_info.is_object())
        throw std::invalid_argument("canvas_info: not an object");
    auto list = canvas_info.find("canvas_list");
    if (list == canvas_info.end() || !list->is_array())
        throw std::invalid_argument("canvas_info: canvas_list missing or not an array");

    Info info;
    info.active_canvas_id = get_int(canvas_info, "active_canvas_id", -1);
    info.active_tray_id   = get_int(canvas_info, "active_tray_id", -1);
    info.auto_refill      = get_bool(canvas_info, "auto_refill", false);

    for (const json &ju : *list) {
        if (!ju.is_object())
            continue;
        Unit u;
        u.canvas_id = get_int(ju, "canvas_id", 0);
        u.connected = get_bool(ju, "connected", false);
        const int unit_index = static_cast<int>(info.units.size());
        auto tl = ju.find("tray_list");
        if (tl != ju.end() && tl->is_array()) {
            for (const json &jt : *tl) {
                if (!jt.is_object())
                    continue;
                Tray t;
                t.canvas_id       = u.canvas_id;
                t.unit_index      = unit_index;
                t.unit_connected  = u.connected;
                t.tray_id         = get_int(jt, "tray_id", 0);
                t.brand           = get_str(jt, "brand");
                t.filament_type   = get_str(jt, "filament_type");
                t.filament_name   = get_str(jt, "filament_name");
                t.filament_code   = get_str(jt, "filament_code");
                t.filament_color  = normalize_color(get_str(jt, "filament_color"));
                t.min_nozzle_temp = get_int(jt, "min_nozzle_temp", 0);
                t.max_nozzle_temp = get_int(jt, "max_nozzle_temp", 0);
                t.status          = to_status(get_int(jt, "status", 0));
                t.unknown_filament = t.filament_type.empty() || t.filament_name.empty();
                u.trays.push_back(std::move(t));
            }
        }
        info.units.push_back(std::move(u));
    }
    return info;
}

json slot_map_json(const std::vector<SlotMapEntry> &entries)
{
    json out = json::array();
    for (const SlotMapEntry &e : entries)
        out.push_back(json{{"t", e.t}, {"canvas_id", e.canvas_id}, {"tray_id", e.tray_id}});
    return out;
}

json info_json(const Info &info)
{
    json trays = json::array();
    for (const Unit &u : info.units)
        for (const Tray &t : u.trays)
            trays.push_back(json{{"canvas_id", t.canvas_id},
                                 {"tray_id", t.tray_id},
                                 {"label", t.label()},
                                 {"status", status_word(t.status)},
                                 {"ready", t.ready()},
                                 {"unknown_filament", t.unknown_filament},
                                 {"filament_type", t.filament_type},
                                 {"filament_name", t.filament_name},
                                 {"brand", t.brand},
                                 {"filament_color", t.filament_color},
                                 {"min_nozzle_temp", t.min_nozzle_temp},
                                 {"max_nozzle_temp", t.max_nozzle_temp}});
    return json{{"connected", info.connected()},
                {"active", json{{"canvas_id", info.active_canvas_id}, {"tray_id", info.active_tray_id}}},
                {"auto_refill", info.auto_refill},
                {"firmware", info.firmware},
                {"trays", std::move(trays)}};
}

}} // namespace Slic3r::ElegooCanvas
