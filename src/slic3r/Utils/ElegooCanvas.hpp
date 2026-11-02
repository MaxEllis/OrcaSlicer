#ifndef slic3r_ElegooCanvas_hpp_
#define slic3r_ElegooCanvas_hpp_

// Elegoo CANVAS (multi-material unit) data model. Pure and transport-free:
// parses the printer's `canvas_info` JSON object and builds the API/slot_map JSON.

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r { namespace ElegooCanvas {

enum class TrayStatus { Disconnected, Preloaded, Loaded, Error };

struct Tray
{
    int         canvas_id = 0;
    int         tray_id   = 0;
    int         unit_index = 0; // position of the owning unit in Info::units (drives the label letter)
    std::string brand, filament_type, filament_name, filament_code;
    std::string filament_color; // "#RRGGBB" upper-case, or "" when missing/invalid
    int         min_nozzle_temp = 0;
    int         max_nozzle_temp = 0;
    TrayStatus  status          = TrayStatus::Disconnected;
    bool        unknown_filament = false;

    // Status Preloaded/Loaded and type, name and colour all known.
    bool ready() const;
    // Unit letter (by unit order) + tray_id, e.g. "A1".
    std::string label() const;
};

struct Unit
{
    int               canvas_id = 0;
    bool              connected = false;
    std::vector<Tray> trays;
};

struct Info
{
    int               active_canvas_id = -1;
    int               active_tray_id   = -1;
    bool              auto_refill      = false;
    std::vector<Unit> units;
    std::string       firmware;

    // True when at least one unit is connected.
    bool connected() const;
    const Tray *find(int canvas_id, int tray_id) const;
};

// Throws std::invalid_argument only when `canvas_list` is missing or not an array.
Info parse_canvas_info(const nlohmann::json &canvas_info);

struct SlotMapEntry
{
    int t;
    int canvas_id;
    int tray_id;
};

nlohmann::json slot_map_json(const std::vector<SlotMapEntry> &entries);
nlohmann::json info_json(const Info &info);

}} // namespace Slic3r::ElegooCanvas

#endif
