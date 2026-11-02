#ifndef slic3r_GUI_ElegooCanvasMappingDialog_hpp_
#define slic3r_GUI_ElegooCanvasMappingDialog_hpp_

// Elegoo CANVAS: before a multi-filament print is sent to a Centauri Carbon 2, the user maps each project filament
// to a loaded tray. The rows are pre-filled (API-set mapping, saved choices, matcher) and Send stays disabled until
// ElegooCanvas::validate() has nothing to say.

#include <string>
#include <utility>
#include <vector>

#include <wx/bitmap.h>
#include <wx/string.h>

#include "GUI_Utils.hpp"
#include "slic3r/Utils/ElegooCanvas.hpp"
#include "slic3r/Utils/ElegooCanvasMatch.hpp"

class wxBitmapComboBox;
class wxButton;
class wxScrolledWindow;
class wxStaticText;

namespace Slic3r {

class DynamicPrintConfig;

namespace GUI {

// Reads the CANVAS slot list for `printer_cfg` on a worker thread. The calling (GUI) thread shows a busy cursor,
// disables every window and keeps repainting until the worker ends; nothing modal is shown from the worker.
// On success stores the result in CanvasState::last_info too. On failure returns false with a message in `error`.
bool elegoo_canvas_read_info(const DynamicPrintConfig &printer_cfg, ElegooCanvas::Info &out, wxString &error);

// Shared by the send flow and the Remote API (GUI thread only: they read the preset bundle and the plater).
//
// The project's filaments as matcher input; index = position in the project (the G-code T number). `plate_filaments`
// (1-based, as PartPlate::get_extruders() returns them) limits the list to a plate's filaments; empty = all of them.
std::vector<ElegooCanvas::ProjectFilament> elegoo_canvas_project_filaments(const std::vector<int> &plate_filaments);
// The plate a Send of `plate_idx` maps: PLATE_ALL_IDX and PLATE_CURRENT_IDX mean the current plate.
int elegoo_canvas_send_plate_index(int plate_idx);
// That plate's filaments as PartPlate::get_extruders(true) returns them (1-based, custom G-code tool changes
// included); empty when there is no such plate.
std::vector<int> elegoo_canvas_send_plate_filaments(int plate_idx);
// CanvasState::project_key of the open project (project file + filament count).
std::string elegoo_canvas_project_key();
// The remembered-mapping file (<data_dir>/user/filament_mms_mapping.json), loaded.
ElegooCanvas::MappingStore elegoo_canvas_mapping_store();
// One mapping per filament, in the order of `filaments`, taken from `initial` by index (none when absent).
std::vector<ElegooCanvas::Mapping> elegoo_canvas_rows(const std::vector<ElegooCanvas::ProjectFilament> &filaments,
                                                      const std::vector<ElegooCanvas::Mapping>         &initial);
// Nozzle temperature of the named filament preset, 0 when unknown.
int elegoo_canvas_nozzle_temp(const std::string &filament_preset_name);
// Warning codes for filament `f` fed from tray `t`: "type_mismatch" (auto_match's rule) and "temp_out_of_range"
// (`nozzle_temp` outside the tray's range, when all three are known).
std::vector<std::string> elegoo_canvas_mapping_warnings(const ElegooCanvas::ProjectFilament &f, const ElegooCanvas::Tray &t,
                                                        int nozzle_temp);

class ElegooCanvasMappingDialog : public DPIDialog
{
public:
    ElegooCanvasMappingDialog(wxWindow                                         *parent,
                              const std::vector<ElegooCanvas::ProjectFilament> &filaments,
                              const ElegooCanvas::Info                         &info,
                              std::vector<ElegooCanvas::Mapping>                initial);

    // One entry per mapped row: t = the filament's project index.
    std::vector<ElegooCanvas::SlotMapEntry> slot_map() const;
    // One row per filament, in project order; rows the user changed carry source = User.
    std::vector<ElegooCanvas::Mapping> mapping() const { return m_mappings; }
    // The slot list the rows refer to (newer than the constructor's after "Refresh from printer").
    const ElegooCanvas::Info &info() const { return m_info; }

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    struct Row
    {
        wxBitmapComboBox                *combo   = nullptr;
        wxStaticText                    *warning = nullptr;
        std::vector<std::pair<int, int>> choices; // combo item n (n >= 1) -> choices[n - 1]; item 0 = not mapped
    };

    void     build_rows();
    void     drop_unusable_trays();
    void     on_tray_selected(size_t row);
    void     on_refresh();
    void     on_send();
    void     update_row_warning(size_t row);
    void     update_validation();
    wxBitmap swatch(const std::string &color) const;

    std::vector<ElegooCanvas::ProjectFilament> m_filaments;
    std::vector<int>                           m_nozzle_temps; // first nozzle_temperature of each filament preset, 0 = unknown
    ElegooCanvas::Info                         m_info;
    std::vector<ElegooCanvas::Mapping>         m_mappings; // same order and size as m_filaments
    std::vector<Row>                           m_rows;

    wxScrolledWindow *m_rows_panel  = nullptr;
    wxStaticText     *m_validation  = nullptr;
    wxButton         *m_btn_refresh = nullptr;
    wxButton         *m_btn_send    = nullptr;
};

} // namespace GUI
} // namespace Slic3r

#endif
