#ifndef slic3r_GUI_ElegooCanvasState_hpp_
#define slic3r_GUI_ElegooCanvasState_hpp_

// In-memory CANVAS state shared by the send flow and the Remote API: the last slot list read from the printer and
// the mapping the API set for the current project. Pure (no wx) so it can be unit-tested. Only touched on the GUI
// thread (the API marshals its handlers there), so it carries no locking.

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "slic3r/Utils/ElegooCanvas.hpp"
#include "slic3r/Utils/ElegooCanvasMatch.hpp"

namespace Slic3r { namespace GUI {

struct CanvasState
{
    static CanvasState &get()
    {
        static CanvasState instance;
        return instance;
    }

    // Last successful slot read and the printer it came from. Read it through cached_info(): switching the printer
    // preset to another printer must not let the API validate against the old printer's trays.
    std::optional<ElegooCanvas::Info>   last_info;
    std::string                         last_info_printer; // make_printer_key() of the printer last_info was read from
    std::vector<ElegooCanvas::Mapping>  api_mapping;       // set over the API; pre-fills the next send's dialog
    std::string                         project_key;       // project the api_mapping belongs to

    // Identity of a printer for the slot cache: its model and the host the slots are read from.
    static std::string make_printer_key(const std::string &printer_model, const std::string &print_host)
    {
        return printer_model + "|" + print_host;
    }

    void store_info(const std::string &printer_key, const ElegooCanvas::Info &info)
    {
        last_info         = info;
        last_info_printer = printer_key;
    }

    // A failed read: whatever was cached may be stale.
    void clear_info()
    {
        last_info.reset();
        last_info_printer.clear();
    }

    // The cached slot list when it was read from `printer_key`; nullopt when there is none or it is another printer's.
    std::optional<ElegooCanvas::Info> cached_info(const std::string &printer_key) const
    {
        if (!last_info.has_value() || last_info_printer != printer_key)
            return std::nullopt;
        return last_info;
    }

    // Project path ("" when unsaved) + ":" + filament count. A new project, a save under another name or a changed
    // filament count gives a new key.
    static std::string make_project_key(const std::string &project_path, size_t filament_count)
    {
        return project_path + ":" + std::to_string(filament_count);
    }

    // Drops the API-set mapping when `key` names another project (or filament count) than the stored one.
    void reset_if_project_changed(const std::string &key)
    {
        if (key == project_key)
            return;
        project_key = key;
        api_mapping.clear();
    }
};

}} // namespace Slic3r::GUI

#endif
