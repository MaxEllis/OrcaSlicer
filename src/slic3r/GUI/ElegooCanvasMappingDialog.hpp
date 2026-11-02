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
