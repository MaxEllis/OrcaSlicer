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

    std::optional<ElegooCanvas::Info>   last_info;   // last successful slot read
    std::vector<ElegooCanvas::Mapping>  api_mapping; // set over the API; pre-fills the next send's dialog
    std::string                         project_key; // project the api_mapping belongs to

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
