// The GUI side of the user-change journal (McpJournal.hpp): reads the project into an McpBaseline. GUI thread only.
#include "McpJournal.hpp"
#include "McpRecorder.hpp" // hhmm / now_s: the same clock (and test offset) as the timeline
#include "RemoteAPIController.hpp" // sensitive_config_key

#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <exception>
#include <set>

namespace Slic3r { namespace GUI { namespace RemoteAPI {

namespace {

// One pass over the edited preset's keys: its value, and the saved value where it was edited on top.
void snap_edited(const PresetCollection &c, McpConfigSnap &s)
{
    const Preset &ed  = c.get_edited_preset();
    const Preset &sel = c.get_selected_preset();
    s.preset          = c.get_selected_preset_name();
    for (const std::string &k : ed.config.keys()) {
        const ConfigOption *o = ed.config.option(k);
        if (o == nullptr) continue;
        s.values.emplace(k, o->serialize());
        const ConfigOption *so = sel.config.option(k);
        if (so != nullptr && !(*so == *o)) s.saved.emplace(k, so->serialize());
    }
}

void snap_saved(const Preset &p, McpConfigSnap &s)
{
    s.preset = p.name;
    for (const std::string &k : p.config.keys())
        if (const ConfigOption *o = p.config.option(k)) s.values.emplace(k, o->serialize());
}

std::vector<McpObjectSnap> capture_objects(Plater *plater, int &plates)
{
    std::vector<McpObjectSnap> out;
    plates = 0;
    if (plater == nullptr) return out;
    for (const ModelObject *mo : plater->model().objects) {
        McpObjectSnap o;
        o.id   = (uint64_t) mo->id().id;
        o.name = mo->name;
        for (const std::string &k : mo->config.keys()) o.config.emplace(k, mo->config.opt_serialize(k));
        for (const ModelInstance *mi : mo->instances) {
            o.instance_ids.push_back((uint64_t) mi->id().id);
            const Transform3d     &t = mi->get_matrix();
            std::array<double, 16> m{};
            std::copy(t.matrix().data(), t.matrix().data() + 16, m.begin());
            o.transforms.push_back(m);
        }
        out.push_back(std::move(o));
    }
    plates = plater->get_partplate_list().get_plate_count();
    return out;
}

// Settings are worded as the timeline words them, and a sensitive key's values are never shown. Set before the
// first diff (also when the API is off: a panel action diffs too).
McpJournal &journal()
{
    static bool fns_set = false;
    McpJournal &j       = McpJournal::get();
    if (!fns_set) {
        McpTextFns fns;
        fns.label     = &McpRecorder::config_label;
        fns.value     = &McpRecorder::config_value_text;
        fns.sensitive = &Controller::sensitive_config_key;
        j.set_text_fns(std::move(fns));
        fns_set = true;
    }
    return j;
}

std::string now_hhmm()
{
    McpRecorder &rec = McpRecorder::get();
    return McpRecorder::hhmm(rec.now_s());
}

} // namespace

McpBaseline mcp_capture_baseline()
{
    McpBaseline b;
    McpJournal &j  = McpJournal::get();
    b.project_gen  = j.project_gen();
    b.project_text = j.project_text();
    PresetBundle *bundle = wxGetApp().preset_bundle;
    if (bundle != nullptr) {
        snap_edited(bundle->prints, b.print);
        snap_edited(bundle->printers, b.printer);
        // Only the preset the Filament tab edits can hold unsaved edits; every other slot shows its saved preset.
        const std::string edited = bundle->filaments.get_selected_preset_name();
        b.filament_slots         = bundle->filament_presets;
        for (const std::string &name : b.filament_slots) {
            if (b.filaments.count(name)) continue;
            McpConfigSnap &s = b.filaments[name];
            if (name == edited) {
                snap_edited(bundle->filaments, s);
            } else if (const Preset *p = bundle->filaments.find_preset(name, false)) {
                snap_saved(*p, s);
            } else {
                s.preset = name;
            }
        }
    }
    if (bundle != nullptr) {
        // The project settings PUT /config writes; filament_colour per slot.
        const DynamicPrintConfig &proj = bundle->project_config;
        for (const std::string &k : Controller::writable_project_keys()) {
            if (k == "filament_colour") {
                if (const auto *c = proj.option<ConfigOptionStrings>(k)) b.filament_colours = c->values;
                continue;
            }
            if (const ConfigOption *o = proj.option(k)) b.project.values.emplace(k, o->serialize());
        }
    }
    b.objects = capture_objects(wxGetApp().plater(), b.plates);
    return b;
}

void mcp_journal_begin()
{
    if (wxGetApp().plater() == nullptr || wxGetApp().preset_bundle == nullptr) return;
    try {
        journal().begin_request(mcp_capture_baseline(), now_hhmm());
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "MCP journal: diff failed: " << e.what();
    }
}

void mcp_journal_end()
{
    if (wxGetApp().plater() == nullptr || wxGetApp().preset_bundle == nullptr) return;
    try {
        McpJournal::get().end_request(mcp_capture_baseline());
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "MCP journal: baseline failed: " << e.what();
        McpJournal::get().invalidate();
    }
}

void mcp_journal_panel(const std::string &text)
{
    try {
        McpJournal::get().add("panel", "Panel", text, now_hhmm());
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "MCP journal: panel entry not added: " << e.what();
    }
    mcp_journal_end();
}

std::shared_ptr<int> mcp_journal_api_job_queued() { return McpJournal::get().api_job_started(); }

void mcp_journal_api_objects_moved()
{
    try {
        int  plates = 0;
        auto objs   = capture_objects(wxGetApp().plater(), plates);
        McpJournal::get().rebaseline_objects(std::move(objs), plates);
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "MCP journal: object baseline failed: " << e.what();
    }
}

void mcp_journal_project_opened()
{
    Plater *p = wxGetApp().plater();
    if (p == nullptr) return;
    const std::string name = p->get_project_name().ToUTF8().data();
    McpJournal::get().note_project("Opened project " + name);
}

void mcp_journal_project_new() { McpJournal::get().note_project("New project"); }

}}} // namespace Slic3r::GUI::RemoteAPI
