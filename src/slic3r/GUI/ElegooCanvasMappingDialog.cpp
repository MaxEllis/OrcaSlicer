#include "ElegooCanvasMappingDialog.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <exception>
#include <memory>
#include <thread>

#include <wx/bmpcbox.h>
#include <wx/button.h>
#include <wx/colour.h>
#include <wx/dcmemory.h>
#include <wx/evtloop.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/utils.h>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/ElegooLink.hpp"

#include "ElegooCanvasState.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "format.hpp"

namespace Slic3r { namespace GUI {

namespace {

// Same comparison as ElegooCanvas::auto_match's type_mismatch rule.
std::string upper_trim(const std::string &s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    std::string out = s.substr(b, e - b);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

} // namespace

bool elegoo_canvas_read_info(const DynamicPrintConfig &printer_cfg, ElegooCanvas::Info &out, wxString &error)
{
    struct Shared
    {
        std::atomic<bool>             done{false};
        ElegooCanvas::Info            info;
        ElegooCanvas::Session::Result result;
    };
    auto shared = std::make_shared<Shared>();

    // The worker owns a copy of the printer config; MQTT and the HTTP serial lookup never run on the GUI thread.
    std::thread worker([shared, cfg = printer_cfg]() mutable {
        try {
            ElegooLink link(&cfg);
            shared->result = link.get_canvas_info(shared->info);
        } catch (const std::exception &e) {
            shared->result.ok         = false;
            shared->result.error_code = "unreachable";
            shared->result.message    = e.what();
        } catch (...) {
            shared->result.ok         = false;
            shared->result.error_code = "unreachable";
            shared->result.message    = "unknown error";
        }
        shared->done.store(true);
    });

    {
        // Busy state: no input reaches any window (so the send cannot be re-entered), but the UI keeps painting.
        wxBusyCursor     busy;
        wxWindowDisabler disabler;
        while (!shared->done.load()) {
            if (wxEventLoopBase *loop = wxEventLoopBase::GetActive(); loop != nullptr)
                loop->YieldFor(wxEVT_CATEGORY_UI);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
    }
    worker.join();

    if (!shared->result.ok) {
        const std::string &msg = shared->result.message.empty() ? shared->result.error_code : shared->result.message;
        error = msg.empty() ? _L("The printer did not answer.") : from_u8(msg);
        return false;
    }
    out                          = shared->info;
    CanvasState::get().last_info = shared->info;
    return true;
}

ElegooCanvasMappingDialog::ElegooCanvasMappingDialog(wxWindow                                         *parent,
                                                     const std::vector<ElegooCanvas::ProjectFilament> &filaments,
                                                     const ElegooCanvas::Info                         &info,
                                                     std::vector<ElegooCanvas::Mapping>                initial)
    : DPIDialog(parent, wxID_ANY, _L("Map filaments to CANVAS trays"), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_filaments(filaments)
    , m_info(info)
{
    // Nozzle temperature of each filament's preset, for the tray range warning.
    const PresetCollection &filament_presets = wxGetApp().preset_bundle->filaments;
    m_nozzle_temps.reserve(m_filaments.size());
    for (const ElegooCanvas::ProjectFilament &f : m_filaments) {
        int temp = 0;
        if (const Preset *preset = filament_presets.find_preset(f.name, false); preset != nullptr)
            if (const auto *opt = preset->config.option<ConfigOptionInts>("nozzle_temperature"); opt != nullptr && !opt->values.empty())
                temp = opt->values.front();
        m_nozzle_temps.push_back(temp);
    }

    // One mapping per filament, in project order, taken from `initial` by index.
    m_mappings.reserve(m_filaments.size());
    for (const ElegooCanvas::ProjectFilament &f : m_filaments) {
        ElegooCanvas::Mapping m;
        m.index = f.index;
        auto it = std::find_if(initial.begin(), initial.end(), [&f](const ElegooCanvas::Mapping &im) { return im.index == f.index; });
        if (it != initial.end()) {
            m.tray     = it->tray;
            m.source   = it->source;
            m.warnings = it->warnings;
        }
        m_mappings.push_back(std::move(m));
    }
    drop_unusable_trays();

    const int em  = em_unit();
    auto     *top = new wxBoxSizer(wxVERTICAL);

    auto *intro = new wxStaticText(this, wxID_ANY, _L("Choose the CANVAS tray that feeds each filament of this project."));
    top->Add(intro, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em);

    m_rows_panel = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_rows_panel->SetScrollRate(0, em);
    top->Add(m_rows_panel, 1, wxEXPAND | wxALL, em);

    m_validation = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_validation->SetForegroundColour(wxColour(0xE1, 0x4B, 0x4B));
    top->Add(m_validation, 0, wxEXPAND | wxLEFT | wxRIGHT, em);

    auto *buttons = new wxBoxSizer(wxHORIZONTAL);
    m_btn_refresh = new wxButton(this, wxID_ANY, _L("Refresh from printer"));
    m_btn_send    = new wxButton(this, wxID_OK, _L("Send"));
    auto *cancel  = new wxButton(this, wxID_CANCEL, _L("Cancel"));
    buttons->Add(m_btn_refresh, 0, wxALIGN_CENTER_VERTICAL);
    buttons->AddStretchSpacer(1);
    buttons->Add(m_btn_send, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em);
    buttons->Add(cancel, 0, wxALIGN_CENTER_VERTICAL);
    top->Add(buttons, 0, wxEXPAND | wxALL, em);

    m_btn_refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_refresh(); });
    m_btn_send->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_send(); });

    SetSizer(top);
    build_rows();
    update_validation();
    SetMinSize(wxSize(60 * em, -1));
    Fit();
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

std::vector<ElegooCanvas::SlotMapEntry> ElegooCanvasMappingDialog::slot_map() const
{
    std::vector<ElegooCanvas::SlotMapEntry> out;
    for (size_t i = 0; i < m_mappings.size(); ++i)
        if (m_mappings[i].tray)
            out.push_back({m_filaments[i].index, m_mappings[i].tray->first, m_mappings[i].tray->second});
    return out;
}

// A tray the dropdowns cannot show (gone, or not ready) is dropped so the row reads "not mapped"; validate() then
// keeps Send disabled until the user picks a ready tray.
void ElegooCanvasMappingDialog::drop_unusable_trays()
{
    for (ElegooCanvas::Mapping &m : m_mappings) {
        if (!m.tray)
            continue;
        const ElegooCanvas::Tray *t = m_info.find(m.tray->first, m.tray->second);
        if (t == nullptr || !t->ready()) {
            m.tray.reset();
            m.warnings.clear();
            if (m.source != ElegooCanvas::MatchSource::User)
                m.source = ElegooCanvas::MatchSource::None;
        }
    }
}

wxBitmap ElegooCanvasMappingDialog::swatch(const std::string &color) const
{
    const int w = FromDIP(16);
    const int h = FromDIP(16);
    wxBitmap  bmp(w, h);
    {
        wxMemoryDC dc(bmp);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();
        wxColour   c;
        const bool valid = !color.empty() && c.Set(wxString::FromUTF8(color.c_str()));
        dc.SetPen(wxPen(wxColour(0x90, 0x90, 0x90)));
        dc.SetBrush(valid ? wxBrush(c) : *wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(0, 0, w, h);
        dc.SelectObject(wxNullBitmap);
    }
    return bmp;
}

void ElegooCanvasMappingDialog::build_rows()
{
    const int em = em_unit();
    m_rows_panel->DestroyChildren(); // child windows detach themselves from the old sizer, which SetSizer() deletes
    m_rows.assign(m_filaments.size(), Row{});

    std::vector<const ElegooCanvas::Tray *> ready;
    for (const ElegooCanvas::Unit &unit : m_info.units)
        for (const ElegooCanvas::Tray &tray : unit.trays)
            if (tray.ready())
                ready.push_back(&tray);

    // Columns: swatch | name | type | tray dropdown; a warning line sits under each dropdown.
    auto *grid = new wxFlexGridSizer(4, em / 2, em);
    grid->AddGrowableCol(3, 1);

    for (size_t i = 0; i < m_filaments.size(); ++i) {
        const ElegooCanvas::ProjectFilament &f   = m_filaments[i];
        Row                                 &row = m_rows[i];

        grid->Add(new wxStaticBitmap(m_rows_panel, wxID_ANY, swatch(f.color)), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(new wxStaticText(m_rows_panel, wxID_ANY, from_u8(std::to_string(f.index + 1) + ". " + f.name)), 0,
                  wxALIGN_CENTER_VERTICAL);
        grid->Add(new wxStaticText(m_rows_panel, wxID_ANY, from_u8(f.type)), 0, wxALIGN_CENTER_VERTICAL);

        row.combo = new wxBitmapComboBox(m_rows_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(28 * em, -1), 0, nullptr,
                                         wxCB_READONLY);
        row.combo->Append(_L("(not mapped)"), swatch(std::string()));
        int selection = 0;
        for (const ElegooCanvas::Tray *t : ready) {
            row.combo->Append(from_u8(t->label() + "   " + t->filament_type + "   " + t->filament_name), swatch(t->filament_color));
            row.choices.emplace_back(t->canvas_id, t->tray_id);
            if (m_mappings[i].tray && *m_mappings[i].tray == row.choices.back())
                selection = static_cast<int>(row.choices.size());
        }
        row.combo->SetSelection(selection);
        row.combo->Bind(wxEVT_COMBOBOX, [this, i](wxCommandEvent &) { on_tray_selected(i); });
        grid->Add(row.combo, 0, wxEXPAND);

        grid->AddSpacer(0);
        grid->AddSpacer(0);
        grid->AddSpacer(0);
        row.warning = new wxStaticText(m_rows_panel, wxID_ANY, wxEmptyString);
        row.warning->SetForegroundColour(wxColour(0xE0, 0x8A, 0x00));
        grid->Add(row.warning, 0, wxEXPAND);

        update_row_warning(i);
    }

    m_rows_panel->SetSizer(grid);
    m_rows_panel->FitInside();
    const wxSize min = grid->GetMinSize();
    m_rows_panel->SetMinSize(wxSize(min.x + 2 * em, std::min(min.y, 30 * em)));
    wxGetApp().UpdateDlgDarkUI(this);
}

void ElegooCanvasMappingDialog::on_tray_selected(size_t row)
{
    if (row >= m_rows.size())
        return;
    const int              sel = m_rows[row].combo->GetSelection();
    ElegooCanvas::Mapping &m   = m_mappings[row];
    if (sel <= 0 || static_cast<size_t>(sel) > m_rows[row].choices.size())
        m.tray.reset();
    else
        m.tray = m_rows[row].choices[static_cast<size_t>(sel) - 1];
    m.source = ElegooCanvas::MatchSource::User;
    update_row_warning(row);
    update_validation();
}

// Recomputes the row's warnings for the tray it now points at: "type_mismatch" (auto_match's rule) and
// "temp_out_of_range" (the preset's nozzle temperature outside the tray's range, when both ends are known).
void ElegooCanvasMappingDialog::update_row_warning(size_t row)
{
    ElegooCanvas::Mapping              &m = m_mappings[row];
    const ElegooCanvas::ProjectFilament &f = m_filaments[row];
    const ElegooCanvas::Tray           *t = m.tray ? m_info.find(m.tray->first, m.tray->second) : nullptr;

    m.warnings.clear();
    wxString text;
    if (t != nullptr) {
        if (upper_trim(t->filament_type) != upper_trim(f.type)) {
            m.warnings.push_back("type_mismatch");
            text = format_wxstr(_L("Type mismatch: the project uses %1%, the tray holds %2%."), f.type, t->filament_type);
        }
        const int temp = m_nozzle_temps[row];
        if (temp > 0 && t->min_nozzle_temp > 0 && t->max_nozzle_temp > 0 && (temp < t->min_nozzle_temp || temp > t->max_nozzle_temp)) {
            m.warnings.push_back("temp_out_of_range");
            if (!text.empty())
                text += "\n";
            text += format_wxstr(_L("Nozzle temperature %1%℃ is outside the tray's range (%2%-%3%℃)."), temp, t->min_nozzle_temp,
                                 t->max_nozzle_temp);
        }
    }
    Row &r = m_rows[row];
    if (r.warning != nullptr) {
        r.warning->SetLabel(text);
        r.warning->Show(!text.empty());
    }
}

void ElegooCanvasMappingDialog::update_validation()
{
    const std::vector<std::string> msgs = ElegooCanvas::validate(m_mappings, m_info);
    wxString                       text;
    for (const std::string &msg : msgs) {
        if (!text.empty())
            text += "\n";
        text += from_u8(msg);
    }
    m_validation->SetLabel(text);
    m_validation->Wrap(56 * em_unit());
    m_validation->Show(!text.empty());
    m_btn_send->Enable(msgs.empty());
    m_rows_panel->Layout();
    Layout();
}

void ElegooCanvasMappingDialog::on_refresh()
{
    DynamicPrintConfig cfg = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    ElegooCanvas::Info info;
    wxString           error;
    if (!elegoo_canvas_read_info(cfg, info, error)) {
        MessageDialog dlg(this, error, _L("Refresh from printer"), wxOK);
        dlg.ShowModal();
        return;
    }
    m_info = std::move(info);
    drop_unusable_trays();
    build_rows();
    update_validation();
    Fit();
}

void ElegooCanvasMappingDialog::on_send()
{
    // Send is disabled while validate() reports anything; re-check in case a key press reaches it anyway.
    if (!ElegooCanvas::validate(m_mappings, m_info).empty()) {
        update_validation();
        return;
    }
    EndModal(wxID_OK);
}

void ElegooCanvasMappingDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    build_rows(); // swatches and widths follow the new scale
    update_validation();
    Fit();
    Refresh();
    if (!suggested_rect.IsEmpty())
        SetSize(suggested_rect.GetSize());
}

}} // namespace Slic3r::GUI
