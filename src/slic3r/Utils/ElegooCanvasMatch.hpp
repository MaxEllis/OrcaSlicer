#ifndef slic3r_ElegooCanvasMatch_hpp_
#define slic3r_ElegooCanvasMatch_hpp_

// Matching project filaments to CANVAS trays, and the remembered-mapping file.
// Pure functions over the Task 3 data model; no preset or data_dir dependencies.
// The rules and the mapping file shape follow ElegooSlicer's PrinterMmsManager
// (https://github.com/elegooofficial/ElegooSlicer, AGPL-3.0).

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem/path.hpp>

#include "ElegooCanvas.hpp"
#include "ElegooColorBuckets.hpp" // color_bucket()

namespace Slic3r { namespace ElegooCanvas {

struct ProjectFilament
{
    int         index = 0; // 0-based position in the project
    std::string filament_id, setting_id, vendor, name, alias, type, color;
    bool        library = false; // an Orca filament library preset (match_tray_preset prefers the printer vendor's own)
};

enum class MatchSource { None, Saved, Matched, User, Api };

struct Mapping
{
    int                                index = 0;
    std::optional<std::pair<int, int>> tray; // canvas_id, tray_id
    MatchSource                        source = MatchSource::None;
    std::vector<std::string>           warnings; // "type_mismatch"
};

struct SavedMapping
{
    std::string type, name, color; // mappedFilamentType / Name / Color
};

class MappingStore
{
public:
    explicit MappingStore(boost::filesystem::path file);

    // Saved choice for this filament as (mappedFilamentName, mappedFilamentColor).
    std::optional<std::pair<std::string, std::string>> lookup(const ProjectFilament &f) const;
    std::optional<SavedMapping>                        lookup_entry(const ProjectFilament &f) const;
    // Remember that `f` was mapped to `t`. Ignored when either colour has no bucket or the tray lacks name/colour.
    void remember(const ProjectFilament &f, const Tray &t);
    void save() const; // creates the parent directory; throws nothing, logs on failure
    void load();       // missing file = empty store, corrupt file = empty store + warning

private:
    boost::filesystem::path m_file;
    nlohmann::json        m_json = nlohmann::json::object();
};

std::vector<Mapping>     auto_match(const std::vector<ProjectFilament> &, const Info &, const MappingStore &);
// Filament numbers in messages are 1-based (Mapping::index + 1).
std::vector<std::string> validate(const std::vector<Mapping> &, const Info &);

// The filament preset a tray stands for, for the sidebar sync. `presets` describe the candidate presets
// (vendor = the filament's own vendor, name, alias, type, library are read; the rest is ignored). Passes, as
// ElegooSlicer's matchFilamentPreset:
//   1. standardised names equal (upper-case, vendor word and " @..." suffix dropped, '-' as ' '; exact, never a
//      prefix), preset type equal to the tray's or of its family (PLA / PLA-CF; PETG is not PET-CF); best = vendor
//      equals the tray's brand, then vendor "Elegoo", then generic, then any other;
//   2. preset name equals the tray's type or the tray's name equals the preset type, same type only;
//   3. same type, exactly.
// Passes 2 and 3 rank the tray's brand, then generic ("Generic" vendor/name, Orca filament library generic), then
// any other. Within a rank a printer-vendor preset beats an Orca filament library one, then list order.
// A tray with a known type never resolves to a preset of an unrelated type.
// Returns the position in `presets`, or nullopt (tray without a name, or nothing matches).
std::optional<size_t> match_tray_preset(const Tray &tray, const std::vector<ProjectFilament> &presets);

}} // namespace Slic3r::ElegooCanvas

#endif
