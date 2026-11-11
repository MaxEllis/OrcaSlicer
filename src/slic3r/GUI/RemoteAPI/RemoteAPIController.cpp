// src/slic3r/GUI/RemoteAPI/RemoteAPIController.cpp
#include "RemoteAPIController.hpp"
#include "McpActions.hpp"
#include "McpJournal.hpp"
#include "McpRecorder.hpp"
#include "McpSettingLabel.hpp"
#include "McpPanel.hpp"
#include "McpText.hpp"
#include "UiTaskGate.hpp"
#include "ConfigRange.hpp"

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r_version.h"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/SettingsIndex.hpp" // the sidebar's line labels and groups for the panel's setting labels
#include "slic3r/GUI/PresetComboBoxes.hpp" // PlaterPresetComboBox::update() for the filament slot combos
#include "libslic3r/Model.hpp"    // M4b: ModelObject/ModelInstance for GET /objects
#include "slic3r/GUI/NotificationManager.hpp" // in-app change notifications
#include "libslic3r/AppConfig.hpp"             // remote_api_notify toggle
#include "slic3r/GUI/Tab.hpp"
#include "slic3r/GUI/BackgroundSlicingProcess.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/BuildVolume.hpp"    // all_paths_inside: expose the plate-boundary warning
#include "libslic3r/Format/bbs_3mf.hpp" // LoadStrategy (used by Plater::load_files)
#include "libslic3r/Slicing.hpp"          // M4c: layer_height_profile_adaptive, t_layer_height_range
#include "slic3r/GUI/GUI_ObjectList.hpp" // M4c: obj_list()->update_info_items
#include "slic3r/GUI/I18N.hpp"     // _L for the object list's "default" filament label
#include "slic3r/GUI/GLCanvas3D.hpp"      // plate_render: offscreen thumbnail/gcode render
#include "slic3r/GUI/Camera.hpp"          // plate_render: ViewAngleType
#include "libslic3r/GCode/ThumbnailData.hpp" // plate_render: ThumbnailData/ThumbnailsParams
#include <miniz.h>                        // plate_render: RGBA -> PNG in memory
#include "libslic3r/FlushVolCalc.hpp"      // project scope: g_max_flush_volume bound
#include "slic3r/GUI/WipeTowerDialog.hpp"  // project scope: is_flush_config_modified
#include "slic3r/GUI/GUI.hpp"              // project files: into_path/from_path for the 3mf paths
#include "slic3r/GUI/MainFrame.hpp"        // project files: the File > Recent list
#include "slic3r/GUI/ElegooCanvasGate.hpp"          // CANVAS: elegoo_canvas_enabled
#include "slic3r/GUI/ElegooCanvasMappingDialog.hpp" // CANVAS: shared project-filament/mapping helpers
#include "slic3r/GUI/ElegooCanvasState.hpp"         // CANVAS: last slot read + API-set mapping
#include "slic3r/GUI/Jobs/Worker.hpp"               // MCP panel: queue_job after arrange / orient
#include "slic3r/Utils/ElegooCanvas.hpp"
#include "slic3r/Utils/ElegooCanvasMatch.hpp"
#include "slic3r/Utils/ElegooLink.hpp"              // CANVAS: get_canvas_info

#include <algorithm>
#include <cmath>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <fstream>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>

namespace Slic3r { namespace GUI { namespace RemoteAPI {

ServerStatus server_status()
{
    ServerStatus st;
    Server &srv     = wxGetApp().remote_api_server();
    st.listening    = srv.running();
    st.start_failed = srv.start_failed();
    st.port         = srv.port();
    st.bind_lan     = srv.bind_lan();
    return st;
}

static Controller *g_controller = nullptr; // set in ctor, cleared in dtor

Controller::Controller()  { g_controller = this; }
Controller::~Controller() { g_controller = nullptr; }

// --- in-app notifications for API-driven changes -----------------------------
// Gated by the remote_api_notify setting (default on). Thread-agnostic: marshals
// to the GUI thread via CallAfter, so it is safe from io threads and from inside
// run_on_ui alike. Deliberately NOT used for slice / arrange / orient - Orca's
// own progress + result notifications already cover those (avoid double-up).
static void api_notify(const std::string &text, bool warning = false)
{
    wxGetApp().CallAfter([text, warning]() {
        auto *ac = wxGetApp().app_config;
        bool enabled = (ac && ac->has("remote_api_notify")) ? ac->get_bool("remote_api_notify") : true;
        if (!enabled) return;
        // The MCP panel lists every change with its undo, so while it is open the
        // plain change toasts would only repeat it. Warnings (refusals) still show.
        if (!warning && McpPanel::is_open()) return;
        Plater *plater = wxGetApp().plater();
        if (plater == nullptr) return;
        NotificationManager *nm = plater->get_notification_manager();
        if (nm == nullptr) return;
        // Coalesce rapid-fire API messages into ONE toast: anything arriving within
        // a 5s quiet window is re-shown together as a list instead of a toast flood.
        // Plain text, ASCII only (the notification font lacks bullet/arrow glyphs);
        // notifications are toggled via Preferences (remote_api_notify).
        static std::vector<std::string> recent;   // GUI thread only
        static bool       recent_warning = false;
        static wxLongLong last_ms = 0;
        wxLongLong now = wxGetLocalTimeMillis();
        if ((now - last_ms).GetValue() > 5000) { recent.clear(); recent_warning = false; }
        last_ms = now;
        recent.push_back(text);
        recent_warning |= warning;

        std::string body;
        if (recent.size() == 1)
            body = recent.front();
        else {
            body = "Remote changes:";
            size_t shown = 0;
            for (const std::string &m : recent) {
                if (shown < 6) {
                    std::string item = m;
                    size_t pos = 0;   // indent detail lines under their list entry
                    while ((pos = item.find('\n', pos)) != std::string::npos) { item.replace(pos, 1, "\n   "); pos += 4; }
                    body += "\n - " + item;
                }
                ++shown;
            }
            if (shown > 6)
                body += "\n(+ " + std::to_string(shown - 6) + " more)";
        }
        nm->close_notification_of_type(NotificationType::RemoteAPIChange);
        nm->push_notification(NotificationType::RemoteAPIChange,
            recent_warning ? NotificationManager::NotificationLevel::WarningNotificationLevel
                           : NotificationManager::NotificationLevel::RegularNotificationLevel,
            body);
    });
}

// Human label / unit for a config key, from OrcaSlicer's own definitions.
static std::string api_config_label(const std::string &key)
{
    const ConfigOptionDef *d = print_config_def.get(key);
    return (d != nullptr && !d->label.empty()) ? d->label : key;
}
static std::string api_config_unit(const std::string &key)
{
    const ConfigOptionDef *d = print_config_def.get(key);
    return (d != nullptr && !d->sidetext.empty()) ? (std::string(" ") + d->sidetext) : std::string();
}

// The MCP panel's rows use McpRecorder::config_label (full_label first: "Nozzle temperature", not "Other
// layers") and McpRecorder::config_value_text (unit and enum label) instead of the two above.

// --- MCP panel: recording API changes ------------------------------------------------------------------------
// Every mutating handler calls mcp_before_change() inside its UI lambda once the request has validated, right
// before the first change (its run_on_ui timeout is 120 s: a checkpoint export can be slow), then records its
// row(s); a refusal or error reply to a change records a refused row. The target schema is documented at the
// top of McpRecorder.hpp. Recording never fails a request: a recorder problem is logged and the change stands.
// (The middle dot, arrow, times sign and en dash the rows show sit inside their MCP_T templates below.)
static const char *const k_deg   = "\xC2\xB0";     // U+00B0
static const std::string k_needs_sensitive =
    MCP_T("Needs Preferences \xE2\x80\xBA Remote API \xE2\x80\xBA Allow script, G-code and connection edits");
static const std::string k_needs_file_changes =
    MCP_T("Needs Preferences \xE2\x80\xBA Remote API \xE2\x80\xBA Allow deleting and replacing project files");

// M4: a request with X-Orca-Mcp-Trial: 1 (Request::trial) is a trial run: no rows, no refusals, no restore points.
// t_io_trial: the request the io thread is dispatching; s_ui_trial: the API task running on the GUI thread came from
// such a request (run_on_ui carries the flag over, UiTask::run sets it around the task).
static thread_local bool t_io_trial = false;
static bool              s_ui_trial = false;

static void mcp_before_change()
{
    if (s_ui_trial) return;
    try {
        McpRecorder::get().before_change();
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "MCP panel: before_change failed: " << e.what();
    }
}

// Object targets get the object's name and the current load stamp: ObjectIDs are renewed by every project load,
// after which the panel finds the object by its name (McpRecorder.hpp, the target schema).
static void mcp_stamp_objects(McpRow &row)
{
    if (!row.target.is_object()) return;
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr) return;
    const Model &model   = plater->model();
    auto         name_of = [&model](nlohmann::json &t) {
        if (!t.is_object() || !t.contains("object_id") || !t["object_id"].is_number_integer()) return false;
        const uint64_t id = t["object_id"].get<uint64_t>();
        for (const ModelObject *mo : model.objects)
            if ((uint64_t) mo->id().id == id) {
                t["object_name"] = mo->name;
                break;
            }
        return true;
    };
    bool any = name_of(row.target);
    if (row.target.contains("items") && row.target["items"].is_array())
        for (nlohmann::json &it : row.target["items"]) any = name_of(it) || any;
    if (any) row.target["load"] = McpRecorder::get().load_stamp();
}

static void mcp_record(McpRow row)
{
    if (s_ui_trial) return;
    try {
        mcp_stamp_objects(row);
        McpRecorder::get().record(std::move(row));
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "MCP panel: row not recorded: " << e.what();
    }
}

static McpRow mcp_row(const char *kind, std::string label, std::string value, std::string before = {},
                      std::string where = {})
{
    McpRow r;
    r.kind   = kind;
    r.label  = std::move(label);
    r.value  = std::move(value);
    r.before = std::move(before);
    r.where  = std::move(where);
    return r;
}

static void mcp_undo(McpRow &r, const char *verb, std::string tip)
{
    r.undo_verb = verb;
    r.undo_tip  = std::move(tip);
}

// Short reason for an API error token (the row's value); unknown tokens read as `fallback`.
static std::string mcp_reason(const std::string &e, const char *fallback = MCP_T("Rejected"))
{
    static const std::map<std::string, std::string> reasons{
        {"blocked_by_remote_api_policy", MCP_T("Not allowed")},
        {"unknown_object", MCP_T("Object not found")},
        {"no_instance", MCP_T("Object has no copy")},
        {"unknown_instance", MCP_T("No such copy")},
        {"file_not_found", MCP_T("File not found")},
        {"directory_not_found", MCP_T("Folder not found")},
        {"load_failed", MCP_T("Couldn't open it")},
        {"unsupported_format", MCP_T("Unsupported file")},
        {"slicing", MCP_T("Busy slicing")},
        {"busy", MCP_T("Busy")},
        {"job_running", MCP_T("Busy")},
        {"unsaved_changes", MCP_T("Unsaved changes")},
        {"no_project_path", MCP_T("Never saved")},
        {"exists", MCP_T("File exists")},
        {"same_path", MCP_T("Same path")},
        {"project_is_open", MCP_T("Project is open")},
        {"save_failed", MCP_T("Couldn't save")},
        {"move_failed", MCP_T("Couldn't move")},
        {"delete_failed", MCP_T("Couldn't delete")},
        {"cancelled", MCP_T("Cancelled")},
        {"unknown_recent", MCP_T("Not in the recent list")},
        {"unknown_plate", MCP_T("No such plate")},
        {"max_plates", MCP_T("No room for another plate")},
        {"last_plate", MCP_T("Last plate")},
        {"plate_locked", MCP_T("Plate locked")},
        {"select_failed", MCP_T("Couldn't select")},
        {"empty", MCP_T("Nothing on the plate")},
        {"unknown_preset", MCP_T("No such preset")},
        {"incompatible_preset", MCP_T("Not compatible")},
        {"slot_out_of_range", MCP_T("No such filament")},
        {"select_cancelled", MCP_T("Switch cancelled")},
        {"tab_unavailable", MCP_T("Not ready")},
        {"name_reserved", MCP_T("Built-in name")},
        {"builtin_preset", MCP_T("Built-in preset")},
        {"preset_selected", MCP_T("Preset in use")},
        {"has_children", MCP_T("Other presets inherit it")},
        {"layer_height_out_of_range", MCP_T("Layer height out of range")},
        {"overlaps_existing", MCP_T("Overlaps a range")},
        {"profile_failed", MCP_T("Couldn't compute")},
        {"unknown_key", MCP_T("Unknown setting")},
        {"not_editable_in_current_config", MCP_T("Not editable here")},
        {"redacted_placeholder", MCP_T("Hidden value")},
        {"key_dropped_by_legacy_handler", MCP_T("Not stored")},
        {"unknown_filament_index", MCP_T("No such filament")},
        {"unknown_tray", MCP_T("No such tray")},
        {"tray_not_ready", MCP_T("Tray not ready")},
        {"duplicate_tray", MCP_T("Tray used twice")},
        {"slots_unavailable", MCP_T("No CANVAS trays")},
        {"protected_path", MCP_T("Restore point folder")},
    };
    auto it = reasons.find(e);
    if (it != reasons.end()) return it->second;
    if (e.rfind("per_filament_length_mismatch", 0) == 0) return MCP_T("Wrong number of values");
    if (e.find("derived_from") == 0 || e.find("indexed_unchecked") == 0 || e.find("values_are_unchecked") == 0 ||
        e.find("tied_to") == 0 || e.find("live_printer_state") == 0 || e == "not_writable_project_key")
        return MCP_T("Read-only");
    return fallback;
}

// The MCP panel's restore points (<data_dir>/mcp_history) are out of the API's reach: a restore opens its file
// without the sensitive-key check, so the API must never put a file there, take one out, or open, save or load one.
// Also checked against the file's fingerprint at restore (McpActions); this keeps the points themselves intact.
// null when `path` (UTF-8) is outside, else the error reply.
static nlohmann::json mcp_history_refusal(const std::string &path)
{
    if (path.empty()) return nullptr;
    bool inside = true; // unknown counts as inside
    try {
        inside = mcp_path_within(McpRecorder::history_root_dir(), path);
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(warning) << "MCP panel: path check failed for " << path << ": " << e.what();
    }
    if (!inside) return nullptr;
    return {{"error", "protected_path"}, {"path", path},
            {"detail", "this is the MCP panel's restore point folder; the API does not open, save, move, delete or "
                       "load files there"}};
}

// A refused change. label = the setting or action; value = "Not allowed" for a policy block, else a short
// reason; where = what allows it, or the API's error and detail.
static void mcp_refused(const std::string &label, const std::string &value, const std::string &where,
                        const std::string &action)
{
    if (s_ui_trial) return;
    try {
        McpRecorder::get().record_refusal(label, where, value, {{"type", "refused"}, {"action", action}});
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "MCP panel: refusal not recorded: " << e.what();
    }
}

// The refused row for an error reply built in a UI lambda: {"error", "detail"?, "path"?}. A policy block
// names the Preferences box that allows it.
static void mcp_refused_reply(const std::string &label, const nlohmann::json &r, const std::string &action)
{
    const std::string e = r.contains("error") && r["error"].is_string() ? r["error"].get<std::string>() : "error";
    std::string       where;
    if (e == "blocked_by_remote_api_policy") {
        const std::string detail = r.contains("detail") && r["detail"].is_string() ? r["detail"].get<std::string>() : "";
        where = detail.find("deleting and replacing") != std::string::npos ? k_needs_file_changes : k_needs_sensitive;
    } else {
        where = e;
        if (r.contains("detail") && r["detail"].is_string())
            where += ": " + r["detail"].get<std::string>();
        else if (r.contains("path") && r["path"].is_string())
            where += ": " + r["path"].get<std::string>();
    }
    mcp_refused(label, mcp_reason(e), where, action);
}

static std::string mcp_file_name(const std::string &path_utf8)
{
    return into_u8(from_path(into_path(from_u8(path_utf8)).filename()));
}

static std::string mcp_size_text(const ModelObject &mo)
{
    const Vec3d sz = mo.bounding_box_exact().size();
    return mcp_fmt(MCP_T("{x} \xC3\x97 {y} \xC3\x97 {z} mm"), {{"x", McpRecorder::num_text(std::round(sz.x()))},
                                                              {"y", McpRecorder::num_text(std::round(sz.y()))},
                                                              {"z", McpRecorder::num_text(std::round(sz.z()))}});
}

// "Plate 2", or "beside the bed" for -1 (off every plate).
static std::string mcp_plate_text(int plate)
{
    return plate >= 0 ? mcp_fmt(MCP_T("Plate {n}"), {{"n", std::to_string(plate + 1)}}) :
                        std::string(MCP_T("beside the bed"));
}

static const char *mcp_preset_type_name(Preset::Type t)
{
    return t == Preset::TYPE_PRINT ? "print" : t == Preset::TYPE_FILAMENT ? "filament" : "printer";
}

// "<label> (filament 2)" / "<label> (filaments 1, 3)": the label with the slots using the preset the Filament tab
// edits, when more than one filament is loaded; else the label alone.
static std::string mcp_edited_filament_label(const std::string &label)
{
    auto *bundle = wxGetApp().preset_bundle;
    if (bundle->filament_presets.size() <= 1) return label;
    const std::string        sel = bundle->filaments.get_selected_preset_name();
    std::vector<std::string> slots;
    for (size_t i = 0; i < bundle->filament_presets.size(); ++i)
        if (bundle->filament_presets[i] == sel) slots.push_back(std::to_string(i + 1));
    if (slots.empty()) return label;
    if (slots.size() == 1) return mcp_fmt(MCP_T("{label} (filament {n})"), {{"label", label}, {"n", slots.front()}});
    std::string list;
    for (size_t i = 0; i < slots.size(); ++i) list += (i ? ", " : "") + slots[i];
    return mcp_fmt(MCP_T("{label} (filaments {list})"), {{"label", label}, {"list", list}});
}

// A setting row: value/before as config_value_text, undo "Revert" (raws in target).
// The panel's label for a setting (m1, polish P3): the text the settings sidebar draws for it (SettingsIndex
// display_label_en: "Top shell layers"), with its section only when that is needed to tell it apart ("Acceleration:
// Normal printing", "Other layers speed: Outer wall"); McpSettingLabel.hpp has the rule. preset_type: Preset::Type of
// the preset the key was written to, -1 = any (then a row text of another type counts as shared too). GUI thread.
static std::string mcp_setting_label(const std::string &key, int preset_type = -1)
{
    McpLabelParts p;
    p.full = McpRecorder::config_label(key);
    if (print_config_def.get(key) == nullptr) return p.full;
    try {
        if (wxGetApp().plater() != nullptr) {
            const std::vector<Search::Option> &opts = wxGetApp().sidebar().settings_index().all_options();
            auto base_of = [](const std::string &k) {
                const size_t h = k.find('#');
                return h == std::string::npos ? k : k.substr(0, h);
            };
            auto text_of = [](const Search::Option &o) {
                return o.display_label_en.empty() ? boost::nowide::narrow(o.label) : o.display_label_en;
            };
            const Search::Option *mine = nullptr;
            for (const Search::Option &o : opts) {
                if (preset_type >= 0 && int(o.type) != preset_type) continue;
                if (base_of(o.opt_key()) == key) { mine = &o; break; }
            }
            if (mine != nullptr) {
                p.row      = mine->display_label_en;
                p.group    = boost::nowide::narrow(mine->group);
                p.category = boost::nowide::narrow(mine->category);
                const std::string mine_text = mcp_label_lower(p.row.empty() ? text_of(*mine) : p.row);
                for (const Search::Option &o : opts) {
                    if (preset_type >= 0 && int(o.type) != preset_type) continue;
                    if (base_of(o.opt_key()) == key) continue;
                    if (mcp_label_lower(text_of(o)) == mine_text) { p.shared = true; break; }
                }
            }
        }
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(warning) << "MCP panel: no sidebar label for " << key << ": " << e.what();
    }
    return mcp_compose_setting_label(p);
}

static McpRow mcp_config_row(const std::string &key, const std::string &label, const std::string &before_raw,
                             const std::string &after_raw, nlohmann::json target, const std::string &where)
{
    McpRow r = mcp_row("setting", label, McpRecorder::config_value_text(key, after_raw),
                       McpRecorder::config_value_text(key, before_raw), where);
    // Multi-line text (a G-code template, the post-processing scripts): its first line and line count, or the first
    // line that differs when both sides start alike, and a few lines for the hover tag. Undo keeps the raws below.
    std::string after_text, before_text;
    const bool  has_after  = Controller::config_text(key, after_raw, after_text);
    const bool  has_before = Controller::config_text(key, before_raw, before_text);
    const std::string shown_after  = has_after ? mcp_multiline_display(after_text, has_before ? &before_text : nullptr) : std::string();
    const std::string shown_before = has_before ? mcp_multiline_display(before_text, has_after ? &after_text : nullptr) : std::string();
    if (!shown_after.empty()) {
        r.value     = shown_after;
        r.value_tip = mcp_multiline_tip(after_text, has_before ? &before_text : nullptr);
    }
    if (!shown_before.empty()) {
        r.before     = shown_before;
        r.before_tip = mcp_multiline_tip(before_text, has_after ? &after_text : nullptr);
    }
    // The live value of such a row is judged on the raws (McpActions live_of): its display form depends on both sides.
    if (!shown_after.empty() || !shown_before.empty()) target["multiline"] = true;
    target["before_raw"] = before_raw;
    target["after_raw"]  = after_raw;
    r.target             = std::move(target);
    mcp_undo(r, MCP_T("Revert"), mcp_fmt(MCP_T("Set {label} back to {before}"), {{"label", label}, {"before", r.before}}));
    return r;
}

// Every instance of the model: id, index, plate and matrix. For the arrange / orient rows.
struct McpInstanceState
{
    uint64_t       object_id   = 0;
    size_t         instance    = 0;
    uint64_t       instance_id = 0;
    int            plate       = -1;
    nlohmann::json matrix;
};

static std::vector<McpInstanceState> mcp_instance_states(Plater *plater)
{
    std::vector<McpInstanceState> out;
    const Model   &model  = plater->model();
    PartPlateList &plates = plater->get_partplate_list();
    for (size_t oi = 0; oi < model.objects.size(); ++oi) {
        const ModelObject *mo = model.objects[oi];
        for (size_t ii = 0; ii < mo->instances.size(); ++ii) {
            McpInstanceState st;
            st.object_id   = (uint64_t) mo->id().id;
            st.instance    = ii;
            st.instance_id = (uint64_t) mo->instances[ii]->id().id;
            st.plate       = plates.find_instance((int) oi, (int) ii);
            st.matrix      = McpRecorder::instance_matrix(*mo->instances[ii]);
            out.push_back(std::move(st));
        }
    }
    return out;
}

static bool mcp_same_matrix(const nlohmann::json &a, const nlohmann::json &b)
{
    if (!a.is_array() || !b.is_array() || a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const double x = a[i].get<double>(), y = b[i].get<double>();
        if (std::abs(x - y) > 1e-6 * std::max({std::abs(x), std::abs(y), 1.0})) return false;
    }
    return true;
}

// After an arrange / orient job: one row with every instance that moved (or changed plate). Runs on the GUI
// thread from the job queue, after the job's own finalize.
static void mcp_record_instances(const char *op, const std::vector<McpInstanceState> &before, int plates_before,
                                 bool trial = false)
{
    if (trial) {
        // A trial's job: no row, but the new places are still the API's.
        mcp_journal_api_objects_moved();
        return;
    }
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr) return;
    const std::vector<McpInstanceState> after = mcp_instance_states(plater);
    nlohmann::json        items = nlohmann::json::array();
    std::set<uint64_t>    moved_objects;
    for (const McpInstanceState &b : before) {
        auto a = std::find_if(after.begin(), after.end(), [&b](const McpInstanceState &x) {
            return x.object_id == b.object_id && x.instance_id == b.instance_id;
        });
        if (a == after.end()) continue;
        if (a->plate == b.plate && mcp_same_matrix(a->matrix, b.matrix)) continue;
        items.push_back({{"object_id", b.object_id}, {"instance", a->instance}, {"instance_id", b.instance_id},
                         {"plate_before", b.plate}, {"plate_after", a->plate}, {"before", b.matrix},
                         {"after", a->matrix}});
        moved_objects.insert(b.object_id);
    }
    const bool arrange = std::string(op) == "arrange";
    const int  n       = (int) moved_objects.size();
    McpRow     r       = mcp_row(arrange ? "plate" : "model", arrange ? MCP_T("Arranged") : MCP_T("Auto-oriented"),
                                 n == 0 ? std::string(MCP_T("Nothing moved"))
                                        : mcp_fmt(n == 1 ? MCP_T("{n} object") : MCP_T("{n} objects"),
                                                  {{"n", std::to_string(n)}}));
    if (n == 1) {
        const int oi = [&] {
            for (size_t i = 0; i < plater->model().objects.size(); ++i)
                if ((uint64_t) plater->model().objects[i]->id().id == *moved_objects.begin()) return (int) i;
            return -1;
        }();
        if (oi >= 0) r.where = plater->model().objects[oi]->name;
    }
    r.target = {{"type", "instances"}, {"op", op}, {"plates_before", plates_before},
                {"plates_after", plater->get_partplate_list().get_plate_count()}, {"items", items}};
    if (n > 0)
        mcp_undo(r, MCP_T("Put back"),
                 n == 1 && !r.where.empty() ? mcp_fmt(MCP_T("Move {name} back where it was"), {{"name", r.where}})
                                            : mcp_fmt(MCP_T("Put the {n} objects back where they were"),
                                                      {{"n", std::to_string(n)}}));
    mcp_record(std::move(r));
    // The job ran after the request's own baseline: these new places are the API's, not the user's.
    mcp_journal_api_objects_moved();
}

static void mcp_slice_finished(const nlohmann::json &stats, bool ok, const std::string &error)
{
    if (s_ui_trial) return; // a trial's slice that did not start records nothing either (the completion event checks m_api_slice)
    try {
        McpRecorder::get().slice_finished(stats, ok, error);
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "MCP panel: slice row not recorded: " << e.what();
    }
}

static int find_object_index(const Model &model, uint64_t id); // defined with the M4b object handlers below
static bool sensitive_writes_allowed(); // defined with the sensitive-key policy below
static bool project_file_changes_allowed(); // defined with the sensitive-key policy below
static bool project_has_unsaved_changes(Plater *plater); // defined with the project handlers below


void Controller::notify_config_changed(int preset_type)
{
    // The MCP panel's live values (also while the API is off: the pane stays).
    McpPanel::config_changed();
    if (g_controller) g_controller->on_config_changed(preset_type);
}

void Controller::notify_project_opened()
{
    // MCP panel: a restore opens a checkpoint file and then puts the project's own name and path back; that is
    // not a project being opened (no event, and the recorder keeps the project's timeline).
    if (McpActions::restoring()) return;
    // MCP panel: the next history sync takes a path change for an open, not a save. Also while the API
    // is off, so a project opened then has its timeline back once the API is turned on.
    McpRecorder::note_project_opened();
    McpPanel::project_changed();
    // The user-change journal: one "Opened project" entry replaces everything before it (an API open is
    // absorbed by the baseline at the end of its request).
    mcp_journal_project_opened();
    if (g_controller == nullptr) return;
    // On the GUI thread at the end of Plater::load_project.
    std::string name = wxGetApp().plater()->get_project_filename().ToUTF8().data();
    wxGetApp().remote_api_server().broadcast({{"event", "project.opened"}, {"project", name}});
}

void Controller::notify_project_new()
{
    // A restore may start a new project on its way to opening the checkpoint: not the user's new project.
    if (McpActions::restoring()) return;
    mcp_journal_project_new();
    McpPanel::project_changed();
}

void Controller::on_config_changed(int preset_type)
{
    // Called on the GUI thread from Tab::update_dirty. Debounce one event-loop
    // turn: GUI edits and load_config fire this repeatedly.
    {
        std::lock_guard<std::mutex> lk(m_cc_mutex);
        m_cc_pending.insert(preset_type);
        if (m_cc_timer_armed) return;
        m_cc_timer_armed = true;
    }
    wxGetApp().CallAfter([this] {
        std::set<int> pending;
        {
            std::lock_guard<std::mutex> lk(m_cc_mutex);
            pending.swap(m_cc_pending);
            m_cc_timer_armed = false;
        }
        nlohmann::json tabs = nlohmann::json::array();
        for (int t : pending)
            tabs.push_back(t == Preset::TYPE_PRINT        ? "print" :
                           t == Preset::TYPE_FILAMENT     ? "filament" :
                           t == Preset::TYPE_PRINTER      ? "printer" :
                           t == Preset::TYPE_SLA_PRINT    ? "sla_print" :
                           t == Preset::TYPE_SLA_MATERIAL ? "sla_material" : "other");
        wxGetApp().remote_api_server().broadcast({{"event", "config.changed"}, {"tabs", tabs}});
    });
}

// --- F8 gates (GUI thread only) ----------------------------------------------
// The periodic auto-backup exporter (MainFrame, every 10s) pumps the event
// queue while it serializes the project, so a queued CallAfter API mutation
// could execute INSIDE export_3mf and mutate the model/presets being written -
// observed as heap-corruption crashes under config-write and preset-save
// bursts. Two-way exclusion: API tasks arriving during an export are parked
// and run when the export ends; the exporter skips a cycle (re-arming itself)
// if an API task is on the stack (nested-pump case). The gate (UiTaskGate.hpp)
// keeps the tasks strictly in arrival order, parked or not: the io thread does
// not wait for the journal's tasks, so a parked task must never be overtaken.
namespace {

UiTaskGate &ui_gate()
{
    static UiTaskGate gate([](UiTaskGate::Task t) { wxGetApp().CallAfter(std::move(t)); });
    return gate;
}

struct UiTask : std::enable_shared_from_this<UiTask>
{
    bool                                          trial { false }; // from a X-Orca-Mcp-Trial request (M4)
    std::shared_ptr<std::promise<nlohmann::json>> promise;
    std::shared_ptr<std::atomic<bool>>            cancelled;
    std::function<nlohmann::json()>               fn;

    void schedule()
    {
        auto self = shared_from_this();
        wxGetApp().CallAfter([self] {
            if (self->cancelled->load()) return; // caller already returned 504; do nothing
            ui_gate().arrive([self] { self->run(); });
        });
    }
    void run();
};

// Runs through the gate: no export bracketed, no other API task on the stack, every earlier task done.
void UiTask::run()
{
    if (cancelled->load()) return; // timed out while parked
    // Push the next periodic backup out of the mutation window so it doesn't
    // fire between the calls of an API burst.
    Slic3r::backup_defer(3);
    const bool trial_before = s_ui_trial;
    s_ui_trial = trial;
    try {
        promise->set_value(fn());
    } catch (...) {
        promise->set_exception(std::current_exception());
    }
    s_ui_trial = trial_before;
}

} // namespace

bool Controller::api_ui_task_active() { return ui_gate().task_active(); }

void Controller::set_backup_in_progress(bool v) { ui_gate().backup(v); }

bool Controller::backup_in_progress() { return ui_gate().backup_active(); }

nlohmann::json Controller::run_on_ui(std::function<nlohmann::json()> fn, int timeout_s)
{
    auto task       = std::make_shared<UiTask>();
    task->promise   = std::make_shared<std::promise<nlohmann::json>>();
    // Set if the io side gives up (10s timeout) before the GUI lambda runs. The
    // lambda checks it at the top, so a timed-out request does NOT still apply
    // its side effects (PUT config / reslice) on the GUI thread afterwards.
    task->cancelled = std::make_shared<std::atomic<bool>>(false);
    task->fn        = std::move(fn);
    task->trial     = t_io_trial;
    auto future     = task->promise->get_future();
    task->schedule();
    if (future.wait_for(std::chrono::seconds(timeout_s)) != std::future_status::ready) {
        task->cancelled->store(true);
        throw std::runtime_error("ui_timeout");
    }
    return future.get(); // rethrows GUI-side exceptions
}

static nlohmann::json config_to_json(const DynamicPrintConfig &cfg,
                                     const std::vector<std::string> *only_keys)
{
    nlohmann::json out = nlohmann::json::object();
    auto emit = [&](const std::string &key) {
        const ConfigOption *opt = cfg.option(key);
        if (opt != nullptr)
            out[key] = opt->serialize(); // canonical string form, same as .ini/.3mf
    };
    if (only_keys) {
        for (const auto &k : *only_keys) emit(k);
    } else {
        for (const auto &k : cfg.keys()) emit(k);
    }
    return out;
}

Response Controller::handle_status()
{
    nlohmann::json j = run_on_ui([]() -> nlohmann::json {
        auto *bundle = wxGetApp().preset_bundle;
        auto *plater = wxGetApp().plater();

        nlohmann::json objects = nlohmann::json::array();
        for (const ModelObject *mo : plater->model().objects) {
            auto sz = mo->bounding_box_exact().size();
            objects.push_back({{"name", mo->name},
                               {"size_mm", {sz.x(), sz.y(), sz.z()}}});
        }
        PartPlateList &plates = plater->get_partplate_list();
        auto plate_valid = plates.get_curr_plate()->is_slice_result_valid();
        return {
            {"app", SLIC3R_APP_NAME},
            // plates: how many there are and which one slice/gcode/render act on.
            {"plates", {{"count", plates.get_plate_count()}, {"current", plates.get_curr_plate_index()}}},
            {"app_version", SoftFever_VERSION},
            {"api_version", "1.0"},
            {"capabilities", {"status", "config", "slice", "events", "model", "preset", "gcode", "objects", "arrange", "orient", "object_config", "slice_breakdown", "plate_render", "sensitive_key_policy", "plates", "printer_slots", "project", "preset_slot", "activity"}},
            {"sensitive_keys_allowed", sensitive_writes_allowed()},
            {"project_file_changes_allowed", project_file_changes_allowed()},
            {"project", plater->get_project_filename().ToUTF8().data()},
            // project: the window title's name, and whether File > Save has something to
            // write (plater edits, modified presets, or undo-stack changes since the save).
            {"project_name", std::string(plater->get_project_name().ToUTF8().data())},
            {"project_dirty", project_has_unsaved_changes(plater)},
            {"objects", objects},
            {"presets", {
                {"printer",  bundle->printers.get_selected_preset_name()},
                {"print",    bundle->prints.get_selected_preset_name()},
                {"filaments", bundle->filament_presets}
            }},
            {"modified", {
                {"print",    bundle->prints.current_dirty_options(true)},
                {"filament", bundle->filaments.current_dirty_options(true)},
                {"printer",  bundle->printers.current_dirty_options(true)}
            }},
            {"slice_result_valid", plate_valid}
        };
    });
    j["slicing"] = slice_state().state == "slicing";
    return { 200, j };
}

// Percent-decode a query value. Clients (e.g. httpx) percent-encode ',' as %2C, so the
// keys list must be decoded BEFORE splitting on ',' - otherwise the whole list is one
// unmatched key and the filter returns nothing.
static std::string url_decode(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    auto hexval = [](char c) { return c <= '9' ? c - '0' : (std::tolower((unsigned char)c) - 'a' + 10); };
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() &&
            std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2])) {
            out += char(hexval(s[i + 1]) * 16 + hexval(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

Response Controller::handle_get_config(const std::string &target)
{
    // Optional filter: /api/v1/config?keys=layer_height,wall_loops
    std::vector<std::string> keys;
    auto qpos = target.find("?keys=");
    if (qpos != std::string::npos) {
        std::string list = url_decode(target.substr(qpos + 6));
        size_t start = 0;
        while (start <= list.size()) {
            auto comma = list.find(',', start);
            auto item  = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (!item.empty()) keys.push_back(item);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    nlohmann::json j = run_on_ui([keys = std::move(keys)]() -> nlohmann::json {
        DynamicPrintConfig cfg = wxGetApp().preset_bundle->full_config_secure();
        return config_to_json(cfg, keys.empty() ? nullptr : &keys);
    });
    return { 200, {{"config", j}} };
}

static std::string json_value_to_config_string(const nlohmann::json &v)
{
    if (v.is_string())  return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "1" : "0";
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number_float()) {
        std::ostringstream ss;
        ss << v.get<double>();
        return ss.str();
    }
    throw std::runtime_error("unsupported value type");
}

// ---------------------------------------------------------------------------
// Value ranges (B1)
//
// The sidebar never lets a value outside its option's range through (Field::get_value_by_opt_type clamps it), and
// PrintConfig's validate() refuses to slice one; the API applied them as given, and a layer height of -1 crashed
// OrcaSlicer's reslice (out of memory) before validate() could say no. Every write path (PUT /config, object
// settings, the panel's undo and restore edits) now checks each changed element against the option's
// ConfigOptionDef min / max (and validate()'s stricter minimums: a layer height above 0) with ConfigRange.hpp and
// refuses the whole
// request with out_of_range, applying nothing.
// The lowest layer height the printer allows: Printer Settings > Layer height limits > Min, the lowest over its
// extruders (as ConfigManipulation::layer_height_limits takes it), never below kMinLayerHeight (also when the profile
// leaves it at 0). printer: the printer config to read (a PUT /config batch's staged copy), else the edited preset.
// GUI thread.
static constexpr double kMinLayerHeight = 0.01;
static double printer_min_layer_height(const DynamicPrintConfig *printer = nullptr)
{
    double m = kMinLayerHeight;
    try {
        const DynamicPrintConfig *pc = printer;
        if (pc == nullptr)
            if (auto *b = wxGetApp().preset_bundle) pc = &b->printers.get_edited_preset().config;
        const ConfigOption *o = pc != nullptr ? pc->option("min_layer_height") : nullptr;
        if (o != nullptr && (o->type() == coFloats || o->type() == coFloat)) {
            double lowest = std::numeric_limits<double>::infinity();
            if (o->type() == coFloat) lowest = static_cast<const ConfigOptionFloat *>(o)->value;
            else
                for (const double v : static_cast<const ConfigOptionVector<double> *>(o)->values)
                    if (std::isfinite(v)) lowest = std::min(lowest, v);
            if (std::isfinite(lowest)) m = std::max(lowest, kMinLayerHeight);
        }
    } catch (const std::exception &) {}
    return m;
}

static std::string range_unit(const ConfigOptionDef &def, bool percent)
{
    if (percent) return "%";
    const std::string &s = def.sidetext;
    if (s.empty() || s.find('%') == 0) return {};
    const size_t sp = s.find(' ');
    return " " + (sp == std::string::npos ? s : s.substr(0, sp));
}

static void range_elems(const ConfigOption &opt, const ConfigOption *before, std::vector<McpRangeElem> &out, bool &ok)
{
    ok = true;
    switch (opt.type()) {
    case coFloat:
    case coPercent: {
        McpRangeElem e;
        e.value   = static_cast<const ConfigOptionFloat &>(opt).value;
        e.percent = opt.type() == coPercent;
        e.changed = before == nullptr || before->type() != opt.type() ||
                    !(static_cast<const ConfigOptionFloat *>(before)->value == e.value);
        out.push_back(e);
        break;
    }
    case coFloatOrPercent: {
        const auto  &o = static_cast<const ConfigOptionFloatOrPercent &>(opt);
        McpRangeElem e;
        e.value   = o.value;
        e.percent = o.percent;
        const auto *b = before != nullptr && before->type() == coFloatOrPercent ? static_cast<const ConfigOptionFloatOrPercent *>(before) : nullptr;
        e.changed = b == nullptr || !(b->value == o.value && b->percent == o.percent);
        out.push_back(e);
        break;
    }
    case coFloats:
    case coPercents: {
        const auto &v  = static_cast<const ConfigOptionVector<double> &>(opt).values;
        const auto *bv = before != nullptr && before->type() == opt.type() ? &static_cast<const ConfigOptionVector<double> *>(before)->values : nullptr;
        for (size_t i = 0; i < v.size(); ++i) {
            McpRangeElem e;
            e.value   = v[i];
            e.nil     = std::isnan(v[i]);
            e.percent = opt.type() == coPercents;
            const bool same = bv != nullptr && i < bv->size() &&
                              ((*bv)[i] == v[i] || (std::isnan((*bv)[i]) && std::isnan(v[i])));
            e.changed = !same;
            out.push_back(e);
        }
        break;
    }
    case coFloatsOrPercents: {
        const auto &v  = static_cast<const ConfigOptionVector<FloatOrPercent> &>(opt).values;
        const auto *bv = before != nullptr && before->type() == coFloatsOrPercents ? &static_cast<const ConfigOptionVector<FloatOrPercent> *>(before)->values : nullptr;
        for (size_t i = 0; i < v.size(); ++i) {
            McpRangeElem e;
            e.value   = v[i].value;
            e.percent = v[i].percent;
            e.nil     = std::isnan(v[i].value);
            const bool same = bv != nullptr && i < bv->size() && (*bv)[i].percent == v[i].percent &&
                              ((*bv)[i].value == v[i].value || (std::isnan((*bv)[i].value) && std::isnan(v[i].value)));
            e.changed = !same;
            out.push_back(e);
        }
        break;
    }
    case coInt: {
        McpRangeElem e;
        e.value   = static_cast<const ConfigOptionInt &>(opt).value;
        e.changed = before == nullptr || before->type() != coInt || static_cast<const ConfigOptionInt *>(before)->value != static_cast<const ConfigOptionInt &>(opt).value;
        out.push_back(e);
        break;
    }
    case coInts: {
        const auto &v  = static_cast<const ConfigOptionVector<int> &>(opt).values;
        const auto *bv = before != nullptr && before->type() == coInts ? &static_cast<const ConfigOptionVector<int> *>(before)->values : nullptr;
        for (size_t i = 0; i < v.size(); ++i) {
            McpRangeElem e;
            e.value   = v[i];
            e.nil     = opt.nullable() && v[i] == ConfigOptionIntsNullable::nil_value();
            e.changed = !(bv != nullptr && i < bv->size() && (*bv)[i] == v[i]);
            out.push_back(e);
        }
        break;
    }
    default: ok = false; // not a number: no range
    }
}

bool Controller::config_value_in_range(const std::string &key, const ConfigOption *before, const ConfigOption &after,
                                       nlohmann::json &detail, std::string &text, const DynamicPrintConfig *printer)
{
    const ConfigOptionDef *def = print_config_def.get(key);
    if (def == nullptr) return true;
    std::vector<McpRangeElem> elems;
    bool                      numeric = false;
    range_elems(after, before, elems, numeric);
    if (!numeric) return true;
    McpRangeLimits l;
    l.min           = def->min;
    l.max           = def->max;
    l.nullable      = def->nullable;
    // A layer height is held to the printer's lowest (Printer Settings > Layer height limits), never below
    // kMinLayerHeight: validate() only refuses <= 0, and a layer height of 0.0001 mm on a 50 mm part is 500 000 layers,
    // the out-of-memory reslice a layer height of -1 caused (the GUI asks in a dialog; the API cannot).
    bool layer_limit = false;
    if (key == "layer_height" || key == "initial_layer_print_height") {
        l.min       = std::max(l.min, printer_min_layer_height(printer));
        layer_limit = true;
    }
    // validate()'s other rules (PrintConfig.cpp validate), stricter than these options' min.
    else if (key == "bridge_flow" || key == "internal_bridge_flow" || key == "filament_flow_ratio" ||
             key == "extruder_clearance_radius" || key == "extruder_clearance_height_to_rod" ||
             key == "extruder_clearance_height_to_lid" || key == "nozzle_height") {
        l.min = std::max(l.min, 0.); l.min_exclusive = true;
    }
    else if (key == "filament_diameter") l.min = std::max(l.min, 1.);
    else if (key == "nozzle_diameter")   l.min = std::max(l.min, 0.005);
    const McpRangeHit hit = mcp_first_out_of_range(l, elems);
    if (!hit.found) return true;
    const bool        pct_bounds = after.type() == coPercent || after.type() == coPercents || hit.percent;
    const std::string unit       = range_unit(*def, pct_bounds);
    const std::string got        = std::isfinite(hit.value) ? McpRecorder::num_text(hit.value) + (hit.percent ? std::string("%") : unit)
                                                            : std::string(MCP_T("not a number"));
    const bool lo = mcp_range_bounded(hit.min), hi = mcp_range_bounded(hit.max);
    detail = {{"error", "out_of_range"}, {"key", key},
              {"value", std::isfinite(hit.value) ? McpRecorder::num_text(hit.value) + (hit.percent ? "%" : "") : std::string("nan")},
              {"min", lo ? nlohmann::json(double(hit.min)) : nlohmann::json(nullptr)},
              {"max", hi ? nlohmann::json(double(hit.max)) : nlohmann::json(nullptr)}};
    if (hit.min_exclusive) detail["min_exclusive"] = true;
    if (elems.size() > 1) detail["index"] = hit.index;
    const std::string label = McpRecorder::config_label(key);
    const std::string mn = McpRecorder::num_text(hit.min) + unit, mx = McpRecorder::num_text(hit.max) + unit;
    // One whole-sentence template per case: the MCP panel matches the full text.
    const char *tmpl;
    if (lo && hit.min_exclusive && hi)
        tmpl = layer_limit ? MCP_T("{label} must be above {min} and at most {max} (got {got}); the lowest the printer allows is set in Printer Settings > Layer height limits")
                           : MCP_T("{label} must be above {min} and at most {max} (got {got})");
    else if (lo && hit.min_exclusive)
        tmpl = layer_limit ? MCP_T("{label} must be above {min} (got {got}); the lowest the printer allows is set in Printer Settings > Layer height limits")
                           : MCP_T("{label} must be above {min} (got {got})");
    else if (lo && hi)
        tmpl = layer_limit ? MCP_T("{label} must be between {min} and {max} (got {got}); the lowest the printer allows is set in Printer Settings > Layer height limits")
                           : MCP_T("{label} must be between {min} and {max} (got {got})");
    else if (lo)
        tmpl = layer_limit ? MCP_T("{label} must be at least {min} (got {got}); the lowest the printer allows is set in Printer Settings > Layer height limits")
                           : MCP_T("{label} must be at least {min} (got {got})");
    else
        tmpl = layer_limit ? MCP_T("{label} must be at most {max} (got {got}); the lowest the printer allows is set in Printer Settings > Layer height limits")
                           : MCP_T("{label} must be at most {max} (got {got})");
    text = mcp_fmt(tmpl, {{"label", label}, {"min", mn}, {"max", mx}, {"got", got}});
    detail["detail"] = text;
    return false;
}

// ---------------------------------------------------------------------------
// Sensitive keys
//
// PUT /config reaches every preset key, and some of them are not slicing settings:
// post_process runs shell commands after export, the *_gcode templates go straight to
// the printer, and the physical-printer options (print_host, printhost_*, host_type,
// printer_agent, ...) decide where and how a print is sent and with which credentials.
// printer_model / printer_technology move OrcaSlicer onto other code paths and
// filename_format names the exported file. An AI agent holding the token can be steered
// by text it reads, so changing any of these is refused unless the user ticks
// Preferences > Remote API > "Allow script, G-code and connection edits". Writing a key
// back to the value it already has is not a change and passes. The MCP server keeps the
// same rule in its guard.py as a second check; keep the two in step.
static bool is_sensitive_config_key(const std::string &key)
{
    auto ends_with = [&key](const std::string &suffix) {
        return key.size() > suffix.size() &&
               key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    // slicing_pipeline_plugin (2.5): Python plugins run at each slicing step.
    // plugins (2.5): the plugin capabilities a preset brings with it.
    if (key == "post_process" || key == "printer_model" || key == "filename_format" ||
        key == "slicing_pipeline_plugin" || key == "plugins" ||
        key.rfind("printhost_", 0) == 0)
        return true;
    // *_gcode templates, and *_gcodes lists of them (imex_mode_gcodes, 2.5).
    if (ends_with("_gcode") || ends_with("_gcodes"))
        return key != "emit_machine_limits_to_gcode" && key != "high_precision_gcode" &&
               key != "skip_modified_gcodes"; // toggles, not templates
    const std::vector<std::string> &pp = PhysicalPrinter::printer_options();
    return std::find(pp.begin(), pp.end(), key) != pp.end();
}

bool Controller::sensitive_config_key(const std::string &key) { return is_sensitive_config_key(key); }

bool Controller::config_text(const std::string &key, const std::string &raw, std::string &text)
{
    const ConfigOptionDef *d = print_config_def.get(key);
    if (d == nullptr) return false;
    if (d->type == coString) return unescape_string_cstyle(raw, text);
    if (d->type == coStrings && d->multiline) {
        std::vector<std::string> entries;
        if (!unescape_strings_cstyle(raw, entries)) return false;
        text.clear();
        for (size_t i = 0; i < entries.size(); ++i) text += (i > 0 ? "\n" : "") + entries[i];
        return true;
    }
    return false;
}

// GUI thread only (AppConfig).
static bool sensitive_writes_allowed()
{
    return wxGetApp().app_config->get_bool("remote_api_allow_sensitive");
}

// The project file routes reach any .3mf on this computer, and a delete is
// permanent (no trash). Deleting a file, or replacing one with a save or a move,
// is refused unless the user ticks Preferences > Remote API > "Allow deleting and
// replacing project files". Saving, opening and moving onto a free name never
// lose data and stay open. GUI thread only (AppConfig). Absent = off.
static bool project_file_changes_allowed()
{
    return wxGetApp().app_config->get_bool("remote_api_allow_file_changes");
}

// What POST /preset/config hides. GET /config already drops the whole host block
// (full_config_secure), but a named preset's config is returned as stored, so the
// credentials are replaced and any user:password inside a host URL (the print_host
// tooltip documents https://user:password@host/) is cut out. The host, CA file path and
// port stay visible so upload problems can still be diagnosed.
static const char *const k_redacted = "<redacted>";

static bool is_credential_key(const std::string &key)
{
    return key == "printhost_apikey" || key == "printhost_user" || key == "printhost_password";
}

static bool is_host_url_key(const std::string &key)
{
    return key == "print_host" || key == "print_host_webui";
}

// Length of a leading "scheme://", or 0. The scheme has to start the value and be
// RFC 3986 letters (ASCII, whatever the locale), so in "user:pa://ss@host" the "user:pa"
// is not mistaken for one.
static size_t url_scheme_length(const std::string &url)
{
    const auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    if (url.empty() || !alpha(url[0]))
        return 0;
    size_t i = 1;
    while (i < url.size() && (alpha(url[i]) || digit(url[i]) || url[i] == '+' || url[i] == '-' || url[i] == '.'))
        ++i;
    return url.compare(i, 3, "://") == 0 ? i + 3 : 0;
}

// Everything between the scheme and the LAST '@' is cut. A host never contains '@', while a
// password may contain '@', '/', '?', '#' or spaces, so this hides all of them. A URL with an
// '@' in its path loses the part before it too: it cannot be told apart from a password
// containing '/', and over-redacting a rare URL is the safe way to be wrong.
// Keep in step with _strip_userinfo in orcaslicer-mcp's guard.py.
static std::string strip_url_userinfo(const std::string &url)
{
    const size_t at = url.rfind('@');
    if (at == std::string::npos)
        return url;
    return url.substr(0, url_scheme_length(url)) + k_redacted + url.substr(at);
}

// ---------------------------------------------------------------------------
// Project-scope writes (CFS / multi-material)
//
// PresetBundle::project_config holds the ~19 keys of s_project_options, but
// opening all of them would be unsafe. Their consumers index them with unchecked
// arithmetic whose bound comes from a DIFFERENT vector, so a wrong-length write
// is an out-of-bounds heap access rather than a validation failure - e.g.
// Sidebar::auto_calc_flushing_volumes sizes its loop from filament_colour and
// writes flush_volumes_matrix at [n*from + to] (Plater.cpp), and
// Print::_make_wipe_tower uses filament_map values as direct extruder indices.
//
// The usual guard does not help: set_deserialize_strict rejects a malformed
// scalar or enum, but ConfigOptionFloatsTempl::deserialize (Config.hpp) accepts
// any input and unconditionally returns true, so a short or non-numeric matrix
// sails through as "applied". Hence an explicit allow-list, each entry carrying
// the invariant its consumers assume, checked after the whole batch is staged.
static const std::set<std::string> &project_writable_set()
{
    static const std::set<std::string> s_writable {
        "flush_volumes_matrix",  // filaments^2 * nozzles
        "flush_multiplier",      // one per nozzle
        "flush_multiplier_fast", // one per nozzle
        "filament_colour",       // its length IS the filament count - kept fixed
        "curr_bed_type",         // enum; btDefault rejected
        "prime_volume_mode",     // enum, self-validating
        "wipe_tower_x",          // per-plate vector, written via set_at
        "wipe_tower_y",
    };
    return s_writable;
}

static bool project_key_writable(const std::string &key) { return project_writable_set().count(key) > 0; }

std::vector<std::string> Controller::writable_project_keys()
{
    return std::vector<std::string>(project_writable_set().begin(), project_writable_set().end());
}

// Why a recognized project key stays read-only. Keeps the 422 actionable rather
// than a bare "not_editable_in_current_config".
static const char *project_key_block_reason(const std::string &key)
{
    if (key == "flush_volumes_vector")
        return "derived_from_flush_volumes_matrix";
    if (key == "filament_colour_type" || key == "filament_multi_colour")
        return "indexed_unchecked_by_the_colour_picker";
    if (key == "filament_map" || key == "filament_volume_map" || key == "filament_nozzle_map")
        return "values_are_unchecked_extruder_indices";
    if (key == "filament_map_mode" || key == "nozzle_volume_type")
        return "tied_to_filament_map";
    if (key == "has_filament_switcher" || key == "enable_filament_dynamic_map")
        return "live_printer_state_not_a_setting";
    return "not_writable_project_key";
}

// ConfigOptionFloatsTempl::deserialize pushes a zero-initialized value for any
// token it cannot parse and still returns true, so "a,b,c" would be stored as
// {0,0,0} and reported as applied - a silent wrong answer that no range check can
// catch, because 0 is a perfectly valid purge volume. Vet the raw string first.
static std::string numeric_list_error(const std::string &s)
{
    if (s.empty())
        return "expected a comma-separated list of numbers, got an empty string";
    size_t start = 0;
    while (true) {
        const size_t comma = s.find(',', start);
        std::string  tok   = s.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        const size_t b     = tok.find_first_not_of(" \t");
        const size_t e     = tok.find_last_not_of(" \t");
        tok                = (b == std::string::npos) ? std::string() : tok.substr(b, e - b + 1);
        try {
            size_t pos = 0;
            (void) std::stod(tok, &pos); // only the parse matters here
            if (pos != tok.size())
                throw std::invalid_argument("trailing characters");
        } catch (const std::exception &) {
            return "\"" + tok + "\" is not a number";
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return {};
}

// Same class of silent failure for colours: ConfigOptionStrings::deserialize
// accepts anything, and a colour the parser cannot read becomes black in the
// flush estimator rather than an error.
static std::string colour_list_error(const std::string &s)
{
    size_t start = 0;
    while (true) {
        const size_t semi = s.find(';', start);
        std::string  tok  = s.substr(start, semi == std::string::npos ? std::string::npos : semi - start);
        const size_t b    = tok.find_first_not_of(" \t");
        const size_t e    = tok.find_last_not_of(" \t");
        tok               = (b == std::string::npos) ? std::string() : tok.substr(b, e - b + 1);
        const bool ok = (tok.size() == 7 || tok.size() == 9) && tok[0] == '#' &&
                        std::all_of(tok.begin() + 1, tok.end(),
                                    [](char c) { return std::isxdigit((unsigned char) c) != 0; });
        if (!ok)
            return "\"" + tok + "\" is not a #RRGGBB or #RRGGBBAA colour";
        if (semi == std::string::npos) break;
        start = semi + 1;
    }
    return {};
}

// Checks the staged value against the invariant its consumers assume. Runs over
// the FINAL staged config, after every key in the batch is written, so a request
// that changes filament_colour and flush_volumes_matrix together is judged
// against the new filament count regardless of key order. Returns "" when valid.
static std::string project_value_error(const std::string        &key,
                                       const DynamicPrintConfig &staged,
                                       int                       nozzles,
                                       size_t                    filaments_before)
{
    auto floats_of = [&staged](const std::string &k) -> std::vector<double> {
        const auto *o = staged.option<ConfigOptionFloats>(k);
        return o == nullptr ? std::vector<double>{} : o->values;
    };

    if (key == "flush_volumes_matrix") {
        const std::vector<double> v = floats_of(key);
        const auto *colours = staged.option<ConfigOptionStrings>("filament_colour");
        const size_t filaments = colours == nullptr ? filaments_before : colours->values.size();
        const size_t want = filaments * filaments * size_t(nozzles > 0 ? nozzles : 1);
        if (v.size() != want)
            return "expected " + std::to_string(want) + " values (filaments^2 x nozzles = " +
                   std::to_string(filaments) + "^2 x " + std::to_string(nozzles) + "), got " +
                   std::to_string(v.size());
        for (double d : v)
            if (!std::isfinite(d) || d < 0. || d > double(g_max_flush_volume))
                return "each value must be a finite number within [0, " + std::to_string(g_max_flush_volume) + "]";
        return {};
    }
    if (key == "flush_multiplier" || key == "flush_multiplier_fast") {
        const std::vector<double> v = floats_of(key);
        if (v.size() != size_t(nozzles > 0 ? nozzles : 1))
            return "expected " + std::to_string(nozzles) + " value(s), one per nozzle, got " +
                   std::to_string(v.size());
        // Same range the wipe-tower dialog's spin control enforces.
        for (double d : v)
            if (!std::isfinite(d) || d < 0. || d > 3.)
                return "each value must be a finite number within [0, 3]";
        return {};
    }
    if (key == "filament_colour") {
        const auto *o = staged.option<ConfigOptionStrings>(key);
        const size_t n = o == nullptr ? 0 : o->values.size();
        // The length of filament_colour is what defines the filament count for
        // the wipe tower and the flush matrix. Changing it here would desync
        // every vector sized from it, so colours may be recoloured but not
        // added or removed - that is the filament-preset selection's job.
        if (n != filaments_before)
            return "filament count is fixed here: expected " + std::to_string(filaments_before) +
                   " colours (';'-separated), got " + std::to_string(n);
        return {};
    }
    if (key == "curr_bed_type") {
        // btDefault is reachable only through a raw write (the GUI never offers
        // it) and is a null dereference on the slicing path: its bed-temperature
        // key is "", so the lookup returns nullptr and GCode.cpp dereferences it.
        if (staged.opt_enum<BedType>(key) == btDefault)
            return "\"Default Plate\" is not selectable; pick a concrete plate type";
        return {};
    }
    if (key == "wipe_tower_x" || key == "wipe_tower_y") {
        const std::vector<double> v = floats_of(key);
        for (double d : v)
            if (!std::isfinite(d))
                return "must be a finite number";
        return {};
    }
    return {};
}

// GET /config reports the MERGED config, where every vector-valued filament key holds
// one run of values per loaded filament slot: PresetBundle::full_fff_config walks the
// filament keys and does set() for the first slot then append() for the rest
// (src/libslic3r/PresetBundle.cpp), skipping only the two compatibility lists. A write,
// by contrast, lands in the single preset open in the Filament tab. So echoing a merged
// value back ("70,100,70,70,35") would store the whole list in that one preset, and the
// next merge would splice all of it in at every slot using that preset - the list grows
// on each write and the per-slot values are lost.
//
// A preset's own vector length is the authoritative one (normally 1, but more when the
// printer has extruder variants - full_fff_config reads that length back as
// filament_variant_count), so rather than hard-coding "exactly one value" we refuse any
// write that would CHANGE the length. That keeps single-filament write-back working and
// rejects the merged shape.
static bool is_merged_per_filament_key(Preset::Type type, const std::string &key, const ConfigOption *opt)
{
    return type == Preset::TYPE_FILAMENT && opt != nullptr && opt->is_vector() &&
           key != "compatible_prints" && key != "compatible_printers";
}

static size_t config_vector_size(const DynamicPrintConfig &cfg, const std::string &key)
{
    const ConfigOption *opt = cfg.option(key);
    return (opt == nullptr || ! opt->is_vector())
               ? 0
               : static_cast<const ConfigOptionVectorBase *>(opt)->size();
}

// The project-scope epilogue of PUT /config: applies a staged copy of project_config and runs the follow-ups its
// keys need. Also the MCP panel's Revert of a project key. GUI thread only.
void Controller::apply_project_config(const DynamicPrintConfig &proj_new, const std::set<std::string> &proj_keys,
                                      const std::vector<std::string> &colours_before)
{
    // Project scope has no Tab, so the preset path of PUT /config does not apply. There
    // is no single "apply a project config change" helper in-tree either - every
    // GUI writer hand-composes this epilogue (WipeTowerDialog::open_flushing_dialog,
    // Sidebar::auto_calc_flushing_volumes, Plater::priv::on_select_bed_type,
    // PlaterPresetComboBox::sync_colour_config). This mirrors them, once per
    // request rather than once per key.
    if (proj_keys.empty()) return;
    auto   *bundle = wxGetApp().preset_bundle;
    Plater *plater = wxGetApp().plater();
    bundle->project_config.apply(proj_new);

    const bool touched_flush = proj_keys.count("flush_volumes_matrix") ||
                               proj_keys.count("flush_multiplier") ||
                               proj_keys.count("flush_multiplier_fast");
    const bool touched_tower = proj_keys.count("wipe_tower_x") || proj_keys.count("wipe_tower_y");

    if (proj_keys.count("curr_bed_type")) {
        // The bed type is cached in AppConfig as well, globally and per
        // printer, and export_selections below reads the global copy back -
        // so these have to run first. on_bed_type_change (not
        // Sidebar::set_bed_type_accord_combox) because the latter fires
        // wxEVT_COMBOBOX and would re-enter this same write path.
        BedType    bt = bundle->project_config.opt_enum<BedType>("curr_bed_type");
        AppConfig *ac = wxGetApp().app_config;
        ac->set("curr_bed_type", std::to_string(int(bt)));
        ac->set_printer_setting(bundle->printers.get_selected_preset_name(),
                                "curr_bed_type", std::to_string(int(bt)));
        plater->on_bed_type_change(bt);
    }
    if (touched_flush)
        wxGetApp().sidebar().set_flushing_volume_warning(is_flush_config_modified());

    if (proj_keys.count("filament_colour")) {
        // Same follow-ups as the sidebar colour picker
        // (PlaterPresetComboBox::sync_colour_config) choosing one colour, for
        // each slot whose colour changed: the slot becomes a solid colour
        // (filament_multi_colour = that colour, filament_colour_type "1"; a
        // gradient left there would keep drawing the old colours), its badge is
        // repainted (drawn from the config only on update()) and
        // EVT_FILAMENT_COLOR_CHANGED is queued (flush recalculation).
        const auto &colours = bundle->project_config.option<ConfigOptionStrings>("filament_colour")->values;
        auto       *multi   = bundle->project_config.option<ConfigOptionStrings>("filament_multi_colour");
        auto       *ctype   = bundle->project_config.option<ConfigOptionStrings>("filament_colour_type");
        auto       &combos  = wxGetApp().sidebar().combos_filament();
        for (size_t i = 0; i < colours.size(); ++i) {
            if (i < colours_before.size() && colours_before[i] == colours[i])
                continue;
            if (multi != nullptr) {
                if (i >= multi->values.size()) multi->values.resize(i + 1);
                multi->values[i] = colours[i];
            }
            if (ctype != nullptr) {
                if (i >= ctype->values.size()) ctype->values.resize(i + 1);
                ctype->values[i] = "1";
            }
            if (i < combos.size())
                combos[i]->update();
            wxCommandEvent *evt = new wxCommandEvent(EVT_FILAMENT_COLOR_CHANGED);
            evt->SetInt(int(i));
            wxQueueEvent(plater, evt);
        }
    }

    bundle->export_selections(*wxGetApp().app_config);
    // Once per request: it re-arms the auto-backup exporter.
    plater->update_project_dirty_from_presets();

    // Flip the flag synchronously. on_config_change only arms a 500 ms
    // one-shot timer, so a GET /status issued right after this 200 would
    // otherwise still report the stale slice as valid.
    plater->get_partplate_list().invalid_all_slice_result();

    // Most project keys are outside Plater::priv::config's whitelist and so
    // are ignored by the per-key loop, but the unconditional tail is what we
    // want: schedule_background_process + title dirty + auto-reslice.
    plater->on_config_change(bundle->full_config());

    if (touched_flush || touched_tower) {
        plater->get_view3D_canvas3D()->reload_scene(true);
        plater->update();
    }
}

Response Controller::handle_put_config(const std::string &body)
{
    // May throw nlohmann::json::parse_error - caught by the dispatch route
    // below and turned into a 400, distinct from the generic 500 path.
    nlohmann::json in = nlohmann::json::parse(body);
    if (!in.is_object())
        return { 400, {{"error", "body_must_be_object"}} };

    // NOTE (Task 8 lesson applied): `in` is captured BY VALUE (moved) into the
    // run_on_ui lambda, never by reference. run_on_ui blocks the calling (io)
    // thread up to 10s via future.wait_for and THROWS on timeout without
    // waiting for the queued CallAfter lambda to finish running on the GUI
    // thread. If we captured `in` by reference, a timeout would unwind this
    // stack frame (destroying `in`) while the still-queued lambda later runs
    // against freed memory - exactly the use-after-free that Task 8 hit.
    // Capturing by value/move means the lambda owns its own copy, independent
    // of this function's stack lifetime.
    nlohmann::json result = run_on_ui([in = std::move(in)]() -> nlohmann::json {
        auto *bundle = wxGetApp().preset_bundle;
        const bool allow_sensitive = sensitive_writes_allowed();

        struct Target {
            Preset::Type        type;
            DynamicPrintConfig  cfg;   // full copy of the edited preset config
            bool                touched { false };
        };
        std::array<Target, 3> targets {{
            { Preset::TYPE_PRINT,    bundle->prints.get_edited_preset().config,    false },
            { Preset::TYPE_FILAMENT, bundle->filaments.get_edited_preset().config, false },
            { Preset::TYPE_PRINTER,  bundle->printers.get_edited_preset().config,  false },
        }};

        // What the three presets hold now, for the range check (it judges only what changes).
        const std::array<DynamicPrintConfig, 3> targets_before{{targets[0].cfg, targets[1].cfg, targets[2].cfg}};
        // Layer heights waiting for the printer limits of the whole batch: target index and key.
        std::vector<size_t>      layer_checks;
        std::vector<std::string> layer_check_keys;
        nlohmann::json applied = nlohmann::json::array();
        nlohmann::json errors  = nlohmann::json::object();
        nlohmann::json out_of_range = nullptr; // the first out_of_range refusal, for the reply's top level
        std::vector<std::array<std::string, 3>> changes; // key, old, new -> notification
        // MCP panel: every applied key with its raw old and new value, in request order. type -1 = project.
        struct Staged { int type; std::string key, oldv, newv; };
        std::vector<Staged> staged;

        // Project scope (CFS / multi-material) is staged on its own copy. It is
        // checked AFTER the three presets so wipe_tower_rotation_angle - the one
        // key that lives in both s_project_options and the print preset - keeps
        // routing to the print preset exactly as it did before.
        DynamicPrintConfig     proj_new = bundle->project_config;
        std::set<std::string>  proj_keys;
        const int              nozzles  = bundle->get_printer_extruder_count();
        const auto            *colours0 = bundle->project_config.option<ConfigOptionStrings>("filament_colour");
        const size_t           filaments_before = colours0 == nullptr ? 0 : colours0->values.size();
        const std::vector<std::string> colours_before = colours0 == nullptr ? std::vector<std::string>{} : colours0->values;
        const int              plate_idx = wxGetApp().plater()->get_partplate_list().get_curr_plate_index();

        for (auto it = in.begin(); it != in.end(); ++it) {
            const std::string &key = it.key();
            Target *tgt = nullptr;
            for (auto &t : targets)
                if (t.cfg.option(key) != nullptr) { tgt = &t; break; }
            if (tgt == nullptr && proj_new.option(key) != nullptr) {
                if (!project_key_writable(key)) {
                    errors[key] = project_key_block_reason(key);
                    continue;
                }
                try {
                    std::string oldv = proj_new.opt_serialize(key);
                    std::string sval = json_value_to_config_string(it.value());
                    // Vet the raw text: the vector deserializers below cannot fail,
                    // so garbage would be stored as zeros and reported as applied.
                    std::string terr;
                    if (key == "flush_volumes_matrix" || key == "flush_multiplier" ||
                        key == "flush_multiplier_fast" || key == "wipe_tower_x" || key == "wipe_tower_y")
                        terr = numeric_list_error(sval);
                    else if (key == "filament_colour")
                        terr = colour_list_error(sval);
                    if (!terr.empty()) {
                        errors[key] = terr;
                        continue;
                    }
                    if (key == "wipe_tower_x" || key == "wipe_tower_y") {
                        // Per-plate vectors: the GUI writes them with set_at at the
                        // plate index (GLCanvas3D::WipeTowerInfo::apply_wipe_tower).
                        // A whole-vector deserialize would collapse them to a single
                        // element and move every other plate's tower.
                        ConfigOptionFloat one;
                        if (!one.deserialize(sval))
                            throw std::runtime_error("expected a number");
                        proj_new.option<ConfigOptionFloats>(key, true)->set_at(&one, plate_idx, 0);
                    } else {
                        proj_new.set_deserialize_strict(key, sval);
                    }
                    // handle_legacy (run inside set_deserialize) may rename or clear
                    // a key and still report success, which would otherwise be
                    // reported as applied while nothing was written.
                    if (proj_new.option(key) == nullptr) {
                        errors[key] = "key_dropped_by_legacy_handler";
                        continue;
                    }
                    proj_keys.insert(key);
                    applied.push_back(key);
                    // Re-serialized, not echoed: handle_legacy can rewrite the value.
                    changes.push_back({ key, oldv, proj_new.opt_serialize(key) });
                    staged.push_back({ -1, key, oldv, proj_new.opt_serialize(key) });
                } catch (const std::exception &e) {
                    errors[key] = e.what();
                }
                continue;
            }
            if (tgt == nullptr) {
                // F9: distinguish a real typo from a recognized Orca setting that
                // simply isn't editable through the preset configs (plate-scope and
                // computed-layer keys - GET /config serializes the merged config,
                // which is wider than anything writable).
                errors[key] = (print_config_def.get(key) != nullptr)
                                  ? "not_editable_in_current_config"
                                  : "unknown_key";
                continue;
            }
            try {
                std::string oldv = tgt->cfg.opt_serialize(key);
                std::string sval = json_value_to_config_string(it.value());
                const bool sensitive = is_sensitive_config_key(key);
                if (sensitive && sval.find(k_redacted) != std::string::npos) {
                    // A client writing back POST /preset/config output would replace a
                    // real credential with the placeholder.
                    errors[key] = "redacted_placeholder";
                    continue;
                }
                // Per-filament vector keys are merged across every loaded filament on
                // read but written to one preset here; a length change means the caller
                // handed us the merged shape (see is_merged_per_filament_key).
                const bool   per_filament = is_merged_per_filament_key(tgt->type, key, tgt->cfg.option(key));
                const size_t len_before   = per_filament ? config_vector_size(tgt->cfg, key) : 0;
                // Orca's own validation: throws BadOptionTypeException /
                // BadOptionValueException on garbage.
                tgt->cfg.set_deserialize_strict(key, sval);
                if (per_filament) {
                    const size_t len_after = config_vector_size(tgt->cfg, key);
                    if (len_after != len_before) {
                        // The batch is atomic and bails out below without applying, so the
                        // already-mutated staged copy needs no restore.
                        errors[key] = "per_filament_length_mismatch: the filament preset open in the "
                                      "Filament tab holds " + std::to_string(len_before) + " value(s), got " +
                                      std::to_string(len_after) +
                                      ". GET /config reports this key merged across every loaded filament "
                                      "slot; PUT /config writes one preset. Pass just that preset's value(s), "
                                      "and change the other slots by selecting their presets in turn.";
                        continue;
                    }
                }
                if (key == "layer_height" || key == "initial_layer_print_height") {
                    // Judged after the loop, against the batch's own printer limits: one request may lower
                    // min_layer_height and the layer height together (keys arrive sorted, layer_height first).
                    layer_checks.push_back(size_t(tgt - targets.data()));
                    layer_check_keys.push_back(key);
                } else {
                    // B1: OrcaSlicer's own range, judged on what changes (the staged copy is thrown away on a refusal).
                    const ConfigOption *was = targets_before[size_t(tgt - targets.data())].option(key);
                    nlohmann::json       rd;
                    std::string          rtext;
                    if (!config_value_in_range(key, was, *tgt->cfg.option(key), rd, rtext)) {
                        errors[key] = "out_of_range: " + rtext;
                        if (out_of_range.is_null()) out_of_range = rd;
                        continue;
                    }
                }
                std::string newv = tgt->cfg.opt_serialize(key);
                if (sensitive && !allow_sensitive && newv != oldv) {
                    // Judged on the re-serialized value, so writing back the current value
                    // (snapshot/restore callers) is not a change. The staged copy is thrown
                    // away with the rest of the batch.
                    errors[key] = "blocked_by_remote_api_policy";
                    continue;
                }
                tgt->touched = true;
                applied.push_back(key);
                staged.push_back({ int(tgt->type), key, oldv, newv });
                // The notification is on screen, so it never shows a credential.
                if (is_credential_key(key))
                    changes.push_back({ key, "(hidden)", "(hidden)" });
                else if (is_host_url_key(key))
                    changes.push_back({ key, strip_url_userinfo(oldv), strip_url_userinfo(newv) });
                else
                    changes.push_back({ key, oldv, newv });
            } catch (const std::exception &e) {
                errors[key] = e.what();
            }
        }

        // The layer heights against the printer limits as this batch leaves them (P6 / m13).
        for (size_t i = 0; i < layer_checks.size(); ++i) {
            const std::string  &key = layer_check_keys[i];
            const ConfigOption *was = targets_before[layer_checks[i]].option(key);
            const ConfigOption *now = targets[layer_checks[i]].cfg.option(key);
            nlohmann::json      rd;
            std::string         rtext;
            if (now != nullptr && !config_value_in_range(key, was, *now, rd, rtext, &targets[2].cfg)) {
                errors[key] = "out_of_range: " + rtext;
                if (out_of_range.is_null()) out_of_range = rd;
            }
        }

        // Second pass over the FINAL staged project config: the invariants are
        // cross-key (the matrix size depends on filament_colour), so they can only
        // be judged once the whole batch is written, not as each key arrives.
        for (const std::string &key : proj_keys) {
            std::string err = project_value_error(key, proj_new, nozzles, filaments_before);
            if (!err.empty()) errors[key] = err;
        }

        // Atomic: if any key failed validation, apply NOTHING and report errors.
        // (Previously, valid keys in a mixed batch applied before the 422.)
        if (!errors.empty()) {
            std::string emsg = "Couldn't update settings";
            for (auto it2 = errors.begin(); it2 != errors.end(); ++it2) {
                emsg += "\n - ";
                emsg += api_config_label(it2.key());
                if (it2.value() == "blocked_by_remote_api_policy")
                    emsg += " (blocked, see Preferences > Remote API)";
            }
            api_notify(emsg, true);
            // MCP panel: one refused row per rejected key (the batch applies nothing).
            for (auto it2 = errors.begin(); it2 != errors.end(); ++it2) {
                const std::string why = it2.value().is_string() ? it2.value().get<std::string>() : std::string("error");
                if (why == "blocked_by_remote_api_policy")
                    mcp_refused(mcp_setting_label(it2.key()), MCP_T("Not allowed"), k_needs_sensitive, "PUT /config");
                else if (why.rfind("out_of_range: ", 0) == 0)
                    mcp_refused(mcp_setting_label(it2.key()), MCP_T("Out of range"), why.substr(14), "PUT /config");
                else
                    mcp_refused(mcp_setting_label(it2.key()), mcp_reason(why, MCP_T("Invalid value")),
                                it2.key() + ": " + why, "PUT /config");
            }
            nlohmann::json reply = {{"applied", nlohmann::json::array()}, {"errors", errors}};
            // The first out-of-range key at the top level too: {"error":"out_of_range","key","value","min","max"}.
            if (!out_of_range.is_null())
                for (auto it3 = out_of_range.begin(); it3 != out_of_range.end(); ++it3) reply[it3.key()] = it3.value();
            return reply;
        }

        // MCP panel rows, built from the state before anything is applied (preset names, colours). A key
        // written back to the value it has changes nothing and gets no row.
        std::vector<McpRow> mcp_rows;
        for (const Staged &c : staged) {
            if (c.type < 0) {
                if (c.key == "filament_colour") {
                    // One row per slot whose colour changed.
                    const auto *after = proj_new.option<ConfigOptionStrings>("filament_colour");
                    if (after == nullptr) continue;
                    for (size_t i = 0; i < after->values.size(); ++i) {
                        const std::string b = i < colours_before.size() ? colours_before[i] : std::string();
                        if (b == after->values[i]) continue;
                        mcp_rows.push_back(mcp_config_row(c.key, mcp_fmt(MCP_T("Filament {n} colour"), {{"n", std::to_string(i + 1)}}), b,
                                                          after->values[i],
                                                          {{"type", "config"}, {"key", c.key}, {"scope", "project"}, {"index", i}},
                                                          std::string()));
                    }
                } else if (c.key == "wipe_tower_x" || c.key == "wipe_tower_y") {
                    // Per-plate vectors: the element of the current plate (written with set_at above).
                    const auto *ob = bundle->project_config.option<ConfigOptionFloats>(c.key);
                    const auto *na = proj_new.option<ConfigOptionFloats>(c.key);
                    if (na == nullptr || na->values.empty()) continue;
                    const std::string b = (ob == nullptr || ob->values.empty()) ? std::string()
                                              : ConfigOptionFloat(ob->get_at(size_t(plate_idx))).serialize();
                    const std::string a = ConfigOptionFloat(na->get_at(size_t(plate_idx))).serialize();
                    if (a == b) continue;
                    mcp_rows.push_back(mcp_config_row(c.key, c.key == "wipe_tower_x" ? MCP_T("Wipe tower X") : MCP_T("Wipe tower Y"), b, a,
                                                      {{"type", "config"}, {"key", c.key}, {"scope", "project"},
                                                       {"index", plate_idx}},
                                                      mcp_fmt(MCP_T("plate {n}"), {{"n", std::to_string(plate_idx + 1)}})));
                } else {
                    if (c.oldv == c.newv) continue;
                    mcp_rows.push_back(mcp_config_row(c.key, mcp_setting_label(c.key), c.oldv, c.newv,
                                                      {{"type", "config"}, {"key", c.key}, {"scope", "project"}},
                                                      std::string()));
                }
                continue;
            }
            if (c.oldv == c.newv) continue;
            const Preset::Type     type  = Preset::Type(c.type);
            const PresetCollection &coll = type == Preset::TYPE_PRINT    ? bundle->prints :
                                           type == Preset::TYPE_FILAMENT ? bundle->filaments : bundle->printers;
            const std::string preset = coll.get_selected_preset_name();
            const std::string label  = type == Preset::TYPE_FILAMENT ? mcp_edited_filament_label(mcp_setting_label(c.key, int(type)))
                                                                     : mcp_setting_label(c.key, int(type));
            const std::string where  = mcp_fmt(MCP_T("{preset} (unsaved)"), {{"preset", preset}});
            nlohmann::json    target = {{"type", "config"}, {"key", c.key}, {"scope", mcp_preset_type_name(type)},
                                        {"preset", preset}};
            const bool login_in_url = is_host_url_key(c.key) &&
                                      (c.oldv.find('@') != std::string::npos || c.newv.find('@') != std::string::npos);
            if (is_credential_key(c.key) || login_in_url) {
                // Never kept or shown: no raws, no undo (the change is still listed).
                McpRow r = mcp_row("setting", label,
                                   login_in_url ? strip_url_userinfo(c.newv) : std::string(MCP_T("(hidden)")),
                                   login_in_url ? strip_url_userinfo(c.oldv) : std::string(MCP_T("(hidden)")), where);
                r.target = std::move(target);
                mcp_rows.push_back(std::move(r));
                continue;
            }
            mcp_rows.push_back(mcp_config_row(c.key, label, c.oldv, c.newv, std::move(target), where));
        }
        if (!mcp_rows.empty()) mcp_before_change();

        // Apply through the GUI's own path: dirty markers + live panel refresh.
        // Same idiom as PlaterPresetComboBox::change_extruder_color()
        // (PresetComboBoxes.cpp): load_config() diffs+sets+update_dirty()+
        // reload_config(), then Plater::on_config_change() invalidates the
        // slice and repaints.
        for (auto &t : targets)
            if (t.touched) {
                Tab *tab = wxGetApp().get_tab(t.type);
                if (tab == nullptr) { // tab not built yet (shouldn't happen post_init)
                    errors["_tab"] = "tab_unavailable";
                    continue;
                }
                tab->load_config(t.cfg);
                wxGetApp().plater()->on_config_change(t.cfg);
            }

        apply_project_config(proj_new, proj_keys, colours_before);

        for (McpRow &row : mcp_rows) mcp_record(std::move(row));

        if (!changes.empty()) {
            std::string msg = "Settings updated";
            int shown = 0;
            for (auto &ch : changes) {
                if (shown < 6) {
                    msg += "\n - ";
                    msg += api_config_label(ch[0]);
                    msg += ": " + ch[1] + " -> " + ch[2] + api_config_unit(ch[0]);
                }
                ++shown;
            }
            if (shown > 6)
                msg += "\n(+ " + std::to_string(shown - 6) + " more)";
            api_notify(msg, false);
        }

        return {{"applied", applied}, {"errors", errors}};
    }, /*timeout_s=*/120); // a checkpoint export in mcp_before_change can be slow

    int status = result["errors"].empty() ? 200 : 422;
    return { status, result };
}

void Controller::set_slice_state(const std::function<void(SliceState&)> &mut, const char *event_name)
{
    nlohmann::json snapshot;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        mut(m_slice);
        snapshot = {{"event", event_name},
                    {"state", m_slice.state},
                    {"percent", m_slice.percent},
                    {"message", m_slice.message}};
        if (!m_slice.stats.is_null()) snapshot["stats"] = m_slice.stats;
    }
    wxGetApp().remote_api_server().broadcast(snapshot);
}

// Stats + warnings of the CURRENT plate's slice result, in the shape GET /slice/status
// reports. GUI thread only. Used when a slice completes, and (plates) whenever the
// current plate changes or an already-valid plate is sliced again, so /slice/status
// always describes the plate that slice, gcode and plate/render act on.
static void current_plate_slice_stats(nlohmann::json &stats, nlohmann::json &warnings)
{
    warnings = nlohmann::json::array();
    auto &plates = wxGetApp().plater()->get_partplate_list();
    const PrintStatistics &ps = plates.get_current_fff_print().print_statistics();
    stats = {
        {"estimated_time", ps.estimated_normal_print_time},
        {"filament_used_mm", ps.total_used_filament},
        {"filament_used_g", ps.total_weight},
        {"total_cost", ps.total_cost}
    };
    if (GCodeProcessorResult *res = plates.get_curr_plate()->get_slice_result()) {
        if (!res->print_statistics.modes.empty())
            stats["estimated_time_seconds"] = res->print_statistics.modes.front().time;
        for (const auto &w : res->warnings)
            warnings.push_back({{"level", w.level}, {"message", w.msg}, {"code", w.error_code}});
        // The "toolpath goes beyond the plate boundaries" warning is a GUI plater
        // notification (GLCanvas3D), computed from the gcode viewer's bed-containment
        // check - so it never reaches res->warnings and was invisible to the API.
        // Recompute it here from the same data the viewer uses (BuildVolume::
        // all_paths_inside over the extrude moves) and surface it in warnings[].
        BoundingBoxf3 paths_bbox;
        bool any_path = false;
        for (const auto &mv : res->moves) {
            if (mv.type == EMoveType::Extrude && mv.extrusion_role != erCustom &&
                mv.width != 0.f && mv.height != 0.f) {
                paths_bbox.merge(mv.position.cast<double>());
                any_path = true;
            }
        }
        if (any_path && !wxGetApp().plater()->build_volume().all_paths_inside(*res, paths_bbox))
            warnings.push_back({{"level", 3},
                                {"message", "A G-code path goes beyond the plate boundaries."},
                                {"code", "TOOLPATH_OUTSIDE"}});

        // F14: per-feature (extrusion-role) breakdown for the slice-analytics MCP.
        // PrintEstimatedStatistics has no per-role TIME, so sum it per role from the
        // moves (Normal mode, index 0 - matches estimated_time_seconds). Filament comes
        // from used_filaments_per_role; flow is the COMMANDED volumetric rate
        // (feedrate * mm3_per_mm), the right basis for detecting flow-ceiling clamping.
        // Role keys are STABLE Orca config-feature tokens (NOT localized role_to_string)
        // so the MCP predicted_flows keys match by string - see the fork-breakdown plan.
        auto role_token = [](int r) -> const char* {
            switch (r) {
                case erExternalPerimeter:        return "outer_wall";
                case erPerimeter:                return "inner_wall";
                case erInternalInfill:           return "sparse_infill";
                case erSolidInfill:              return "internal_solid_infill";
                case erTopSolidInfill:           return "top_surface";
                case erGapFill:                  return "gap_infill";
                case erBridgeInfill:             return "bridge";
                case erOverhangPerimeter:        return "overhang_perimeter";
                case erBottomSurface:            return "bottom_surface";
                case erIroning:                  return "ironing";
                case erInternalBridgeInfill:     return "internal_bridge";
                case erSkirt:                    return "skirt";
                case erBrim:                     return "brim";
                case erSupportMaterial:          return "support";
                case erSupportMaterialInterface: return "support_interface";
                case erSupportTransition:        return "support_transition";
                case erWipeTower:                return "wipe_tower";
                case erCustom:                   return "custom";
                case erMixed:                    return "mixed";
                default:                         return nullptr; // erNone / erCount
            }
        };
        struct RoleAgg { double time_s = 0.0, max_flow = 0.0, sum_ft = 0.0; };
        RoleAgg agg[erCount];
        for (const auto &mv : res->moves) {
            if (mv.type != EMoveType::Extrude) continue;
            int r = (int) mv.extrusion_role;
            if (r <= 0 || r >= erCount) continue;
            double t = mv.time[0];
            double vr = mv.volumetric_rate(); // feedrate * mm3_per_mm
            agg[r].time_s += t;
            agg[r].sum_ft += vr * t;
            if (vr > agg[r].max_flow) agg[r].max_flow = vr;
        }
        double total_time = res->print_statistics.modes.empty() ? 0.0
                            : (double) res->print_statistics.modes.front().time;
        const auto &fpr = res->print_statistics.used_filaments_per_role;
        nlohmann::json roles = nlohmann::json::array();
        for (int r = 1; r < erCount; ++r) {
            const char *tok = role_token(r);
            if (tok == nullptr) continue;
            auto it = fpr.find((ExtrusionRole) r);
            bool has_fil = (it != fpr.end());
            if (agg[r].time_s <= 0.0 && !has_fil) continue;
            nlohmann::json role_j = {
                {"role", tok},
                {"time_s", agg[r].time_s},
                {"time_pct", total_time > 0.0 ? 100.0 * agg[r].time_s / total_time : 0.0},
                {"flow_mm3_s", {{"max", agg[r].max_flow},
                                {"mean", agg[r].time_s > 0.0 ? agg[r].sum_ft / agg[r].time_s : 0.0}}}
            };
            if (has_fil) {
                // used_filaments_per_role.first is METERS (GUI convention); emit mm to
                // match top-level filament_used_mm. .second is grams.
                role_j["filament_mm"] = it->second.first * 1000.0;
                role_j["filament_g"]  = it->second.second;
            }
            roles.push_back(role_j);
        }
        stats["breakdown"] = {
            {"mode", "normal"},
            {"total_time_s", total_time},
            {"roles", roles},
            {"metrics", nlohmann::json::object()},
            {"layers", nlohmann::json::array()}
        };
    }
}

// plates: point /slice/status at the plate that is current NOW. The stored state
// otherwise keeps describing whichever plate was sliced last, so after a plate
// switch get_slice_status / slice_and_wait would report another plate's time and
// filament. GUI thread only.
void Controller::sync_slice_state_to_current_plate(const char *event_name)
{
    if (slice_state().state == "slicing") return;
    PartPlateList &plates = wxGetApp().plater()->get_partplate_list();
    PartPlate *plate = plates.get_curr_plate();
    if (plate != nullptr && plate->is_slice_result_valid()) {
        nlohmann::json stats, warnings;
        current_plate_slice_stats(stats, warnings);
        set_slice_state([&](SliceState &s) {
            s.state = "done"; s.percent = 100; s.message = "";
            s.stats = stats; s.warnings = warnings;
        }, event_name);
    } else {
        const std::string msg = "plate " + std::to_string(plates.get_curr_plate_index() + 1) + " is not sliced";
        set_slice_state([&](SliceState &s) {
            s.state = "idle"; s.percent = -1; s.message = msg;
            s.stats = nullptr; s.warnings = nlohmann::json::array();
        }, event_name);
    }
}

// Tracks which Plater the slice events are bound to. A GUI recreate (language/
// skin switch) builds a NEW Plater and the old Bind()s die with it, so we must
// rebind onto the new plater. File-static (only one Controller ever exists)
// keeps this out of the header. See final whole-branch review.
static Plater *s_bound_plater = nullptr;

void Controller::bind_plater_events()
{
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr || s_bound_plater == plater) return; // already bound to this plater
    s_bound_plater = plater;
    m_events_bound = true;

    plater->Bind(EVT_SLICING_UPDATE, [this](SlicingStatusEvent &evt) {
        evt.Skip(); // REQUIRED: let the Plater's own handler run (bound earlier = runs after us)
        if (evt.status.percent >= 0)
            set_slice_state([&](SliceState &s) {
                s.state   = "slicing";
                s.percent = evt.status.percent;
                s.message = evt.status.text;
            }, "slice.progress");
    });

    plater->Bind(EVT_PROCESS_COMPLETED, [this](SlicingProcessCompletedEvent &evt) {
        evt.Skip(); // REQUIRED (see above)
        // MCP panel: only a slice the API started gets a row (GUI slices and background re-slices don't).
        const bool api_slice = m_api_slice;
        m_api_slice = false;
        if (evt.error()) {
            auto msg = evt.format_error_message();
            set_slice_state([&](SliceState &s) {
                s.state = "error"; s.percent = -1; s.message = msg.first;
                s.stats = nullptr; s.warnings = nlohmann::json::array();
            }, "slice.error");
            if (api_slice) mcp_slice_finished(nullptr, false, msg.first);
            return;
        }
        if (evt.cancelled()) {
            set_slice_state([&](SliceState &s) {
                s.state = "idle"; s.percent = -1; s.message = "cancelled";
            }, "slice.cancelled");
            if (api_slice) mcp_slice_finished(nullptr, false, "cancelled");
            return;
        }
        // Success: harvest stats on the GUI thread (we ARE on it - wx handler).
        nlohmann::json stats, warnings;
        current_plate_slice_stats(stats, warnings);
        set_slice_state([&](SliceState &s) {
            s.state = "done"; s.percent = 100; s.message = "";
            s.stats = stats; s.warnings = warnings;
        }, "slice.done");
        if (api_slice) mcp_slice_finished(stats, true, std::string());
    });
}

Response Controller::handle_slice()
{
    // All checks + the state transition run on the GUI thread inside run_on_ui,
    // so they are serialized (no TOCTOU between the guard and the state change).
    nlohmann::json r = run_on_ui([this]() -> nlohmann::json {
        if (slice_state().state == "slicing")
            return {{"error", "already_slicing"}};
        Plater *plater = wxGetApp().plater();
        if (plater->model().objects.empty())
            return {{"error", "nothing_to_slice"}};
        // A prior validation failure (e.g. an object that sat outside the bed
        // when the background process last ran) stays LATCHED on the plate
        // (process_completed_with_error) and Plater::reslice() returns directly
        // on it without re-validating. GUI edits clear the latch via the
        // idle-time schedule_background_process() pump, but API-side model
        // mutations never run it - so a once-invalid plate could never slice
        // again over the API even after the geometry was fixed. Force a
        // synchronous re-validation here: it clears the latch when the model
        // has changed since the failure, and re-latches (-> slice_not_started
        // below) when the plate is still genuinely unsliceable.
        plater->update(false, /*force_background_processing_update=*/true);
        // reslice() no-ops on an already-valid plate -> no completion event ->
        // status would stick at "slicing". Report the existing result instead.
        if (plater->get_partplate_list().get_curr_plate()->is_slice_result_valid()) {
            // plates: the stored state may describe another plate, sliced after this
            // one; report this plate's own result with the reply.
            sync_slice_state_to_current_plate("slice.done");
            return {{"started", false}, {"already_valid", true}};
        }
        set_slice_state([](SliceState &s) {
            s.state = "slicing"; s.percent = 0; s.message = "starting";
            s.stats = nullptr; s.warnings = nlohmann::json::array();
        }, "slice.started");
        m_api_slice = !s_ui_trial; // the completion event records the MCP panel's slice row (not a trial's)
        plater->reslice();
        // F2: reslice() bails out synchronously - with NO completion event -
        // when the plate can't be sliced (update_background_process() INVALID,
        // e.g. an object fully outside the bed; or a prior hard error pinned on
        // the plate). m_is_slicing is only set on the started paths, so if it
        // is false here nothing is coming and the state would wedge at
        // "slicing" forever. Flip it to error instead.
        if (!plater->is_background_process_slicing()) {
            // 2.5.0: a plate whose result flag was cleared while its print stayed the
            // same (a new plate widening the plate grid) has nothing to redo: reslice()
            // starts no job and sets the flag again. That is the old result, not an error.
            // The state already says "slicing" here, which sync_slice_state_to_current_plate
            // leaves alone, so report the result directly.
            m_api_slice = false; // nothing started: no completion event is coming
            if (plater->get_partplate_list().get_curr_plate()->is_slice_result_valid()) {
                nlohmann::json stats, warnings;
                current_plate_slice_stats(stats, warnings);
                set_slice_state([&](SliceState &s) {
                    s.state = "done"; s.percent = 100; s.message = "";
                    s.stats = stats; s.warnings = warnings;
                }, "slice.done");
                return {{"started", false}, {"already_valid", true}};
            }
            // M15: OrcaSlicer's own reason (Print::validate(), the message its ValidateError notification shows: a
            // layer height above the nozzle's limit, say), else the generic one.
            std::string why, why_key;
            try {
                StringObjectException err = plater->get_partplate_list().get_current_fff_print().validate();
                plater->post_process_string_object_exception(err);
                why     = boost::algorithm::trim_copy(err.string);
                why_key = err.opt_key;
            } catch (const std::exception &e) {
                BOOST_LOG_TRIVIAL(warning) << "Remote API: validate after slice_not_started threw: " << e.what();
            }
            const std::string msg = why.empty() ? std::string("plate cannot be sliced (object outside bed or invalid state)") : why;
            set_slice_state([&msg](SliceState &s) {
                s.state = "error"; s.percent = -1;
                s.message = msg;
                s.stats = nullptr; s.warnings = nlohmann::json::array();
            }, "slice.error");
            mcp_slice_finished(nullptr, false, msg);
            nlohmann::json reply = {{"error", "slice_not_started"}, {"detail", msg}};
            if (!why_key.empty()) reply["key"] = why_key;
            return reply;
        }
        return {{"started", true}};
    });
    if (r.contains("error")) {
        if (r["error"] == "already_slicing") return { 409, r };
        return { 422, r };
    }
    if (r.value("already_valid", false)) return { 200, r };
    return { 202, r };
}

Response Controller::handle_slice_status()
{
    SliceState s = slice_state();
    nlohmann::json j = {{"state", s.state}, {"percent", s.percent}, {"message", s.message}};
    if (!s.stats.is_null()) {
        j["stats"] = s.stats;
        // F14: surface the per-role breakdown at TOP LEVEL - the slice-analytics MCP
        // reads status.breakdown (build_breakdown / prediction_check), not stats.breakdown.
        if (s.stats.is_object() && s.stats.contains("breakdown"))
            j["breakdown"] = s.stats["breakdown"];
    }
    j["warnings"] = s.warnings;
    // M3: a done slice may be out of date (a setting or an object changed since). The plate knows; ask the GUI
    // thread, but never wait on it for long (wait loops poll this route): without an answer in 300 ms, no field.
    if (s.state == "done") {
        auto promise = std::make_shared<std::promise<bool>>();
        auto fut     = promise->get_future();
        wxGetApp().CallAfter([promise] {
            // No answer while the plate list is being rebuilt (a project load or a panel restore).
            Plater *p = wxGetApp().plater();
            if (p == nullptr || p->is_loading_project() || McpActions::restoring()) return; // the io side times out
            bool valid = false;
            try {
                PartPlateList &pl = p->get_partplate_list();
                if (pl.get_curr_plate_index() < 0 || pl.get_curr_plate_index() >= pl.get_plate_count()) return;
                valid = pl.get_curr_plate() != nullptr && pl.get_curr_plate()->is_slice_result_valid();
            } catch (...) {}
            promise->set_value(valid);
        });
        if (fut.wait_for(std::chrono::milliseconds(300)) == std::future_status::ready) {
            try {
                const bool valid = fut.get();
                j["valid"] = valid;
                j["stale"] = !valid;
                if (!valid && s.message.empty())
                    j["message"] = "out of date: something changed since this slice; slice again for current numbers";
            } catch (const std::future_error &) {
                // The GUI declined to answer (a project load or restore): no field.
            }
        }
    }
    return { 200, j };
}

// F2: POST /api/v1/slice/cancel - abort a running slice, or unwedge a stale
// "slicing" state (previously the only recovery was killing the app). Stops the
// background process if it is actually running, then resets the API state; the
// EVT_PROCESS_COMPLETED(cancelled) handler, if it fires too, sets the same
// idle/cancelled state again (idempotent).
Response Controller::handle_slice_cancel()
{
    nlohmann::json r = run_on_ui([this]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        bool was_slicing = slice_state().state == "slicing";
        // MCP panel: the cancelled row is recorded here; the completion event (if it still fires) finds the
        // flag cleared and records nothing.
        const bool api_slice = m_api_slice;
        m_api_slice = false;
        if (plater != nullptr)
            plater->stop_background_slicing();
        if (api_slice) mcp_slice_finished(nullptr, false, "cancelled");
        set_slice_state([](SliceState &s) {
            s.state = "idle"; s.percent = -1; s.message = "cancelled";
            s.stats = nullptr; s.warnings = nlohmann::json::array();
        }, "slice.cancelled");
        return {{"cancelled", was_slicing}};
    });
    return { 200, r };
}

// M4a: POST /api/v1/model  body {"path":"<orca-host path>"}
// Accepts .stl/.obj/.3mf/.step/.stp (loaded via LoadStrategy::LoadModel, no embedded
// config). STEP import can raise modal dialogs that would wedge the single GUI
// thread with no remote way to dismiss: StepMeshDialog and the non-UTF8 name
// warning are gated by app-config flags forced safe for the duration of the load
// (deflection defaults come from app config); the multipart-object prompt in
// Plater::load_files is skipped while an API task is on the stack (keeps the
// solids as separate objects, the dialog's No answer).
Response Controller::handle_load_model(const std::string &body)
{
    nlohmann::json in = nlohmann::json::parse(body); // parse_error -> 400 in dispatch route
    if (!in.is_object() || !in.contains("path") || !in["path"].is_string())
        return { 400, {{"error", "missing_path"}} };
    std::string path = in["path"].get<std::string>();

    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return (char) std::tolower(c); });
    auto ends_with = [&lower](const char *suf) {
        size_t n = std::char_traits<char>::length(suf);
        return lower.size() >= n && lower.compare(lower.size() - n, n, suf) == 0;
    };
    const bool is_step = ends_with(".step") || ends_with(".stp");
    if (!(ends_with(".stl") || ends_with(".obj") || ends_with(".3mf") || is_step)) {
        api_notify("Can't load " + boost::filesystem::path(path).filename().string() + ": unsupported format", true);
        nlohmann::json e = {{"error", "unsupported_format"}, {"detail", "remote load supports .stl, .obj, .3mf, .step, .stp"}};
        try {
            run_on_ui([e]() -> nlohmann::json { mcp_refused_reply(MCP_T("Add model"), e, "POST /model"); return nullptr; });
        } catch (const std::exception &) {} // the panel row is not worth failing the reply over
        return { 422, e };
    }

    // 120 s instead of the default 10: OCCT read + tessellation of a real-world
    // STEP (and mesh import of a very large STL) can exceed 10 s, and the load
    // would still complete on the GUI thread after a ui_timeout was returned.
    nlohmann::json r = run_on_ui([path, is_step]() -> nlohmann::json {
        auto refuse = [&path](nlohmann::json e) -> nlohmann::json {
            e["path"] = path;
            mcp_refused_reply(mcp_fmt(MCP_T("Add {name}"), {{"name", mcp_file_name(path)}}), e, "POST /model");
            e.erase("path");
            return e;
        };
        if (nlohmann::json e = mcp_history_refusal(path); !e.is_null()) {
            e.erase("path");
            return refuse(e);
        }
        if (!boost::filesystem::exists(path))
            return refuse({{"error", "file_not_found"}}); // not "not_found": that token means a missing route (F18)
        Plater *plater = wxGetApp().plater();
        mcp_before_change();
        // Use the stored deflection defaults instead of StepMeshDialog and skip
        // the non-UTF8 name warning while loading; restore the user's flags after.
        AppConfig *cfg = wxGetApp().app_config;
        const bool prev_mesh_dlg  = cfg->get_bool("enable_step_mesh_setting");
        const bool prev_utf8_warn = cfg->get_bool("step_not_utf8_no_warn");
        if (is_step) {
            cfg->set_bool("enable_step_mesh_setting", false);
            cfg->set_bool("step_not_utf8_no_warn", true);
        }
        // File > Import's snapshot: without it the undo stack never moves, so the
        // project never reads as unsaved and new/open would drop the model unasked.
        Plater::TakeSnapshot snapshot(plater, "Import Object");
        // 2.5.0 dropped the std::string overload; it did this same conversion.
        std::vector<size_t> idxs = plater->load_files(std::vector<boost::filesystem::path>{ boost::filesystem::path(path) },
                                                      LoadStrategy::LoadModel);
        if (is_step) {
            cfg->set_bool("enable_step_mesh_setting", prev_mesh_dlg);
            cfg->set_bool("step_not_utf8_no_warn", prev_utf8_warn);
        }
        if (idxs.empty())
            return refuse({{"error", "load_failed"}});
        nlohmann::json objects = nlohmann::json::array();
        for (size_t i : idxs) {
            const ModelObject *mo = plater->model().objects[i];
            auto sz = mo->bounding_box_exact().size();
            objects.push_back({{"index", i}, {"name", mo->name},
                               {"size_mm", {sz.x(), sz.y(), sz.z()}}});
            McpRow row = mcp_row("model", MCP_T("Added"), mo->name, std::string(), mcp_size_text(*mo));
            row.title_hint = "added";
            row.target     = {{"type", "added"}, {"object_id", (uint64_t) mo->id().id}};
            mcp_undo(row, MCP_T("Remove"), mcp_fmt(MCP_T("Remove {name} from the plate"), {{"name", mo->name}}));
            mcp_record(std::move(row));
        }
        return {{"loaded", true}, {"objects", objects}};
    }, /*timeout_s=*/120);
    std::string fn = boost::filesystem::path(path).filename().string();
    if (r.contains("error")) {
        api_notify("Couldn't load " + fn, true);
        if (r["error"] == "file_not_found") return { 404, r };
        if (r["error"] == "protected_path") return { 403, r };
        return { 422, r };
    }
    api_notify("Added " + fn + " to the plate");
    return { 200, r };
}

// The filament preset of one slot, as the sidebar's per-filament combo switches it (Plater::priv::on_select_preset()
// for a filament combo): touches only that slot, drops no unsaved edits. PUT /preset with a slot, and the MCP
// panel's Switch back. GUI thread only; the caller has checked the slot and the preset.
void Controller::select_filament_slot(size_t slot, const std::string &name)
{
    auto    *bundle  = wxGetApp().preset_bundle;
    Plater  *plater  = wxGetApp().plater();
    Sidebar &sidebar = plater->sidebar();
    const bool was_support = is_support_filament(int(slot));
    bundle->set_filament_preset(slot, name);
    plater->update_project_dirty_from_presets();
    bundle->export_selections(*wxGetApp().app_config);
    sidebar.update_dynamic_filament_list();
    // A switch between a support filament and a normal one changes the flush volumes.
    if (was_support != is_support_filament(int(slot)) && wxGetApp().app_config->get("auto_calculate_flush") == "all")
        sidebar.auto_calc_flushing_volumes(int(slot));
    plater->on_filament_change(slot);
    auto &combos = sidebar.combos_filament();
    if (slot < combos.size())
        combos[slot]->update();
}

// M4a: PUT /api/v1/preset  body {"type":"print|filament|printer","name":"..."}
Response Controller::handle_select_preset(const std::string &body)
{
    nlohmann::json in = nlohmann::json::parse(body);
    if (!in.is_object() || !in.contains("type") || !in.contains("name")
        || !in["type"].is_string() || !in["name"].is_string())
        return { 400, {{"error", "missing_fields"}} };
    std::string type_s = in["type"].get<std::string>();
    std::string name   = in["name"].get<std::string>();
    Preset::Type type = type_s == "print"    ? Preset::TYPE_PRINT :
                        type_s == "filament" ? Preset::TYPE_FILAMENT :
                        type_s == "printer"  ? Preset::TYPE_PRINTER : Preset::TYPE_INVALID;
    if (type == Preset::TYPE_INVALID)
        return { 400, {{"error", "unknown_type"}} };

    // Optional "slot" (0-based): choose the filament of one extruder/tool on a
    // multi-filament printer, as the sidebar's per-filament combo does. This
    // touches only that slot, so unlike the Filament tab path below it discards
    // no unsaved edits. Without "slot" the request behaves exactly as before.
    if (in.contains("slot")) {
        if (type != Preset::TYPE_FILAMENT || !in["slot"].is_number_integer() || in["slot"].get<long long>() < 0)
            return { 400, {{"error", "invalid_slot"}} };
        const size_t slot = in["slot"].get<size_t>();
        nlohmann::json r = run_on_ui([slot, name]() -> nlohmann::json {
            auto    *bundle  = wxGetApp().preset_bundle;
            Plater  *plater  = wxGetApp().plater();
            Sidebar &sidebar = plater->sidebar();
            const std::string slot_label = mcp_fmt(MCP_T("Filament {n}"), {{"n", std::to_string(slot + 1)}});
            auto refuse = [&](nlohmann::json e) -> nlohmann::json {
                e["detail"] = name;
                mcp_refused_reply(slot_label, e, "PUT /preset");
                e.erase("detail");
                return e;
            };
            if (slot >= bundle->filament_presets.size())
                return refuse({{"error", "slot_out_of_range"}, {"filament_count", bundle->filament_presets.size()}});
            const Preset *preset = bundle->filaments.find_preset(name, false, true);
            if (preset == nullptr)
                return refuse({{"error", "unknown_preset"}});
            if (!preset->is_compatible)
                return refuse({{"error", "incompatible_preset"}});
            // Single-filament printer: slot 0 is the Filament tab's preset.
            if (!sidebar.is_multifilament())
                return {{"use_tab", true}};
            // MCP panel: a slot switched to the preset it already has changes nothing.
            const std::string before = bundle->filament_presets[slot];
            if (before != name) mcp_before_change();
            select_filament_slot(slot, name);
            if (before != name) {
                McpRow row = mcp_row("preset", slot_label, name, before);
                row.target = {{"type", "preset"}, {"preset_type", "filament"}, {"slot", slot}, {"before", before},
                              {"after", name}, {"discarded", nlohmann::json::array()}};
                mcp_undo(row, MCP_T("Switch back"),
                         mcp_fmt(MCP_T("Switch {label} back to {before}"), {{"label", slot_label}, {"before", before}}));
                mcp_record(std::move(row));
            }
            return {{"selected", name}, {"slot", slot}, {"filaments", bundle->filament_presets}};
        }, /*timeout_s=*/120);
        if (!r.contains("use_tab")) {
            if (r.contains("error")) {
                const std::string err = r["error"].get<std::string>();
                if (err == "unknown_preset" || err == "incompatible_preset" || err == "slot_out_of_range") {
                    api_notify("Can't use filament preset '" + name + "' for filament " + std::to_string(slot + 1), true);
                    return { 422, r };
                }
                return { 500, r };
            }
            api_notify("Filament " + std::to_string(slot + 1) + " switched to '" + name + "'");
            return { 200, r };
        }
    }

    nlohmann::json r = run_on_ui([type, name]() -> nlohmann::json {
        auto *bundle = wxGetApp().preset_bundle;
        PresetCollection &presets = type == Preset::TYPE_PRINT    ? bundle->prints :
                                    type == Preset::TYPE_FILAMENT ? bundle->filaments :
                                                                    bundle->printers;
        const std::string group_label = type == Preset::TYPE_PRINT    ? MCP_T("Print preset") :
                                        type == Preset::TYPE_FILAMENT ? MCP_T("Filament") : MCP_T("Printer");
        auto refuse = [&](nlohmann::json e) -> nlohmann::json {
            e["detail"] = name;
            mcp_refused_reply(group_label, e, "PUT /preset");
            e.erase("detail");
            return e;
        };
        // Validate up front: select_preset silently falls back to a visible preset
        // for an unknown name, so its return can't be trusted for a 422.
        if (presets.find_preset(name, false, true) == nullptr)
            return refuse({{"error", "unknown_preset"}});
        Tab *tab = wxGetApp().get_tab(type);
        if (tab == nullptr) return refuse({{"error", "tab_unavailable"}});
        // MCP panel: the switch is recorded unless it changes nothing (same preset, no edits to drop).
        const std::string before   = presets.get_selected_preset_name();
        const bool        any_edit = bundle->prints.current_is_dirty() || bundle->filaments.current_is_dirty() ||
                                     bundle->sla_materials.current_is_dirty() || bundle->printers.current_is_dirty();
        const bool        record   = before != name || any_edit;
        if (record) mcp_before_change();
        // Discard un-applied GUI edits on EVERY collection Tab::select_preset may
        // inspect. It consults dependent tabs too (switching a print/printer preset
        // checks the filament/print collections' dirty state), and ANY dirty one
        // pops a modal UnsavedChangesDialog that would wedge the GUI thread with no
        // remote way to dismiss it. Discarding is acceptable for an automation API
        // (callers apply changes deliberately via PUT /config).
        //
        // It is, however, a bigger side effect than "switch this group" suggests: a
        // filament switch also drops unsaved print/printer edits, including ones made
        // by hand in the GUI. Narrowing it to the selected collection is not an option
        // (that is exactly the modal deadlock above), so instead report every group we
        // discarded, and name them in the on-screen notification, so the loss is
        // visible to both the caller and whoever is at the machine.
        nlohmann::json discarded = nlohmann::json::array();
        auto discard_if_dirty = [&discarded](PresetCollection &c, const char *group) {
            if (c.current_is_dirty()) {
                c.discard_current_changes();
                discarded.push_back(group);
            }
        };
        discard_if_dirty(bundle->prints, "print");
        discard_if_dirty(bundle->filaments, "filament");
        discard_if_dirty(bundle->sla_materials, "sla_material");
        discard_if_dirty(bundle->printers, "printer");
        bool ok = tab->select_preset(name, false, "", /*force_select=*/true, /*force_no_transfer=*/true);
        if (!ok) return refuse({{"error", "select_cancelled"}});
        if (record) {
            std::string where;
            if (!discarded.empty()) {
                std::string list;
                for (size_t i = 0; i < discarded.size(); ++i)
                    list += (i ? ", " : "") + discarded[i].get<std::string>();
                where = mcp_fmt(MCP_T("Unsaved edits dropped: {list}"), {{"list", list}});
            }
            McpRow row = mcp_row("preset", group_label, name, before, where);
            row.target = {{"type", "preset"}, {"preset_type", mcp_preset_type_name(type)}, {"slot", nullptr},
                          {"before", before}, {"after", name}, {"discarded", discarded}};
            // The panel's Switch back drops unsaved edits the same way this switch did (no dialog): say so.
            if (before != name)
                mcp_undo(row, MCP_T("Switch back"),
                         mcp_fmt(MCP_T("Switch {label} back to {before} (drops unsaved edits)"),
                                 {{"label", group_label}, {"before", before}}));
            mcp_record(std::move(row));
        }
        return {{"selected", name}, {"discarded_changes", discarded}};
    }, /*timeout_s=*/120);
    if (r.contains("error")) {
        if (r["error"] == "unknown_preset") {
            api_notify("No such " + type_s + " preset '" + name + "'", true);
            return { 422, r };
        }
        return { 500, r };
    }
    std::string msg = "Switched to " + type_s + " preset '" + name + "'";
    if (r.contains("discarded_changes") && ! r["discarded_changes"].empty()) {
        msg += " - discarded unsaved changes in:";
        for (const auto &g : r["discarded_changes"])
            msg += " " + g.get<std::string>();
    }
    api_notify(msg);
    return { 200, r };
}

// POST /api/v1/preset/save  body {"type":"print|filament|printer","name":"...","detach":false?}
// Persists the collection's currently edited settings as a named user preset
// (create or update) via Tab::save_preset, which with a non-empty name runs the
// GUI Save flow without any dialog.
Response Controller::handle_save_preset(const std::string &body)
{
    nlohmann::json in = nlohmann::json::parse(body);
    if (!in.is_object() || !in.contains("type") || !in.contains("name")
        || !in["type"].is_string() || !in["name"].is_string())
        return { 400, {{"error", "missing_fields"}} };
    std::string type_s = in["type"].get<std::string>();
    std::string name   = in["name"].get<std::string>();
    bool detach = in.value("detach", false);
    Preset::Type type = type_s == "print"    ? Preset::TYPE_PRINT :
                        type_s == "filament" ? Preset::TYPE_FILAMENT :
                        type_s == "printer"  ? Preset::TYPE_PRINTER : Preset::TYPE_INVALID;
    if (type == Preset::TYPE_INVALID)
        return { 400, {{"error", "unknown_type"}} };
    // SavePresetDialog normally enforces the name rules; mirror them headless.
    while (!name.empty() && (name.front() == ' ' || name.back() == ' ')) {
        if (name.front() == ' ') name.erase(name.begin());
        else name.pop_back();
    }
    static const std::string unusable_symbols = "<>[]:/\\|?*\"";
    bool bad_char = name.find_first_of(unusable_symbols) != std::string::npos;
    for (char c : name)
        if (static_cast<unsigned char>(c) < 0x20) bad_char = true;
    if (name.empty() || name == "." || name == ".." || name.size() > 128 || bad_char)
        return { 400, {{"error", "invalid_name"}} };

    nlohmann::json r = run_on_ui([type, name, detach]() -> nlohmann::json {
        auto *bundle = wxGetApp().preset_bundle;
        PresetCollection &presets = type == Preset::TYPE_PRINT    ? bundle->prints :
                                    type == Preset::TYPE_FILAMENT ? bundle->filaments :
                                                                    bundle->printers;
        const std::string tname = mcp_preset_type_name(type);
        auto refuse = [&](nlohmann::json e) -> nlohmann::json {
            e["detail"] = name;
            mcp_refused_reply(mcp_fmt(MCP_T("Save {type} preset"), {{"type", tname}}), e, "POST /preset/save");
            e.erase("detail");
            return e;
        };
        const Preset *existing = presets.find_preset(name, false);
        if (existing != nullptr && (existing->is_system || existing->is_default))
            return refuse({{"error", "name_reserved"}});
        Tab *tab = wxGetApp().get_tab(type);
        if (tab == nullptr) return refuse({{"error", "tab_unavailable"}});
        bool created = existing == nullptr;
        mcp_before_change();
        tab->save_preset(name, detach, /*save_to_project=*/false);
        // save_preset returns void; confirm the preset actually landed.
        const Preset *saved = presets.find_preset(name, false);
        if (saved == nullptr) return refuse({{"error", "save_failed"}});
        McpRow row = mcp_row("preset", MCP_T("Saved preset"), name, std::string(),
                             created ? mcp_fmt(MCP_T("New {type} preset"), {{"type", tname}})
                                     : mcp_fmt(MCP_T("Replaced the saved {type} preset"), {{"type", tname}}));
        row.on_disk = true;
        row.target  = {{"type", "preset_saved"}, {"preset_type", tname}, {"name", name}, {"created", created}};
        if (created)
            mcp_undo(row, MCP_T("Delete preset"),
                     mcp_fmt(MCP_T("Delete the {type} preset {name}"), {{"type", tname}, {"name", name}}));
        mcp_record(std::move(row));
        return {{"saved", name}, {"created", created}};
    }, /*timeout_s=*/120);
    if (r.contains("error")) {
        if (r["error"] == "name_reserved") {
            api_notify("Can't overwrite built-in " + type_s + " preset '" + name + "'", true);
            return { 409, r };
        }
        return { 500, r };
    }
    api_notify((r["created"].get<bool>() ? "Saved new " : "Updated ") + type_s + " preset '" + name + "'");
    return { 200, r };
}

// GET /api/v1/presets -> preset names per collection, with system/selected flags.
Response Controller::handle_get_presets()
{
    nlohmann::json r = run_on_ui([]() -> nlohmann::json {
        auto *bundle = wxGetApp().preset_bundle;
        auto dump = [](const PresetCollection &c) {
            nlohmann::json arr = nlohmann::json::array();
            const std::string sel = c.get_selected_preset_name();
            for (const Preset &p : c.get_presets()) {
                if (p.is_default) continue;
                arr.push_back({{"name", p.name}, {"system", p.is_system},
                               {"visible", p.is_visible}, {"selected", p.name == sel}});
            }
            return arr;
        };
        return {{"print", dump(bundle->prints)},
                {"filament", dump(bundle->filaments)},
                {"printer", dump(bundle->printers)}};
    });
    return { 200, r };
}

// PUT /api/v1/objects/{id}/layer_height  body {"mode":"adaptive","quality":0..1} | {"mode":"reset"}
// Mirrors GLCanvas3D::LayersEditing::adaptive_layer_height_profile / reset_layer_height_profile.
Response Controller::handle_put_layer_height(uint64_t id, const std::string &body)
{
    nlohmann::json in = nlohmann::json::parse(body);
    if (!in.is_object() || !in.contains("mode") || !in["mode"].is_string())
        return { 400, {{"error", "missing_mode"}, {"detail", "adaptive|reset"}} };
    std::string mode = in["mode"].get<std::string>();
    if (mode != "adaptive" && mode != "reset")
        return { 400, {{"error", "unknown_mode"}, {"detail", "adaptive|reset"}} };
    double quality = in.value("quality", 0.5);
    if (!(quality >= 0.0 && quality <= 1.0))
        return { 400, {{"error", "quality_out_of_range"}, {"detail", "0..1"}} };

    nlohmann::json r = run_on_ui([id, mode, quality]() -> nlohmann::json {
        const std::string action = mode == "reset" ? MCP_T("Reset layer height") : MCP_T("Adaptive layer height");
        auto refuse = [&](nlohmann::json e) -> nlohmann::json {
            mcp_refused_reply(action, e, "PUT /objects/{id}/layer_height");
            return e;
        };
        Plater *plater = wxGetApp().plater();
        int idx = find_object_index(plater->model(), id);
        if (idx < 0) return refuse({{"error", "unknown_object"}});
        ModelObject *mo = plater->model().objects[idx];
        // MCP panel: the profile before, for "Undo".
        const std::vector<double> profile_before = mo->layer_height_profile.get();
        auto record_profile = [&](const char *label, const std::string &where) {
            McpRow row = mcp_row("setting", label, mo->name, std::string(), where);
            row.target = {{"type", "layer_profile"}, {"object_id", id}, {"before", profile_before},
                          {"after", mo->layer_height_profile.get()}};
            mcp_undo(row, MCP_T("Undo"),
                     mcp_fmt(MCP_T("Put the previous layer height profile of {object} back"), {{"object", mo->name}}));
            mcp_record(std::move(row));
        };
        if (mode == "reset") {
            mcp_before_change();
            mo->layer_height_profile.clear();
            plater->schedule_background_process();
            wxGetApp().obj_list()->update_info_items(idx);
            api_notify("Reset layer height profile on '" + mo->name + "'");
            record_profile(MCP_T("Layer height reset"), std::string());
            return {{"id", id}, {"mode", "reset"}};
        }
        const DynamicPrintConfig full_cfg = wxGetApp().preset_bundle->full_config();
        // object_max_z 0 -> computed from the object's raw bounding box inside.
        SlicingParameters sp = PrintObject::slicing_parameters(
            full_cfg, *mo, 0.f, plater->fff_print().shrinkage_compensation());
        std::vector<double> profile = layer_height_profile_adaptive(sp, *mo, (float) quality);
        if (profile.size() < 4)
            return refuse({{"error", "profile_failed"}});
        mcp_before_change();
        mo->layer_height_profile.set(std::move(profile));
        const std::vector<double> &prof = mo->layer_height_profile.get();
        plater->schedule_background_process();
        wxGetApp().obj_list()->update_info_items(idx);
        double hmin = prof[1], hmax = prof[1];
        for (size_t i = 1; i < prof.size(); i += 2) {
            hmin = std::min(hmin, prof[i]);
            hmax = std::max(hmax, prof[i]);
        }
        api_notify("Applied adaptive layer height to '" + mo->name + "'");
        record_profile(MCP_T("Adaptive layer height"),
                       mcp_fmt(MCP_T("Quality {value}"), {{"value", McpRecorder::num_text(quality)}}));
        return {{"id", id}, {"mode", "adaptive"}, {"quality", quality},
                {"points", (unsigned) (prof.size() / 2)},
                {"layer_height_min", hmin}, {"layer_height_max", hmax}};
    }, /*timeout_s=*/120);
    if (r.contains("error"))
        return { r["error"] == "unknown_object" ? 404 : 422, r };
    return { 200, r };
}

// PUT /api/v1/objects/{id}/height_range
//   body {"min_z":a,"max_z":b,"layer_height":h}  (exact same range = update)
//      | {"clear":true}                          (remove all ranges)
Response Controller::handle_put_height_range(uint64_t id, const std::string &body)
{
    nlohmann::json in = nlohmann::json::parse(body);
    if (!in.is_object())
        return { 400, {{"error", "body_must_be_object"}} };
    bool clear = in.value("clear", false);
    double min_z = in.value("min_z", -1.0);
    double max_z = in.value("max_z", -1.0);
    double lh    = in.value("layer_height", 0.0);
    if (!clear) {
        if (!in.contains("min_z") || !in.contains("max_z") || !in.contains("layer_height"))
            return { 400, {{"error", "missing_fields"}, {"detail", "min_z, max_z, layer_height (or clear:true)"}} };
        if (!(min_z >= 0.0) || !(max_z > min_z))
            return { 400, {{"error", "bad_range"}, {"detail", "need 0 <= min_z < max_z"}} };
    }

    nlohmann::json r = run_on_ui([id, clear, min_z, max_z, lh]() -> nlohmann::json {
        const std::string range_label = mcp_fmt(MCP_T("Height range {min}\xE2\x80\x93{max} mm"),
                                                {{"min", McpRecorder::num_text(min_z)}, {"max", McpRecorder::num_text(max_z)}});
        auto refuse = [&](nlohmann::json e) -> nlohmann::json {
            mcp_refused_reply(clear ? std::string(MCP_T("Clear height ranges")) : range_label, e,
                              "PUT /objects/{id}/height_range");
            return e;
        };
        auto config_map = [](const ModelConfig &c) {
            nlohmann::json m = nlohmann::json::object();
            for (const std::string &k : c.keys()) m[k] = c.opt_serialize(k);
            return m;
        };
        Plater *plater = wxGetApp().plater();
        int idx = find_object_index(plater->model(), id);
        if (idx < 0) return refuse({{"error", "unknown_object"}});
        ModelObject *mo = plater->model().objects[idx];
        auto ranges_json = [mo]() {
            nlohmann::json a = nlohmann::json::array();
            for (const auto &kv : mo->layer_config_ranges)
                a.push_back({{"min_z", kv.first.first}, {"max_z", kv.first.second},
                             {"layer_height", kv.second.has("layer_height") ? kv.second.opt_float("layer_height") : 0.0}});
            return a;
        };
        if (clear) {
            // MCP panel: the ranges as they were, for "Put back". Clearing none changes nothing.
            nlohmann::json was = nlohmann::json::array();
            for (const auto &kv : mo->layer_config_ranges)
                was.push_back({{"min_z", kv.first.first}, {"max_z", kv.first.second}, {"config", config_map(kv.second)}});
            if (!was.empty()) mcp_before_change();
            Plater::TakeSnapshot snapshot(plater, "Delete Height Range");
            mo->layer_config_ranges.clear();
            plater->changed_object(idx);
            wxGetApp().obj_list()->update_info_items(idx);
            api_notify("Cleared height ranges on '" + mo->name + "'");
            if (!was.empty()) {
                McpRow row = mcp_row("setting", MCP_T("Height ranges cleared"),
                                     mcp_fmt(was.size() == 1 ? MCP_T("{n} range") : MCP_T("{n} ranges"),
                                             {{"n", std::to_string(was.size())}}),
                                     std::string(), mo->name);
                row.target = {{"type", "height_ranges_cleared"}, {"object_id", id}, {"ranges", was}};
                mcp_undo(row, MCP_T("Put back"),
                         mcp_fmt(MCP_T("Put the height ranges of {object} back"), {{"object", mo->name}}));
                mcp_record(std::move(row));
            }
            return {{"id", id}, {"height_ranges", ranges_json()}};
        }
        // Printer limits, same rules as ObjectList's get_min/max_layer_height.
        const DynamicPrintConfig &pcfg = wxGetApp().preset_bundle->printers.get_edited_preset().config;
        double minh = printer_min_layer_height(&pcfg); // the lowest extruder's, never below 0.01 mm (B1 / I4, m12)
        double maxh = pcfg.opt_float("max_layer_height", 0);
        if (maxh < EPSILON) maxh = 0.75 * pcfg.opt_float("nozzle_diameter", 0);
        if (lh < minh || lh > maxh)
            return refuse({{"error", "layer_height_out_of_range"}, {"key", "layer_height"}, {"value", lh}, {"min", minh}, {"max", maxh},
                           {"detail", "layer height must be within " + McpRecorder::num_text(minh) + " to " +
                                          McpRecorder::num_text(maxh) + " mm"}});
        t_layer_height_range key{ min_z, max_z };
        for (const auto &kv : mo->layer_config_ranges) {
            if (kv.first == key) continue;
            if (min_z < kv.first.second && kv.first.first < max_z)
                return refuse({{"error", "overlaps_existing"},
                               {"existing", {kv.first.first, kv.first.second}}});
        }
        // MCP panel: an existing range with the same bounds is updated; its earlier config is the "before".
        nlohmann::json range_before = nullptr;
        std::string    lh_before;
        {
            auto it = mo->layer_config_ranges.find(key);
            if (it != mo->layer_config_ranges.end()) {
                range_before = config_map(it->second);
                if (it->second.has("layer_height")) lh_before = mcp_fmt(MCP_T("layer {height} mm"),
                                                                           {{"height", McpRecorder::num_text(it->second.opt_float("layer_height"))}});
            }
        }
        DynamicPrintConfig c;
        c.set_key_value("layer_height", new ConfigOptionFloat(lh));
        c.set_key_value("extruder",     new ConfigOptionInt(0));
        mcp_before_change();
        Plater::TakeSnapshot snapshot(plater, "Add Height Range");
        mo->layer_config_ranges[key].assign_config(std::move(c));
        plater->changed_object(idx);
        wxGetApp().obj_list()->update_info_items(idx);
        api_notify("Set height range " + std::to_string(min_z) + "-" + std::to_string(max_z) + " mm on '" + mo->name + "'");
        {
            McpRow row = mcp_row("setting", range_label, mcp_fmt(MCP_T("layer {height} mm"), {{"height", McpRecorder::num_text(lh)}}),
                                 lh_before, mo->name);
            row.target = {{"type", "height_range"}, {"object_id", id}, {"min_z", min_z}, {"max_z", max_z},
                          {"before", range_before}, {"after", config_map(mo->layer_config_ranges[key])}};
            if (range_before.is_null())
                mcp_undo(row, MCP_T("Remove range"),
                         mcp_fmt(MCP_T("Remove the {min}\xE2\x80\x93{max} mm range from {object}"),
                                 {{"min", McpRecorder::num_text(min_z)}, {"max", McpRecorder::num_text(max_z)},
                                  {"object", mo->name}}));
            else
                mcp_undo(row, MCP_T("Revert"),
                         mcp_fmt(MCP_T("Set the {min}\xE2\x80\x93{max} mm range of {object} back to {before}"),
                                 {{"min", McpRecorder::num_text(min_z)}, {"max", McpRecorder::num_text(max_z)},
                                  {"object", mo->name}, {"before", lh_before}}));
            mcp_record(std::move(row));
        }
        return {{"id", id}, {"height_ranges", ranges_json()}};
    }, /*timeout_s=*/120);
    if (r.contains("error"))
        return { r["error"] == "unknown_object" ? 404 : 422, r };
    return { 200, r };
}

// Shared: parse {"type","name"} and resolve the collection. Returns nullptr collection on bad type.
static PresetCollection *api_preset_collection(const std::string &type_s)
{
    auto *bundle = wxGetApp().preset_bundle;
    return type_s == "print"    ? &bundle->prints :
           type_s == "filament" ? &bundle->filaments :
           type_s == "printer"  ? &bundle->printers : nullptr;
}

// POST /api/v1/preset/config  body {"type","name"} -> full config of that named preset.
// POST (not GET) so names with spaces/@ need no URL encoding; read-only.
Response Controller::handle_get_preset_config(const std::string &body)
{
    nlohmann::json in = nlohmann::json::parse(body);
    if (!in.is_object() || !in.contains("type") || !in.contains("name")
        || !in["type"].is_string() || !in["name"].is_string())
        return { 400, {{"error", "missing_fields"}} };
    std::string type_s = in["type"].get<std::string>();
    std::string name   = in["name"].get<std::string>();
    nlohmann::json r = run_on_ui([type_s, name]() -> nlohmann::json {
        PresetCollection *presets = api_preset_collection(type_s);
        if (presets == nullptr) return {{"error", "unknown_type"}};
        const Preset *p = presets->find_preset(name, false);
        if (p == nullptr) return {{"error", "unknown_preset"}};
        nlohmann::json cfg = nlohmann::json::object();
        for (const std::string &k : p->config.keys()) {
            std::string v = p->config.opt_serialize(k);
            if (is_credential_key(k) && !v.empty())
                v = k_redacted;
            else if (is_host_url_key(k))
                v = strip_url_userinfo(v);
            cfg[k] = v;
        }
        return {{"name", p->name}, {"system", p->is_system}, {"config", cfg}};
    });
    if (r.contains("error")) {
        if (r["error"] == "unknown_type") return { 400, r };
        return { 404, r };
    }
    return { 200, r };
}

// Why DELETE /preset (and the MCP panel's "Delete preset") will not delete a preset, or "" when it may go: never a
// system or default preset, never the selected one, never one other presets inherit from (the GUI's rules).
// type_s = print | filament | printer. GUI thread only.
std::string Controller::preset_delete_refusal(const std::string &type_s, const std::string &name)
{
    PresetCollection *presets = api_preset_collection(type_s);
    if (presets == nullptr) return "unknown_type";
    const Preset *p = presets->find_preset(name, false);
    if (p == nullptr) return "unknown_preset";
    if (p->is_system || p->is_default) return "builtin_preset";
    if (presets->get_selected_preset_name() == name) return "preset_selected";
    for (const Preset &other : presets->get_presets())
        if (other.name != name && other.inherits() == name) return "has_children";
    return {};
}

// Removes a user preset (file + list entry) and refreshes the tab and the sidebar combo. Check
// preset_delete_refusal first. GUI thread only.
bool Controller::delete_user_preset(const std::string &type_s, const std::string &name)
{
    PresetCollection *presets = api_preset_collection(type_s);
    if (presets == nullptr || !presets->delete_preset(name)) return false;
    const Preset::Type type = type_s == "print" ? Preset::TYPE_PRINT :
                              type_s == "filament" ? Preset::TYPE_FILAMENT : Preset::TYPE_PRINTER;
    // Refresh the tab's preset list + the plater sidebar combo.
    if (Tab *tab = wxGetApp().get_tab(type); tab != nullptr)
        tab->update_tab_ui();
    wxGetApp().plater()->sidebar().update_presets(type);
    return true;
}

// DELETE /api/v1/preset  body {"type","name"} -> remove a USER preset (file + list entry).
// Guards: never system/default, never the selected preset (select another first),
// never a base preset that other presets inherit from (same rule as the GUI).
Response Controller::handle_delete_preset(const std::string &body)
{
    nlohmann::json in = nlohmann::json::parse(body);
    if (!in.is_object() || !in.contains("type") || !in.contains("name")
        || !in["type"].is_string() || !in["name"].is_string())
        return { 400, {{"error", "missing_fields"}} };
    std::string type_s = in["type"].get<std::string>();
    std::string name   = in["name"].get<std::string>();
    Preset::Type type = type_s == "print"    ? Preset::TYPE_PRINT :
                        type_s == "filament" ? Preset::TYPE_FILAMENT :
                        type_s == "printer"  ? Preset::TYPE_PRINTER : Preset::TYPE_INVALID;
    if (type == Preset::TYPE_INVALID)
        return { 400, {{"error", "unknown_type"}} };
    nlohmann::json r = run_on_ui([type, type_s, name]() -> nlohmann::json {
        auto refuse = [&](nlohmann::json e) -> nlohmann::json {
            if (!e.contains("detail")) e["detail"] = name;
            mcp_refused_reply(mcp_fmt(MCP_T("Delete {type} preset"), {{"type", type_s}}), e, "DELETE /preset");
            if (e["detail"].is_string() && e["detail"].get<std::string>() == name) e.erase("detail");
            return e;
        };
        const std::string why = preset_delete_refusal(type_s, name);
        if (why == "preset_selected")
            return refuse({{"error", why}, {"detail", "select another preset first, then delete"}});
        if (why == "has_children")
            return refuse({{"error", why}, {"detail", "other presets inherit from this one"}});
        if (!why.empty()) return refuse({{"error", why}});
        mcp_before_change();
        if (!delete_user_preset(type_s, name))
            return refuse({{"error", "delete_failed"}});
        api_notify("Deleted " + type_s + " preset '" + name + "'");
        McpRow row = mcp_row("preset", MCP_T("Deleted preset"), name, std::string(),
                             std::string(type == Preset::TYPE_PRINT    ? MCP_T("Print preset") :
                                         type == Preset::TYPE_FILAMENT ? MCP_T("Filament preset") :
                                                                         MCP_T("Printer preset")));
        row.on_disk = true;
        row.target  = {{"type", "preset_deleted"}, {"preset_type", type_s}, {"name", name}};
        mcp_record(std::move(row));
        return {{"deleted", name}};
    }, /*timeout_s=*/120);
    if (r.contains("error")) {
        if (r["error"] == "unknown_preset") return { 404, r };
        if (r["error"] == "delete_failed")  return { 500, r };
        return { 409, r };
    }
    return { 200, r };
}

// M4a: GET /api/v1/gcode  -> raw G-code of the current plate's last successful slice
Response Controller::handle_get_gcode()
{
    nlohmann::json meta = run_on_ui([]() -> nlohmann::json {
        PartPlate *plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
        if (!plate->is_slice_result_valid())
            return {{"error", "not_sliced"}};
        GCodeProcessorResult *res = plate->get_slice_result();
        if (res == nullptr || res->filename.empty())
            return {{"error", "not_sliced"}};
        return {{"path", res->filename}};
    });
    if (meta.contains("error"))
        return { 409, meta };
    // Read off the GUI thread (only the state lookup above needed it).
    std::string path = meta["path"].get<std::string>();
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return { 500, {{"error", "gcode_file_missing"}} };
    std::ostringstream ss;
    ss << f.rdbuf();
    Response r;
    r.status = 200;
    r.raw_body = ss.str();
    r.raw_content_type = "text/plain";
    return r;
}


Response Controller::handle_plate_render(const std::string &target)
{
    // GET /api/v1/plate/render?view=editor|preview&angle=iso|top|front|left|right|rear|bottom&width=800&height=600
    auto qparam = [&target](const char *name) -> std::string {
        auto qpos = target.find('?');
        if (qpos == std::string::npos) return {};
        const std::string key = std::string(name) + "=";
        const std::string q   = target.substr(qpos + 1);
        size_t start = 0;
        while (start <= q.size()) {
            auto amp = q.find('&', start);
            std::string item = q.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
            if (item.size() >= key.size() && item.compare(0, key.size(), key) == 0)
                return url_decode(item.substr(key.size()));
            if (amp == std::string::npos) break;
            start = amp + 1;
        }
        return {};
    };

    std::string view = qparam("view");
    if (view.empty()) view = "editor";
    if (view != "editor" && view != "preview")
        return { 400, {{"error", "bad_param"}, {"param", "view"},
                       {"allowed", nlohmann::json::array({"editor", "preview"})}} };

    std::string angle = qparam("angle");
    if (angle.empty()) angle = "iso";
    static const std::map<std::string, Camera::ViewAngleType> angle_map = {
        {"iso",    Camera::ViewAngleType::Iso},   {"top",  Camera::ViewAngleType::Top},
        {"front",  Camera::ViewAngleType::Front}, {"left", Camera::ViewAngleType::Left},
        {"right",  Camera::ViewAngleType::Right}, {"rear", Camera::ViewAngleType::Rear},
        {"bottom", Camera::ViewAngleType::Bottom}};
    auto ait = angle_map.find(angle);
    if (ait == angle_map.end())
        return { 400, {{"error", "bad_param"}, {"param", "angle"},
                       {"allowed", nlohmann::json::array({"iso", "top", "front", "left", "right", "rear", "bottom"})}} };

    // frame=plate  -> zoom to the whole bed (where the part sits, footprint)
    // frame=object -> zoom to the model/toolpaths (detail)
    // default: plate for the editor view, object for the preview view.
    std::string frame = qparam("frame");
    if (!frame.empty() && frame != "plate" && frame != "object")
        return { 400, {{"error", "bad_param"}, {"param", "frame"},
                       {"allowed", nlohmann::json::array({"plate", "object"})}} };

    long w = 800, h = 600;
    try {
        const std::string ws = qparam("width"), hs = qparam("height");
        if (!ws.empty()) w = std::stol(ws);
        if (!hs.empty()) h = std::stol(hs);
    } catch (...) {
        return { 400, {{"error", "bad_param"}, {"param", "width/height"}} };
    }
    w = std::min(std::max(w, 64L), 2048L);
    h = std::min(std::max(h, 64L), 2048L);

    const Camera::ViewAngleType va           = ait->second;
    const bool                  preview      = (view == "preview");
    const bool                  frame_object = frame.empty() ? preview : (frame == "object");
    // Filled on the GUI thread; run_on_ui blocks until the lambda completes, and
    // the shared_ptr keeps the buffer alive even if the io side times out first.
    auto png_out = std::make_shared<std::string>();

    // 30s (not the 10s default): a large plate at 2048px can outrun the default.
    nlohmann::json j = run_on_ui([va, w, h, preview, frame_object, png_out]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        ThumbnailData data;
        if (preview) {
            PartPlate *plate = plater->get_partplate_list().get_curr_plate();
            if (!plate->is_slice_result_valid())
                return {{"error", "no_slice_result"}};
            GLCanvas3D *canvas = plater->get_preview_canvas3D();
            if (canvas == nullptr)
                return {{"error", "render_failed"}, {"reason", "no preview canvas"}};
            // The preview only auto-reloads while its panel is on screen
            // (Plater::priv::update: `if (is_preview_shown()) preview->reload_print()`),
            // so an API-driven slice leaves the GCodeViewer empty. Load it on demand.
            if (!canvas->render_gcode_thumbnail(data, (unsigned int)w, (unsigned int)h, va, frame_object)) {
                plater->reload_print();
                if (!canvas->render_gcode_thumbnail(data, (unsigned int)w, (unsigned int)h, va, frame_object))
                    return {{"error", "render_failed"}, {"reason", "gcode preview render unavailable"}};
            }
        } else {
            GLCanvas3D *canvas = plater->get_view3D_canvas3D();
            if (canvas == nullptr || !canvas->render_plate_thumbnail(data, (unsigned int)w, (unsigned int)h, va, frame_object))
                return {{"error", "render_failed"}, {"reason", "editor plate render failed"}};
        }
        size_t png_size = 0;
        void  *png      = tdefl_write_image_to_png_file_in_memory_ex(
            (const void *)data.pixels.data(), data.width, data.height, 4, &png_size, MZ_DEFAULT_LEVEL, 1);
        if (png == nullptr)
            return {{"error", "render_failed"}, {"reason", "png encode failed"}};
        png_out->assign((const char *)png, png_size);
        mz_free(png);
        return {{"bytes", png_size}};
    }, 30);

    if (j.contains("error")) {
        const std::string err = j["error"].get<std::string>();
        return { err == "no_slice_result" ? 409 : 500, j };
    }
    Response r;
    r.status           = 200;
    r.raw_body         = *png_out;
    r.raw_content_type = "image/png";
    return r;
}


Response Controller::handle_get_objects()
{
    nlohmann::json r = run_on_ui([]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        nlohmann::json objects = nlohmann::json::array();
        const Model &model = plater->model();
        for (size_t i = 0; i < model.objects.size(); ++i) {
            const ModelObject *mo = model.objects[i];
            auto sz = mo->bounding_box_exact().size();
            nlohmann::json o = {
                {"id", (uint64_t) mo->id().id},
                {"index", i},
                {"name", mo->name},
                {"size_mm", {sz.x(), sz.y(), sz.z()}},
                {"instances", (unsigned) mo->instances.size()},
                // plates: 0-based index of the plate holding instance 0, -1 when it
                // sits off every plate (parked beside the bed, or on a deleted plate).
                {"plate", plater->get_partplate_list().find_instance((int) i, 0)},
            };
            if (!mo->instances.empty()) {
                const ModelInstance *mi = mo->instances.front();
                auto off = mi->get_offset();
                auto rot = mi->get_rotation();
                auto scl = mi->get_scaling_factor();
                o["transform"] = {
                    {"offset",   {off.x(), off.y(), off.z()}},
                    {"rotation", {rot.x(), rot.y(), rot.z()}},
                    {"scale",    {scl.x(), scl.y(), scl.z()}},
                };
                // World-space bbox of instance 0. `offset` above is the MESH
                // ORIGIN, which only coincides with the bbox centre for a
                // centred, unrotated mesh - so plate contact is NOT derivable
                // from it. bbox_min[2] is the real answer: ~0 sits on the
                // plate, >0 floats, <0 sinks into it.
                const BoundingBoxf3 bb = mo->instance_bounding_box(*mi, false);
                if (bb.defined) {
                    o["bbox_min"] = {bb.min.x(), bb.min.y(), bb.min.z()};
                    o["bbox_max"] = {bb.max.x(), bb.max.y(), bb.max.z()};
                    o["on_plate"] = (std::abs(bb.min.z()) < 0.05);
                }
            }
            // M4c readback: per-object overrides + variable-layer-height state
            {
                const DynamicPrintConfig ocfg = mo->config.get();
                nlohmann::json cfgj = nlohmann::json::object();
                for (const std::string &k : ocfg.keys()) cfgj[k] = ocfg.opt_serialize(k);
                o["config"] = cfgj;
                o["custom_layer_profile"] = !mo->layer_height_profile.empty();
                nlohmann::json rngs = nlohmann::json::array();
                for (const auto &kv : mo->layer_config_ranges)
                    rngs.push_back({{"min_z", kv.first.first}, {"max_z", kv.first.second},
                                    {"layer_height", kv.second.has("layer_height") ? kv.second.opt_float("layer_height") : 0.0}});
                o["height_ranges"] = rngs;
            }
            objects.push_back(std::move(o));
        }
        return {{"objects", objects}, {"count", model.objects.size()}};
    });
    return { 200, r };
}


static int find_object_index(const Model &model, uint64_t id)
{
    for (size_t i = 0; i < model.objects.size(); ++i)
        if ((uint64_t) model.objects[i]->id().id == id)
            return (int) i;
    return -1;
}

Response Controller::handle_delete_object(uint64_t id)
{
    nlohmann::json r = run_on_ui([id]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        int idx = find_object_index(plater->model(), id);
        if (idx < 0) {
            api_notify("Object not found", true);
            nlohmann::json e = {{"error", "unknown_object"}};
            mcp_refused_reply(mcp_fmt(MCP_T("Remove object {n}"), {{"n", std::to_string(id)}}), e, "DELETE /objects/{id}");
            return e;
        }
        std::string oname = plater->model().objects[idx]->name;
        // MCP panel: keep a full copy for "Put back", with the plate it was on.
        const int plate = plater->get_partplate_list().find_instance(idx, 0);
        mcp_before_change();
        int stash = 0;
        try {
            stash = McpRecorder::get().stash_object(*plater->model().objects[idx], plate);
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(error) << "MCP panel: object not stashed: " << e.what();
        }
        Plater::TakeSnapshot snapshot(plater, "Delete Object: " + oname);
        plater->remove((size_t) idx);
        api_notify("Removed '" + oname + "'");
        McpRow row = mcp_row("model", MCP_T("Removed"), oname, std::string(), mcp_plate_text(plate));
        row.target = {{"type", "removed"}, {"object_id", id}, {"stash", stash},
                      {"stash_session", McpRecorder::get().stash_session()}, {"plate", plate}};
        if (stash > 0)
            mcp_undo(row, MCP_T("Put back"),
                     mcp_fmt(MCP_T("Put {name} back on {plate}"),
                             {{"name", oname},
                              {"plate", plate >= 0 ? mcp_plate_text(plate) : std::string(MCP_T("the plate"))}}));
        mcp_record(std::move(row));
        return {{"deleted", true}, {"id", id}, {"count", plater->model().objects.size()}};
    }, /*timeout_s=*/120);
    if (r.contains("error")) return { 404, r };
    return { 200, r };
}

Response Controller::handle_transform_object(uint64_t id, const std::string &body)
{
    nlohmann::json in = nlohmann::json::parse(body); // parse_error -> 400 in dispatch
    if (!in.is_object()) return { 400, {{"error", "body_must_be_object"}} };
    auto read_vec = [&in](const char *k, bool &present) -> Vec3d {
        present = in.contains(k) && in[k].is_array() && in[k].size() == 3;
        if (!present) return Vec3d(0, 0, 0);
        return Vec3d(in[k][0].get<double>(), in[k][1].get<double>(), in[k][2].get<double>());
    };
    bool has_t = false, has_r = false, has_s = false;
    Vec3d tr = read_vec("translate", has_t);
    Vec3d ro = read_vec("rotate", has_r);   // degrees
    Vec3d sc = read_vec("scale", has_s);     // absolute factor
    if (!has_t && !has_r && !has_s)
        return { 400, {{"error", "no_transform"},
                       {"detail", "provide translate (mm), rotate (deg), and/or scale (factor)"}} };

    nlohmann::json r = run_on_ui([=]() -> nlohmann::json {
        auto refuse = [&](nlohmann::json e) -> nlohmann::json {
            mcp_refused_reply(mcp_fmt(has_r ? MCP_T("Rotate object {n}") : (has_s ? MCP_T("Scale object {n}") : MCP_T("Move object {n}")),
                                      {{"n", std::to_string(id)}}),
                              e, "POST /objects/{id}/transform");
            return e;
        };
        Plater *plater = wxGetApp().plater();
        int idx = find_object_index(plater->model(), id);
        if (idx < 0) { api_notify("Object not found", true); return refuse({{"error", "unknown_object"}}); }
        ModelObject *mo = plater->model().objects[idx];
        if (mo->instances.empty()) return refuse({{"error", "no_instance"}});
        ModelInstance *mi = mo->instances.front();
        // MCP panel: the state before, for the row and its undo.
        const nlohmann::json m_before    = McpRecorder::instance_matrix(*mi);
        const int            plate_before = wxGetApp().plater()->get_partplate_list().find_instance(idx, 0);
        const Vec3d          off_before  = mi->get_offset();
        const Vec3d          rot_before  = mi->get_rotation();
        const Vec3d          scl_before  = mi->get_scaling_factor();
        mcp_before_change();
        Plater::TakeSnapshot snapshot(plater, "Transform Object");
        if (has_t) mi->set_offset(mi->get_offset() + tr);
        if (has_r) mi->set_rotation(mi->get_rotation() + ro * 0.017453292519943295); // deg->rad
        if (has_s) mi->set_scaling_factor(sc);
        mo->invalidate_bounding_box();
        // plates: re-check which plate the instance sits on, as a drag in the window
        // does (GLCanvas3D::do_move/do_rotate/do_scale). Without it an object moved
        // onto another plate by coordinates stayed in its old plate's list: that plate
        // kept trying to slice it and the new one ignored it. is_new=true: a plate in
        // spiral-vase mode applies its vase settings to the object without the drag's
        // question dialog, which nothing could answer from an API call.
        wxGetApp().plater()->get_partplate_list().notify_instance_update(idx, 0, true);
        plater->changed_object(idx);
        wxGetApp().obj_list()->update_plate_values_for_items();
        api_notify(std::string(has_t ? "Moved" : (has_r ? "Rotated" : "Resized")) + " '" + mo->name + "'");
        {
            // One row; with several parts in one request it is named after the rotation, then the scale.
            const Vec3d off_after = mi->get_offset();
            const Vec3d scl_after = mi->get_scaling_factor();
            auto deg = [](double rad) { return McpRecorder::num_text(std::round(rad * 57.29577951308232 * 10.0) / 10.0); };
            auto angles = [&deg](const Vec3d &r) {
                const char *axes[] = {"X", "Y", "Z"};
                std::string t;
                for (int a = 0; a < 3; ++a) {
                    if (std::abs(r[a]) < 1e-9) continue;
                    t += (t.empty() ? "" : ", ") + std::string(axes[a]) + " " + deg(r[a]) + k_deg;
                }
                return t;
            };
            auto scale_text = [](const Vec3d &f) {
                if (std::abs(f.x() - f.y()) < 1e-9 && std::abs(f.y() - f.z()) < 1e-9)
                    return std::string("x") + McpRecorder::num_text(f.x());
                return std::string("x") + McpRecorder::num_text(f.x()) + ", " + McpRecorder::num_text(f.y()) + ", " +
                       McpRecorder::num_text(f.z());
            };
            auto pos_text = [](const Vec3d &o, bool z) {
                return McpRecorder::num_text(std::round(o.x() * 10.0) / 10.0) + ", " +
                       McpRecorder::num_text(std::round(o.y() * 10.0) / 10.0) +
                       (z ? ", " + McpRecorder::num_text(std::round(o.z() * 10.0) / 10.0) : std::string());
            };
            std::vector<std::string> parts;
            if (has_r) {
                // "90° about Y" for a turn about one axis, else the turn per axis.
                int axes_used = 0, axis = 0;
                for (int a = 0; a < 3; ++a)
                    if (std::abs(ro[a]) > 1e-9) { ++axes_used; axis = a; }
                const char *names[] = {"X", "Y", "Z"};
                if (axes_used == 1)
                    parts.push_back(mcp_fmt(MCP_T("{angle}\xC2\xB0 about {axis}"),
                                            {{"angle", McpRecorder::num_text(ro[axis])}, {"axis", names[axis]}}));
                else if (axes_used == 0)
                    parts.push_back(std::string("0") + k_deg);
                else
                    parts.push_back(angles(ro * 0.017453292519943295));
            }
            if (has_s) parts.push_back(scale_text(scl_after));
            if (has_t)
                parts.push_back(mcp_fmt(MCP_T("to {position}"), {{"position", pos_text(off_after, std::abs(tr.z()) > 1e-9)}}));
            std::string value;
            for (const std::string &p : parts) value += (value.empty() ? "" : ", ") + p;
            std::string before;
            if (has_r) {
                const std::string a = angles(rot_before);
                before = a.empty() ? std::string(MCP_T("not rotated")) : a;
            } else if (has_s) {
                before = scale_text(scl_before);
            } else {
                before = pos_text(off_before, std::abs(tr.z()) > 1e-9);
            }
            const char *label = has_r ? MCP_T("Rotated") : (has_s ? MCP_T("Scaled") : MCP_T("Moved"));
            McpRow row = mcp_row("model", label, value, before, mo->name);
            const int plate_after = wxGetApp().plater()->get_partplate_list().find_instance(idx, 0);
            row.target = {{"type", "transform"}, {"object_id", id}, {"instance", 0},
                          {"instance_id", (uint64_t) mi->id().id}, {"before", m_before},
                          {"after", McpRecorder::instance_matrix(*mi)}, {"plate_before", plate_before},
                          {"plate_after", plate_after}};
            if (has_r)
                mcp_undo(row, MCP_T("Rotate back"), mcp_fmt(MCP_T("Rotate {name} back"), {{"name", mo->name}}));
            else if (has_s)
                mcp_undo(row, MCP_T("Scale back"), mcp_fmt(MCP_T("Scale {name} back"), {{"name", mo->name}}));
            else
                mcp_undo(row, MCP_T("Move back"),
                         mcp_fmt(MCP_T("Move {name} back where it was"), {{"name", mo->name}}));
            mcp_record(std::move(row));
        }
        auto off = mi->get_offset(); auto rot = mi->get_rotation(); auto scl = mi->get_scaling_factor();
        return {{"id", id}, {"transform", {
            {"offset",   {off.x(), off.y(), off.z()}},
            {"rotation", {rot.x(), rot.y(), rot.z()}},
            {"scale",    {scl.x(), scl.y(), scl.z()}}}}};
    }, /*timeout_s=*/120);
    if (r.contains("error")) return { 404, r };
    return { 200, r };
}

Response Controller::handle_duplicate_object(uint64_t id)
{
    nlohmann::json r = run_on_ui([id]() -> nlohmann::json {
        auto refuse = [id](nlohmann::json e) -> nlohmann::json {
            mcp_refused_reply(mcp_fmt(MCP_T("Duplicate object {n}"), {{"n", std::to_string(id)}}), e,
                              "POST /objects/{id}/duplicate");
            return e;
        };
        Plater *plater = wxGetApp().plater();
        Model &model = plater->model();
        int idx = find_object_index(model, id);
        if (idx < 0) { api_notify("Object not found", true); return refuse({{"error", "unknown_object"}}); }
        ModelObject *mo = model.objects[idx];
        if (mo->instances.empty()) return refuse({{"error", "no_instance"}});
        const ModelInstance *src = mo->instances.front();
        Vec3d off = src->get_offset() + Vec3d(10.0, 10.0, 0.0);
        const bool was_one = mo->instances.size() == 1;
        mcp_before_change();
        Plater::TakeSnapshot snapshot(plater, "Add Instance");
        mo->add_instance(off, src->get_scaling_factor(), src->get_rotation(), src->get_mirror());
        // sidebar: add the "Instances" child now, as Plater::increase_instances does (a lone object
        // gets both rows), instead of only at the next full list rebuild.
        wxGetApp().obj_list()->increase_object_instances(idx, was_one ? 2 : 1);
        // plates: give the new copy to the plate it landed on (see handle_transform_object).
        wxGetApp().plater()->get_partplate_list().notify_instance_update(idx, (int) mo->instances.size() - 1, true);
        plater->changed_object(idx);
        wxGetApp().obj_list()->update_plate_values_for_items();
        api_notify("Duplicated '" + mo->name + "'");
        {
            const size_t         n    = mo->instances.size();
            const ModelInstance *copy = mo->instances.back();
            McpRow row = mcp_row("model", MCP_T("Duplicated"),
                                 mcp_fmt(MCP_T("{name}, now {n} copies"), {{"name", mo->name}, {"n", std::to_string(n)}}));
            row.target = {{"type", "duplicate"}, {"object_id", id}, {"instance", n - 1},
                          {"instance_id", (uint64_t) copy->id().id}, {"after", McpRecorder::instance_matrix(*copy)},
                          {"plate", wxGetApp().plater()->get_partplate_list().find_instance(idx, (int) n - 1)}};
            mcp_undo(row, MCP_T("Remove copy"),
                     mcp_fmt(MCP_T("Remove the new copy of {name}"), {{"name", mo->name}}));
            mcp_record(std::move(row));
        }
        return {{"duplicated", true}, {"id", id}, {"instances", (unsigned) mo->instances.size()}};
    }, /*timeout_s=*/120);
    if (r.contains("error")) return { r["error"] == "unknown_object" ? 404 : 422, r };
    return { 200, r };
}

Response Controller::handle_put_object_config(uint64_t id, const std::string &body)
{
    nlohmann::json in = nlohmann::json::parse(body); // parse_error -> 400 in dispatch
    if (!in.is_object())
        return { 400, {{"error", "body_must_be_object"}} };
    nlohmann::json r = run_on_ui([id, in]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        int idx = find_object_index(plater->model(), id);
        if (idx < 0) {
            nlohmann::json e = {{"error", "unknown_object"}};
            mcp_refused_reply(mcp_fmt(MCP_T("Object settings {n}"), {{"n", std::to_string(id)}}), e,
                              "PUT /objects/{id}/config");
            return e;
        }
        ModelObject *mo = plater->model().objects[idx];
        DynamicPrintConfig cfg = mo->config.get(); // copy of existing per-object overrides
        nlohmann::json applied = nlohmann::json::array();
        nlohmann::json errors  = nlohmann::json::object();
        const bool allow_sensitive = sensitive_writes_allowed();
        // For the range check: the object's own value of a key, else the one it prints with now.
        const DynamicPrintConfig obj_before = cfg;
        const DynamicPrintConfig merged_now = wxGetApp().preset_bundle->full_config();
        nlohmann::json           out_of_range = nullptr;
        for (auto it = in.begin(); it != in.end(); ++it) {
            const std::string &key = it.key();
            if (print_config_def.get(key) == nullptr) { errors[key] = "unknown_key"; continue; }
            // Same policy as PUT /config; a per-object override of these is never a
            // slicing need, so there is no write-back exemption here.
            if (!allow_sensitive && is_sensitive_config_key(key)) {
                errors[key] = "blocked_by_remote_api_policy";
                continue;
            }
            try {
                cfg.set_deserialize_strict(key, json_value_to_config_string(it.value()));
                const ConfigOption *was = obj_before.has(key) ? obj_before.option(key) : merged_now.option(key);
                nlohmann::json      rd;
                std::string         rtext;
                if (!config_value_in_range(key, was, *cfg.option(key), rd, rtext)) {
                    errors[key] = "out_of_range: " + rtext;
                    if (out_of_range.is_null()) out_of_range = rd;
                    continue;
                }
                applied.push_back(key);
            } catch (const std::exception &e) { errors[key] = e.what(); }
        }
        if (!errors.empty()) { // atomic: apply nothing on any error
            api_notify(std::string("Couldn't set object settings on '") + mo->name + "'", true);
            for (auto it2 = errors.begin(); it2 != errors.end(); ++it2) {
                const std::string why = it2.value().is_string() ? it2.value().get<std::string>() : std::string("error");
                if (why == "blocked_by_remote_api_policy")
                    mcp_refused(mcp_setting_label(it2.key()), MCP_T("Not allowed"),
                                mcp_fmt(MCP_T("Needs Preferences \xE2\x80\xBA Remote API \xE2\x80\xBA Allow script, G-code and connection edits \xC2\xB7 {object}"),
                                        {{"object", mo->name}}),
                                "PUT /objects/{id}/config");
                else if (why.rfind("out_of_range: ", 0) == 0)
                    mcp_refused(mcp_setting_label(it2.key()), MCP_T("Out of range"), mo->name + ": " + why.substr(14),
                                "PUT /objects/{id}/config");
                else
                    mcp_refused(mcp_setting_label(it2.key()), mcp_reason(why, MCP_T("Invalid value")),
                                mo->name + ": " + it2.key() + ": " + why, "PUT /objects/{id}/config");
            }
            nlohmann::json reply = {{"applied", nlohmann::json::array()}, {"errors", errors}};
            if (!out_of_range.is_null())
                for (auto it3 = out_of_range.begin(); it3 != out_of_range.end(); ++it3) reply[it3.key()] = it3.value();
            return reply;
        }
        // MCP panel rows: per applied key, the old override (or the value the object used without one).
        std::vector<McpRow> mcp_rows;
        {
            const DynamicPrintConfig old_cfg = mo->config.get();
            const DynamicPrintConfig merged  = wxGetApp().preset_bundle->full_config();
            for (const auto &k : applied) {
                const std::string key        = k.get<std::string>();
                const bool        had        = old_cfg.has(key);
                const std::string before_raw = had ? old_cfg.opt_serialize(key)
                                                   : (merged.has(key) ? merged.opt_serialize(key) : std::string());
                const std::string after_raw  = cfg.opt_serialize(key);
                if (had && before_raw == after_raw) continue;
                const std::string label = mcp_setting_label(key);
                McpRow row = mcp_config_row(key, label, before_raw, after_raw,
                                            {{"type", "object_config"}, {"object_id", id}, {"key", key}, {"had_override", had}},
                                            had ? mcp_fmt(MCP_T("{object} only"), {{"object", mo->name}})
                                                : mcp_fmt(MCP_T("{object} only \xC2\xB7 from the print preset"), {{"object", mo->name}}));
                row.undo_tip = had ? mcp_fmt(MCP_T("Set {label} on {object} back to {before}"),
                                             {{"label", label}, {"object", mo->name}, {"before", row.before}})
                                   : mcp_fmt(MCP_T("Remove the {label} override from {object}"),
                                             {{"label", label}, {"object", mo->name}});
                mcp_rows.push_back(std::move(row));
            }
        }
        if (!mcp_rows.empty()) mcp_before_change();
        Plater::TakeSnapshot snapshot(plater, "Change Object Settings");
        mo->config.assign_config(std::move(cfg));
        plater->changed_object(idx);
        // changed_object() reschedules the background slice and repaints the 3D scene, but
        // it does not touch the object list's filament column. That column is written by
        // ObjectList itself (set_extruder_for_selected_items / update_filament_values_for_items
        // both do SetExtruder + Refresh), so an extruder set through this endpoint sliced
        // with the new filament while the list still showed the old one - the API and the
        // window disagreed with no way to tell which was right. Mirror what the GUI does.
        if (in.contains("extruder") && mo->config.has("extruder")) {
            ObjectList *obj_list = wxGetApp().obj_list();
            ObjectDataViewModel *om = obj_list == nullptr ? nullptr : obj_list->GetModel();
            if (om != nullptr) {
                wxDataViewItem item = om->GetItemById(idx);
                if (item.IsOk()) {
                    // 0 means "use the object/printer default" and the list spells that
                    // out rather than showing a slot number (ObjectDataViewModel.cpp).
                    const int ext = mo->config.extruder();
                    om->SetExtruder(ext == 0 ? _L("default") : wxString::Format("%d", ext), item);
                    obj_list->Refresh(); // BBS: repaint the filament column
                }
            }
        }
        if (!applied.empty())
            api_notify("Updated " + std::to_string(applied.size()) + " setting(s) on '" + mo->name + "'");
        for (McpRow &row : mcp_rows) mcp_record(std::move(row));
        return {{"applied", applied}, {"errors", errors}, {"object", mo->name}};
    }, /*timeout_s=*/120);
    if (r.contains("error") && r["error"] != "out_of_range") return { 404, r };
    return { r["errors"].empty() ? 200 : 422, r };
}

// MCP panel: arrange and orient run as jobs. The instances are noted before the job starts, and a no-op job
// queued right behind it records the row from its finalize (on the GUI thread, after the job's own finalize:
// the worker runs and finalizes its queue in order), when the new positions are known.
static void mcp_queue_instances_row(Plater *plater, const char *op, std::vector<McpInstanceState> before, int plates_before)
{
    const std::string opname = op;
    // The job moves objects after this request's baseline. It keeps the journal's token: when OrcaSlicer drops the
    // queued job unfinished (cancel_all on a delete or a project change), the token dies with it.
    std::shared_ptr<int> token  = mcp_journal_api_job_queued();
    const bool trial = s_ui_trial;
    const bool queued = queue_job(plater->get_ui_job_worker(), [](Job::Ctl &) {},
                                  [opname, before, plates_before, token, trial](bool, std::exception_ptr &) {
                                      (void) token;
                                      mcp_record_instances(opname.c_str(), before, plates_before, trial);
                                  });
    if (!queued) mcp_record_instances(op, before, plates_before, trial); // records what has moved so far
}

Response Controller::handle_arrange()
{
    nlohmann::json r = run_on_ui([]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        if (plater->model().objects.empty()) {
            nlohmann::json e = {{"error", "empty"}};
            mcp_refused_reply(MCP_T("Arrange"), e, "POST /arrange");
            return e;
        }
        if (!plater->get_ui_job_worker().is_idle()) {
            mcp_refused_reply(MCP_T("Arrange"), {{"error", "job_running"}}, "POST /arrange");
            return {{"busy", true}};
        }
        std::vector<McpInstanceState> before = mcp_instance_states(plater);
        const int plates_before = plater->get_partplate_list().get_plate_count();
        mcp_before_change();
        plater->arrange();
        mcp_queue_instances_row(plater, "arrange", std::move(before), plates_before);
        return {{"started", true}};
    }, /*timeout_s=*/120);
    if (r.contains("error")) return { 422, r };
    if (r.value("busy", false)) return { 409, {{"error", "job_running"}} };
    return { 202, r };
}

Response Controller::handle_orient()
{
    nlohmann::json r = run_on_ui([]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        if (plater->model().objects.empty()) {
            nlohmann::json e = {{"error", "empty"}};
            mcp_refused_reply(MCP_T("Auto-orient"), e, "POST /orient");
            return e;
        }
        if (!plater->get_ui_job_worker().is_idle()) {
            mcp_refused_reply(MCP_T("Auto-orient"), {{"error", "job_running"}}, "POST /orient");
            return {{"busy", true}};
        }
        std::vector<McpInstanceState> before = mcp_instance_states(plater);
        const int plates_before = plater->get_partplate_list().get_plate_count();
        mcp_before_change();
        plater->orient();
        mcp_queue_instances_row(plater, "orient", std::move(before), plates_before);
        return {{"started", true}};
    }, /*timeout_s=*/120);
    if (r.contains("error")) return { 422, r };
    if (r.value("busy", false)) return { 409, {{"error", "job_running"}} };
    return { 202, r };
}

Response Controller::handle_jobs_status()
{
    nlohmann::json r = run_on_ui([]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        return {{"idle", plater->get_ui_job_worker().is_idle()}};
    });
    return { 200, r };
}

// --- plates ------------------------------------------------------------------
// GET /plates, POST /plates, POST /plates/{i}/select, DELETE /plates/{i} and
// POST /objects/{id}/plate. Every mutation goes through the same Plater /
// PartPlateList calls the toolbar uses (Plater::priv::on_action_add_plate,
// Plater::select_plate, Plater::delete_plate), so undo snapshots, the sidebar
// plate list, the background process and the preview follow the change exactly
// as they do for a click.

// One plate as JSON. `objects` is read back through PartPlate::contain_instance,
// i.e. from the plate's own bookkeeping, not from positions.
static nlohmann::json plate_json(PartPlateList &plates, int index, const Model &model)
{
    PartPlate *p = plates.get_plate(index);
    nlohmann::json objs = nlohmann::json::array();
    for (size_t oi = 0; oi < model.objects.size(); ++oi) {
        const ModelObject *mo = model.objects[oi];
        for (size_t ii = 0; ii < mo->instances.size(); ++ii)
            if (p->contain_instance((int) oi, (int) ii))
                objs.push_back({{"id", (uint64_t) mo->id().id}, {"instance", ii}, {"name", mo->name}});
    }
    const BoundingBoxf3 bb  = p->get_build_volume();
    const Vec3d         org = p->get_origin();
    return {
        {"index", index},
        {"name", p->get_plate_name()},
        {"current", index == plates.get_curr_plate_index()},
        {"locked", p->is_locked()},
        {"empty", p->empty()},
        {"slice_result_valid", p->is_slice_result_valid()},
        {"origin", {org.x(), org.y(), org.z()}},
        {"bbox_min", {bb.min.x(), bb.min.y(), bb.min.z()}},
        {"bbox_max", {bb.max.x(), bb.max.y(), bb.max.z()}},
        {"objects", objs},
    };
}

Response Controller::handle_get_plates()
{
    nlohmann::json r = run_on_ui([]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        PartPlateList &plates = plater->get_partplate_list();
        nlohmann::json arr = nlohmann::json::array();
        for (int i = 0; i < plates.get_plate_count(); ++i)
            arr.push_back(plate_json(plates, i, plater->model()));
        return {{"plates", arr},
                {"count", plates.get_plate_count()},
                {"current", plates.get_curr_plate_index()},
                {"max", (int) PartPlateList::MAX_PLATES_COUNT},
                {"can_add", plater->can_add_plate()},
                {"can_delete", plater->can_delete_plate()}};
    });
    return { 200, r };
}

// POST /plates  body (optional) {"select": true}
Response Controller::handle_create_plate(const std::string &body)
{
    bool select = true;
    if (!body.empty()) {
        nlohmann::json in = nlohmann::json::parse(body); // parse_error -> 400 in dispatch
        if (!in.is_object()) return { 400, {{"error", "body_must_be_object"}} };
        if (in.contains("select")) {
            if (!in["select"].is_boolean()) return { 400, {{"error", "select_must_be_boolean"}} };
            select = in["select"].get<bool>();
        }
    }
    nlohmann::json r = run_on_ui([this, select]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        PartPlateList &plates = plater->get_partplate_list();
        auto refuse = [](nlohmann::json e) -> nlohmann::json {
            mcp_refused_reply(MCP_T("Add plate"), e, "POST /plates");
            return e;
        };
        if (!plater->can_add_plate()) {
            if (plater->is_background_process_slicing()) return refuse({{"error", "slicing"}});
            return refuse({{"error", "max_plates"}, {"max", (int) PartPlateList::MAX_PLATES_COUNT}});
        }
        mcp_before_change();
        // Same steps as the toolbar's "Add plate" (Plater::priv::on_action_add_plate).
        plater->take_snapshot("add partplate");
        // 2.5.0: when the new plate adds a column to the plate grid, create_plate
        // rebuilds plate membership (reload_all_objects), which drops every plate's
        // slice result, as the toolbar button does. Follow that in /slice/status.
        const bool was_valid = plates.get_curr_plate()->is_slice_result_valid();
        const int idx = plates.create_plate();
        if (idx < 0) return refuse({{"error", "max_plates"}, {"max", (int) PartPlateList::MAX_PLATES_COUNT}});
        if (select) plater->select_plate(idx, false);
        plater->update();
        plater->get_camera().requires_zoom_to_plate = REQUIRES_ZOOM_TO_ALL_PLATE;
        if (select || plates.get_curr_plate()->is_slice_result_valid() != was_valid)
            sync_slice_state_to_current_plate("slice.plate_changed");
        api_notify("Added plate " + std::to_string(idx + 1));
        {
            McpRow row = mcp_row("plate", MCP_T("Added plate"), mcp_fmt(MCP_T("Plate {n}"), {{"n", std::to_string(idx + 1)}}));
            // The origin tells the panel's undo it is still this plate (the grid moves plates when others go).
            const Vec3d org = plates.get_plate(idx)->get_origin();
            row.target = {{"type", "plate_added"}, {"index", idx}, {"origin", {org.x(), org.y(), org.z()}}};
            mcp_undo(row, MCP_T("Delete plate"),
                     mcp_fmt(MCP_T("Delete Plate {n} (only while it is empty)"), {{"n", std::to_string(idx + 1)}}));
            // The panel's undo deletes this very plate, found by its ObjectID (indexes and origins shift when
            // plates before it are deleted).
            if (!s_ui_trial) { // M4: no row for a trial's plate
                try {
                    const int rid = McpRecorder::get().record(std::move(row));
                    McpRecorder::get().note_created_plate(rid, (uint64_t) plates.get_plate(idx)->id().id);
                } catch (const std::exception &e) {
                    BOOST_LOG_TRIVIAL(error) << "MCP panel: row not recorded: " << e.what();
                }
            }
        }
        return {{"created", true}, {"index", idx},
                {"count", plates.get_plate_count()},
                {"current", plates.get_curr_plate_index()},
                {"plate", plate_json(plates, idx, plater->model())}};
    }, /*timeout_s=*/120);
    if (r.contains("error")) return { r["error"] == "slicing" ? 409 : 422, r };
    return { 200, r };
}

// POST /plates/{i}/select - makes plate i the one slice, gcode, plate/render and
// jobs/status act on, exactly like clicking it.
Response Controller::handle_select_plate(int index)
{
    nlohmann::json r = run_on_ui([this, index]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        PartPlateList &plates = plater->get_partplate_list();
        if (index < 0 || index >= plates.get_plate_count())
            return {{"error", "unknown_plate"}, {"count", plates.get_plate_count()}};
        if (plater->is_background_process_slicing()) return {{"error", "slicing"}};
        if (plates.get_curr_plate_index() != index) {
            if (plater->select_plate(index, false) != 0) return {{"error", "select_failed"}};
            api_notify("Selected plate " + std::to_string(index + 1));
        }
        sync_slice_state_to_current_plate("slice.plate_changed");
        return {{"selected", true},
                {"current", plates.get_curr_plate_index()},
                {"plate", plate_json(plates, index, plater->model())}};
    });
    if (r.contains("error")) {
        if (r["error"] == "unknown_plate") return { 404, r };
        if (r["error"] == "slicing")       return { 409, r };
        return { 422, r };
    }
    return { 200, r };
}

// DELETE /plates/{i} - the objects it held are not deleted: Plater::delete_plate
// parks them beside the bed, as the toolbar does. The count is reported back.
Response Controller::handle_delete_plate(int index)
{
    nlohmann::json r = run_on_ui([this, index]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        PartPlateList &plates = plater->get_partplate_list();
        auto refuse = [index](nlohmann::json e) -> nlohmann::json {
            mcp_refused_reply(mcp_fmt(MCP_T("Delete plate {n}"), {{"n", std::to_string(index + 1)}}), e, "DELETE /plates/{i}");
            return e;
        };
        if (index < 0 || index >= plates.get_plate_count())
            return refuse({{"error", "unknown_plate"}, {"count", plates.get_plate_count()}});
        if (!plater->can_delete_plate()) return refuse({{"error", "last_plate"}});
        if (plater->is_background_process_slicing()) return refuse({{"error", "slicing"}});
        int held = 0;
        {
            PartPlate *p = plates.get_plate(index);
            const Model &model = plater->model();
            for (size_t oi = 0; oi < model.objects.size(); ++oi)
                for (size_t ii = 0; ii < model.objects[oi]->instances.size(); ++ii)
                    if (p->contain_instance((int) oi, (int) ii)) ++held;
        }
        mcp_before_change();
        if (plater->delete_plate(index) != 0) return refuse({{"error", "delete_failed"}});
        sync_slice_state_to_current_plate("slice.plate_changed");
        {
            McpRow row = mcp_row("plate", MCP_T("Deleted plate"), mcp_fmt(MCP_T("Plate {n}"), {{"n", std::to_string(index + 1)}}),
                                 std::string(),
                                 held > 0 ? std::string(MCP_T("Its objects moved beside the bed")) : std::string());
            row.target = {{"type", "plate_deleted"}, {"index", index}, {"objects_moved_off", held}};
            mcp_record(std::move(row));
        }
        api_notify("Deleted plate " + std::to_string(index + 1) +
                   (held > 0 ? " (" + std::to_string(held) + " object(s) moved off-plate)" : std::string()));
        return {{"deleted", true}, {"index", index},
                {"count", plates.get_plate_count()},
                {"current", plates.get_curr_plate_index()},
                {"objects_moved_off_plate", held}};
    }, /*timeout_s=*/120);
    if (r.contains("error")) {
        if (r["error"] == "unknown_plate") return { 404, r };
        if (r["error"] == "slicing")       return { 409, r };
        return { 422, r };
    }
    return { 200, r };
}

// POST /objects/{id}/plate  body {"plate": <index>, "instance": 0}
// Moves one instance onto a plate. Coming from another plate it keeps its
// position relative to that plate (front-left of plate 1 -> front-left of
// plate 2); coming from off-plate it lands at the target's centre, which is
// what PartPlateList::add_to_plate does on its own.
Response Controller::handle_put_object_plate(uint64_t id, const std::string &body)
{
    nlohmann::json in = nlohmann::json::parse(body); // parse_error -> 400 in dispatch
    if (!in.is_object() || !in.contains("plate") || !in["plate"].is_number_integer())
        return { 400, {{"error", "missing_plate"},
                       {"detail", "body {\"plate\": <0-based index from GET /plates>, \"instance\": 0}"}} };
    const int plate_idx = in["plate"].get<int>();
    if (in.contains("instance") && !in["instance"].is_number_integer())
        return { 400, {{"error", "instance_must_be_integer"}} };
    const int inst_idx = in.value("instance", 0);
    if (plate_idx < 0 || inst_idx < 0) return { 400, {{"error", "negative_index"}} };

    nlohmann::json r = run_on_ui([this, id, plate_idx, inst_idx]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        PartPlateList &plates = plater->get_partplate_list();
        auto refuse = [id, plate_idx](nlohmann::json e) -> nlohmann::json {
            mcp_refused_reply(mcp_fmt(MCP_T("Move object {n} to Plate {index}"),
                                      {{"n", std::to_string(id)}, {"index", std::to_string(plate_idx + 1)}}),
                              e, "POST /objects/{id}/plate");
            return e;
        };
        const int oi = find_object_index(plater->model(), id);
        if (oi < 0) { api_notify("Object not found", true); return refuse({{"error", "unknown_object"}}); }
        ModelObject *mo = plater->model().objects[oi];
        if (inst_idx >= (int) mo->instances.size())
            return refuse({{"error", "unknown_instance"}, {"instances", (unsigned) mo->instances.size()}});
        PartPlate *target = plates.get_plate(plate_idx);
        if (target == nullptr) return refuse({{"error", "unknown_plate"}, {"count", plates.get_plate_count()}});
        if (target->is_locked()) return refuse({{"error", "plate_locked"}, {"plate", plate_idx}});
        if (plater->is_background_process_slicing()) return refuse({{"error", "slicing"}});

        const int from = plates.find_instance(oi, inst_idx);   // -1: off every plate
        PartPlate *source = from >= 0 ? plates.get_plate(from) : nullptr;
        // The window does not let an object be dragged off a locked plate either.
        if (source != nullptr && from != plate_idx && source->is_locked())
            return refuse({{"error", "plate_locked"}, {"plate", from}});
        ModelInstance *mi = mo->instances[inst_idx];
        if (from == plate_idx) {
            auto off = mi->get_offset();
            return {{"moved", false}, {"already_on_plate", true}, {"id", id}, {"instance", inst_idx},
                    {"plate", plate_idx}, {"offset", {off.x(), off.y(), off.z()}}};
        }
        const nlohmann::json m_before = McpRecorder::instance_matrix(*mi);
        mcp_before_change();
        plater->take_snapshot("move to plate");
        // Keep the position relative to the old plate only when the object really sits
        // on it (a plate's list can lag behind its geometry, e.g. after a GUI edit).
        Vec3d keep = mi->get_offset();
        bool relative = false;
        if (source != nullptr) {
            const BoundingBoxf3 sb = source->get_build_volume();
            relative = keep.x() >= sb.min.x() && keep.x() <= sb.max.x() &&
                       keep.y() >= sb.min.y() && keep.y() <= sb.max.y();
        }
        if (relative) keep -= source->get_origin();
        if (plates.add_to_plate(oi, inst_idx, plate_idx) != 0) return refuse({{"error", "move_failed"}});
        if (relative) {
            mi->set_offset(target->get_origin() + keep);
            mo->invalidate_bounding_box();
        }
        // add_to_plate only drops the instance from the old plate's list. Dragging an
        // object away in the window also marks that plate's slice stale and its
        // thumbnails dirty (PartPlateList::notify_instance_update); without this,
        // GET /gcode on the old plate would still return G-code that prints the
        // object that just left it.
        if (source != nullptr) {
            source->update_slice_result_valid_state(false);
            source->thumbnail_data.reset();
            source->no_light_thumbnail_data.reset();
            source->top_thumbnail_data.reset();
            source->pick_thumbnail_data.reset();
        }
        // Re-check inside/outside on the new plate. is_new=true for the same reason as
        // in handle_transform_object: no spiral-vase question dialog from an API call.
        plates.notify_instance_update(oi, inst_idx, true);
        if (plates.find_instance(oi, inst_idx) != plate_idx) {
            // The relative position put it outside the target (it straddled the old
            // plate's edge): centre it on the target instead.
            Vec3d c = target->get_center_origin();
            c.z() = mi->get_offset().z();
            mi->set_offset(c);
            mo->invalidate_bounding_box();
            plates.notify_instance_update(oi, inst_idx, true);
        }
        plater->changed_object(oi);
        // The sidebar object list groups objects by plate; a drag refreshes it the same way.
        wxGetApp().obj_list()->update_plate_values_for_items();
        sync_slice_state_to_current_plate("slice.plate_changed");
        api_notify("Moved '" + mo->name + "' to plate " + std::to_string(plate_idx + 1));
        {
            const int now = plates.find_instance(oi, inst_idx);
            McpRow row = mcp_row("plate", MCP_T("Moved to plate"),
                                 mcp_fmt(MCP_T("{name} \xE2\x86\x92 {plate}"), {{"name", mo->name}, {"plate", mcp_plate_text(now)}}),
                                 mcp_plate_text(from));
            row.target = {{"type", "instance_plate"}, {"object_id", id}, {"instance", inst_idx},
                          {"instance_id", (uint64_t) mi->id().id}, {"plate_before", from}, {"plate_after", now},
                          {"before", m_before}, {"after", McpRecorder::instance_matrix(*mi)}};
            mcp_undo(row, MCP_T("Move back"),
                     mcp_fmt(MCP_T("Move {name} back to {plate}"), {{"name", mo->name}, {"plate", mcp_plate_text(from)}}));
            mcp_record(std::move(row));
        }
        auto off = mi->get_offset();
        return {{"moved", true}, {"id", id}, {"instance", inst_idx},
                {"plate", plates.find_instance(oi, inst_idx)}, {"from_plate", from},
                {"offset", {off.x(), off.y(), off.z()}}};
    }, /*timeout_s=*/120);
    if (r.contains("error")) {
        const std::string e = r["error"].get<std::string>();
        if (e == "unknown_object" || e == "unknown_plate" || e == "unknown_instance") return { 404, r };
        if (e == "slicing") return { 409, r };
        return { 422, r };
    }
    return { 200, r };
}

// --- project files ------------------------------------------------------------
// GET /project, POST /project/save, POST /project/open, POST /project/new,
// GET /project/recent, DELETE /project/recent, GET /project/browse,
// POST /project/move, DELETE /project/file.
//
// File > Save / Open / New run through modal prompts in the GUI (save the
// changes first? keep the modified presets? overwrite the file?). A remote
// caller cannot see or dismiss those, so every prompt is answered from the
// request body and the GUI's remembered-choice settings are used to keep the
// dialogs from opening. The work itself goes through the same Plater calls the
// menu uses, so the title, dirty flag, undo stack, backup and the File > Recent
// list follow along. The file routes (recent, browse, move, delete) only touch
// .3mf files, and never the file of the project that is open.

static bool path_is_3mf(const std::string &path)
{
    return path.size() > 4 && boost::algorithm::iends_with(path, ".3mf");
}

static std::string project_path_utf8(Plater *plater)
{
    return plater->get_project_filename(".3mf").ToUTF8().data();
}

// The plate-level "there is something to save" answer the GUI gives in its own
// prompts: dirty_state (plater edits + modified presets) or an undo-stack change
// since the last save.
static bool project_has_unsaved_changes(Plater *plater)
{
    return plater->is_project_dirty() || !plater->up_to_date(false, false);
}

static nlohmann::json project_json(Plater *plater)
{
    const std::string path = project_path_utf8(plater);
    return {
        {"name", std::string(plater->get_project_name().ToUTF8().data())},
        {"path", path},
        {"saved", !path.empty()},
        {"dirty", project_has_unsaved_changes(plater)},
        {"plates", plater->get_partplate_list().get_plate_count()},
        {"objects", (unsigned) plater->model().objects.size()},
    };
}

// Preset groups with edits that are not saved to a preset. Named the way
// PUT /preset's discarded_changes names them.
static nlohmann::json dirty_preset_groups()
{
    auto *bundle = wxGetApp().preset_bundle;
    nlohmann::json groups = nlohmann::json::array();
    if (bundle->prints.current_is_dirty())        groups.push_back("print");
    if (bundle->filaments.current_is_dirty())     groups.push_back("filament");
    if (bundle->sla_materials.current_is_dirty()) groups.push_back("sla_material");
    if (bundle->printers.current_is_dirty())      groups.push_back("printer");
    return groups;
}

// Same as PUT /preset: drop the un-applied edits of every collection, so no
// UnsavedChangesDialog can open on the path that follows. Returns the groups.
static nlohmann::json discard_preset_edits()
{
    auto *bundle = wxGetApp().preset_bundle;
    nlohmann::json discarded = nlohmann::json::array();
    auto discard_if_dirty = [&discarded](PresetCollection &c, const char *group) {
        if (c.current_is_dirty()) {
            c.discard_current_changes();
            discarded.push_back(group);
        }
    };
    discard_if_dirty(bundle->prints, "print");
    discard_if_dirty(bundle->filaments, "filament");
    discard_if_dirty(bundle->sla_materials, "sla_material");
    discard_if_dirty(bundle->printers, "printer");
    return discarded;
}

// Plater::save_project without its file dialog and failure MessageDialog: the
// same export strategy, then the same bookkeeping (backup, project name, undo
// "saved" mark, dirty flags, title, recent list). GUI thread only. Returns an
// error token or "".
static std::string save_project_to(Plater *plater, const std::string &path_utf8, uintmax_t &bytes)
{
    const wxString wpath = from_u8(path_utf8);
    SaveStrategy strategy = SaveStrategy::SplitModel | SaveStrategy::ShareMesh;
    if (wxGetApp().app_config->get_bool("export_sources_full_pathnames"))
        strategy = strategy | SaveStrategy::FullPathSources;
    if (plater->export_3mf(into_path(wpath), strategy) < 0)
        return "save_failed";
    Slic3r::remove_backup(plater->model(), false);
    plater->set_project_filename(wpath);
    plater->up_to_date(true, false);
    plater->up_to_date(true, true);
    wxGetApp().update_saved_preset_from_current_preset();
    plater->reset_project_dirty_after_save();
    plater->update_title_dirty_status();
    boost::system::error_code ec;
    bytes = boost::filesystem::file_size(into_path(wpath), ec);
    if (ec) bytes = 0;
    return {};
}

// The "save changes to the current project?" prompt, answered from the request:
// fail (refuse while there are changes), save (to the project's own path), or
// discard. Returns an error token or ""; `out` records what happened.
static std::string answer_unsaved_prompt(Plater *plater, const std::string &if_unsaved, nlohmann::json &out)
{
    const bool dirty = project_has_unsaved_changes(plater);
    out["had_unsaved_changes"] = dirty;
    if (!dirty) return {};
    if (if_unsaved == "fail") return "unsaved_changes";
    if (if_unsaved == "save") {
        const std::string current = project_path_utf8(plater);
        if (current.empty()) return "no_project_path";
        uintmax_t bytes = 0;
        const std::string err = save_project_to(plater, current, bytes);
        if (!err.empty()) return err;
        out["saved"] = current;
        out["saved_bytes"] = bytes;
    }
    return {}; // "discard"
}

// Sets an app-config key for the length of a scope and puts it back (or removes
// it again when it was absent) however the scope is left: the keys these routes
// touch silence GUI prompts, and a throw out of load_project must not leave the
// user's "ask before..." settings switched off.
class AppConfigOverride
{
public:
    AppConfigOverride(const std::string &key, const std::string &value) : m_key(key)
    {
        AppConfig *ac = wxGetApp().app_config;
        m_had  = ac->has(key);
        m_prev = m_had ? ac->get(key) : std::string();
        ac->set(key, value);
    }
    ~AppConfigOverride()
    {
        AppConfig *ac = wxGetApp().app_config;
        if (m_had) ac->set(m_key, m_prev);
        else       ac->erase("app", m_key);
    }
private:
    std::string m_key, m_prev;
    bool        m_had { false };
};

static bool parse_if_unsaved(const nlohmann::json &in, std::string &if_unsaved)
{
    if (!in.contains("if_unsaved") || in["if_unsaved"].is_null()) return true;
    if (!in["if_unsaved"].is_string()) return false;
    if_unsaved = in["if_unsaved"].get<std::string>();
    return if_unsaved == "save" || if_unsaved == "discard" || if_unsaved == "fail";
}

// The script, G-code and connection settings (is_sensitive_config_key) a project
// file would put in place when it is opened. A key counts when the project's
// value differs from the one in the local preset the project names, or when that
// preset is not on this computer (the project brings its own copy). OrcaSlicer's
// own File > Open asks about modified G-code (PresetBundle::validate_presets);
// POST /project/open silences that prompt, so with the policy off it runs this
// check instead and opens nothing when a key turns up. `check_print_printer`
// false and `first_filament` skip what presets: keep_current puts back anyway.
// GUI thread only. Throws when the file cannot be read.
static std::set<std::string> project_sensitive_changes(const std::string &path_utf8, bool check_print_printer,
                                                       size_t first_filament)
{
    DynamicPrintConfig        cfg;
    ConfigSubstitutionContext subs{ForwardCompatibilitySubstitutionRule::Enable};
    PlateDataPtrs             plate_data;
    std::vector<Preset *>     project_presets;
    En3mfType                 file_type = En3mfType::From_BBS;
    Semver                    file_version;
    struct Cleanup
    {
        PlateDataPtrs         &plates;
        std::vector<Preset *> &presets;
        ~Cleanup()
        {
            release_PlateData_list(plates);
            for (Preset *p : presets) delete p;
            presets.clear();
        }
    } cleanup{plate_data, project_presets};
    // Config only: no meshes are read.
    Model::read_from_archive(into_path(from_u8(path_utf8)).string(), &cfg, &subs, file_type, LoadStrategy::LoadConfig,
                             &plate_data, &project_presets, &file_version);

    auto *bundle = wxGetApp().preset_bundle;
    auto  owned_by = [](const std::vector<std::string> &opts, const std::string &key) {
        return std::find(opts.begin(), opts.end(), key) != opts.end();
    };
    // One value of an option: element `idx` of a vector option, the whole value otherwise.
    auto value_at = [](const ConfigOption *opt, size_t idx, std::string &out) {
        if (opt == nullptr) return false;
        if (const auto *vec = dynamic_cast<const ConfigOptionVectorBase *>(opt)) {
            const std::vector<std::string> v = vec->vserialize();
            if (v.empty()) return false;
            out = v[std::min(idx, v.size() - 1)];
            return true;
        }
        out = opt->serialize();
        return true;
    };
    // The project's value against the named local preset's; a missing preset
    // counts any non-empty value.
    auto differs = [&](const Preset *local, const std::string &key, size_t project_idx) {
        std::string theirs, ours;
        if (!value_at(cfg.option(key), project_idx, theirs)) return false;
        if (local == nullptr) return !theirs.empty();
        if (!value_at(local->config.option(key), 0, ours)) return !theirs.empty();
        return theirs != ours;
    };

    const Preset *printer = nullptr, *print = nullptr;
    if (check_print_printer) {
        if (cfg.has("printer_settings_id"))
            printer = bundle->printers.find_preset(cfg.opt_string("printer_settings_id"), false, true);
        if (cfg.has("print_settings_id"))
            print = bundle->prints.find_preset(cfg.opt_string("print_settings_id"), false, true);
    }
    std::vector<std::string> filament_ids;
    if (const auto *ids = cfg.option<ConfigOptionStrings>("filament_settings_id"))
        filament_ids = ids->values;

    std::set<std::string> keys;
    for (const std::string &key : cfg.keys()) {
        if (!is_sensitive_config_key(key)) continue;
        if (owned_by(Preset::filament_options(), key)) {
            for (size_t i = first_filament; i < filament_ids.size(); ++i) {
                if (filament_ids[i].empty()) continue;
                const Preset *local = bundle->filaments.find_preset(filament_ids[i], false, true);
                if (differs(local, key, i)) { keys.insert(key); break; }
            }
        } else if (!check_print_printer) {
            continue;
        } else if (owned_by(Preset::printer_options(), key)) {
            if (differs(printer, key, 0)) keys.insert(key);
        } else if (owned_by(Preset::print_options(), key)) {
            if (differs(print, key, 0)) keys.insert(key);
        }
    }
    return keys;
}

static Response project_error_response(const nlohmann::json &r)
{
    const std::string e = r["error"].get<std::string>();
    if (e == "protected_path")
        return { 403, r };
    if (e == "file_not_found" || e == "directory_not_found" || e == "not_in_recent" || e == "unknown_recent")
        return { 404, r };
    if (e == "slicing" || e == "exists" || e == "busy" || e == "project_is_open")
        return { 409, r };
    if (e == "same_path" || e == "missing_dir")
        return { 400, r };
    return { 422, r };
}

// --- project files: paths, listings and the recent list -------------------------

// A UTF-8 path string (what the app config and the API carry) as a filesystem path.
static boost::filesystem::path path_from_utf8(const std::string &s)
{
    return into_path(from_u8(s));
}

static std::string path_to_utf8(const boost::filesystem::path &p)
{
    return into_u8(from_path(p));
}

static std::string iso_utc(std::time_t t)
{
    std::tm tm {};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// Whether `p` names the file of the project that is open. Compared through the
// filesystem when both exist, so a path spelled differently still matches.
static bool is_open_project_file(Plater *plater, const boost::filesystem::path &p)
{
    const std::string current = project_path_utf8(plater);
    if (current.empty()) return false;
    boost::system::error_code ec;
    const boost::filesystem::path cur = path_from_utf8(current);
    if (boost::filesystem::exists(cur, ec) && boost::filesystem::exists(p, ec))
        return boost::filesystem::equivalent(p, cur, ec);
    return p.lexically_normal() == cur.lexically_normal();
}

// One file entry for GET /project/recent and GET /project/browse: path, the
// project name (file name without .3mf), whether it is still there, and when it
// exists its size and last write time. `open` marks the open project's own file.
static nlohmann::json project_file_json(Plater *plater, const boost::filesystem::path &p, const std::string &path_utf8)
{
    boost::system::error_code ec;
    const bool exists = boost::filesystem::is_regular_file(p, ec);
    nlohmann::json j = {
        {"path", path_utf8},
        {"name", path_to_utf8(p.stem())},
        {"exists", exists},
        {"open", is_open_project_file(plater, p)},
    };
    if (exists) {
        const uintmax_t size = boost::filesystem::file_size(p, ec);
        if (!ec) j["size_bytes"] = size;
        const std::time_t t = boost::filesystem::last_write_time(p, ec);
        if (!ec) j["modified"] = iso_utc(t);
    }
    return j;
}

// File > Recent, most recent first. MainFrame keeps this list and the app
// config entry in step (add_to_recent_projects / remove_recent_project), and
// the config entry is the readable copy.
static nlohmann::json recent_projects_json(Plater *plater)
{
    const std::vector<std::string> paths = wxGetApp().app_config->get_recent_projects();
    nlohmann::json arr = nlohmann::json::array();
    for (size_t i = 0; i < paths.size(); ++i) {
        nlohmann::json j = project_file_json(plater, path_from_utf8(paths[i]), paths[i]);
        j["index"] = i;
        arr.push_back(std::move(j));
    }
    return {{"recent", arr}, {"count", paths.size()}};
}

// The recent-list entries that name the file at `p` (spelled any way), found
// while the file still exists; used before a move or delete so the list can
// be corrected afterwards.
static std::vector<std::string> recent_entries_for(const boost::filesystem::path &p)
{
    std::vector<std::string> hits;
    boost::system::error_code ec;
    for (const std::string &s : wxGetApp().app_config->get_recent_projects()) {
        const boost::filesystem::path q = path_from_utf8(s);
        const bool same = (boost::filesystem::exists(q, ec) && boost::filesystem::exists(p, ec))
                              ? boost::filesystem::equivalent(q, p, ec)
                              : q.lexically_normal() == p.lexically_normal();
        if (same) hits.push_back(s);
    }
    return hits;
}

static void forget_recent_entries(const std::vector<std::string> &entries)
{
    for (const std::string &s : entries)
        wxGetApp().mainframe->remove_recent_project(size_t(-1), from_u8(s));
}

// Query value from a GET target, percent-decoded ("" when absent).
static std::string target_query_param(const std::string &target, const char *name)
{
    auto qpos = target.find('?');
    if (qpos == std::string::npos) return {};
    const std::string key = std::string(name) + "=";
    const std::string q   = target.substr(qpos + 1);
    size_t start = 0;
    while (start <= q.size()) {
        auto amp = q.find('&', start);
        std::string item = q.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
        if (item.size() >= key.size() && item.compare(0, key.size(), key) == 0)
            return url_decode(item.substr(key.size()));
        if (amp == std::string::npos) break;
        start = amp + 1;
    }
    return {};
}

// --- project: GET /project, save / open / new -----------------------------------

Response Controller::handle_get_project()
{
    nlohmann::json r = run_on_ui([]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        nlohmann::json j = project_json(plater);
        j["modified"] = dirty_preset_groups();
        j["recent_count"] = wxGetApp().app_config->get_recent_projects().size();
        return j;
    });
    return { 200, r };
}

// POST /project/save  body (optional) {"path": "<orca-host path>.3mf" | null, "overwrite": false}
// path null: save to the project's own path (File > Save). A path: File > Save As,
// which refuses to replace an existing file unless overwrite is true. Saving the
// project back onto its own path is a plain Save and never counts as overwriting.
Response Controller::handle_save_project(const std::string &body)
{
    std::string path;
    bool overwrite = false;
    if (!body.empty()) {
        nlohmann::json in = nlohmann::json::parse(body); // parse_error -> 400 in dispatch
        if (!in.is_object()) return { 400, {{"error", "body_must_be_object"}} };
        if (in.contains("path") && !in["path"].is_null()) {
            if (!in["path"].is_string()) return { 400, {{"error", "path_must_be_string"}} };
            path = in["path"].get<std::string>();
        }
        if (in.contains("overwrite")) {
            if (!in["overwrite"].is_boolean()) return { 400, {{"error", "overwrite_must_be_boolean"}} };
            overwrite = in["overwrite"].get<bool>();
        }
    }
    if (!path.empty() && !path_is_3mf(path))
        return { 400, {{"error", "not_3mf"}, {"detail", "the project path must end in .3mf"}} };

    // 120 s: export_3mf renders a thumbnail per plate and writes every mesh.
    nlohmann::json r = run_on_ui([path, overwrite]() -> nlohmann::json {
        auto refuse = [&path](nlohmann::json e) -> nlohmann::json {
            mcp_refused_reply(path.empty() ? std::string(MCP_T("Save project"))
                                           : mcp_fmt(MCP_T("Save project as {name}"), {{"name", mcp_file_name(path)}}),
                              e, "POST /project/save");
            return e;
        };
        Plater *plater = wxGetApp().plater();
        if (plater->is_background_process_slicing()) return refuse({{"error", "slicing"}});
        const std::string current = project_path_utf8(plater);
        const std::string target  = path.empty() ? current : path;
        if (target.empty())
            return refuse({{"error", "no_project_path"},
                           {"detail", "the project has never been saved; pass a path"}});
        if (nlohmann::json e = mcp_history_refusal(target); !e.is_null()) return refuse(e);
        const boost::filesystem::path bp = path_from_utf8(target);
        boost::system::error_code ec;
        if (!bp.parent_path().empty() && !boost::filesystem::is_directory(bp.parent_path(), ec))
            return refuse({{"error", "directory_not_found"}, {"path", target}});
        if (!path.empty() && boost::filesystem::exists(bp, ec) && !is_open_project_file(plater, bp)) {
            if (!overwrite)
                return refuse({{"error", "exists"}, {"path", target},
                               {"detail", "a file is already there; pass overwrite:true to replace it"}});
            if (!project_file_changes_allowed())
                return refuse({{"error", "blocked_by_remote_api_policy"}, {"path", target},
                               {"detail", "replacing a file needs Preferences > Remote API > Allow deleting and replacing project files"}});
        }
        // MCP panel: a save changes no project state, but it is the assistant's change; the row lands in the
        // history of the path saved to (the recorder follows a first save or Save As).
        mcp_before_change();
        uintmax_t bytes = 0;
        const std::string err = save_project_to(plater, target, bytes);
        if (!err.empty()) return refuse({{"error", err}, {"path", target}});
        const std::string saved = project_path_utf8(plater);
        McpRow row = mcp_row("file", MCP_T("Saved project"), mcp_file_name(saved), std::string(), saved);
        row.on_disk = true;
        row.target  = {{"type", "project_saved"}, {"path", saved}};
        mcp_record(std::move(row));
        return {{"saved", saved}, {"bytes", bytes}, {"project", project_json(plater)}};
    }, /*timeout_s=*/120);
    if (r.contains("error")) {
        if (r["error"] == "blocked_by_remote_api_policy")
            api_notify("Couldn't save the project over another file (blocked, see Preferences > Remote API)", true);
        else if (r["error"] != "slicing")
            api_notify("Couldn't save the project (" + r["error"].get<std::string>() + ")", true);
        return project_error_response(r);
    }
    api_notify("Saved project " + path_to_utf8(path_from_utf8(r["saved"].get<std::string>()).filename()));
    return { 200, r };
}

// POST /project/open's work once the request is read, shared with the MCP panel's Restore: checks (file, slicing,
// loading), the sensitive-key check when policy_check, then before_load (the unsaved-changes prompt and the
// recorder; it may add to the reply or return an error), then OrcaSlicer's own open with its prompts answered and,
// with keep_presets, the earlier presets selected back. Returns the reply's fields, or {"error", ...}. Records
// nothing in the MCP panel. GUI thread only.
nlohmann::json Controller::open_project_from(Plater *plater, const std::string &path, bool keep_presets, bool policy_check,
                                             const std::function<nlohmann::json(nlohmann::json &)> &before_load)
{
    auto *bundle = wxGetApp().preset_bundle;
    const wxString wpath = from_u8(path);
    boost::system::error_code ec;
    // file_not_found, not not_found: the MCP client reads a bare not_found as "route missing".
    if (!boost::filesystem::is_regular_file(into_path(wpath), ec)) {
        // File > Recent would offer to drop a missing entry; say so instead.
        nlohmann::json e = {{"error", "file_not_found"}, {"path", path}};
        if (!recent_entries_for(into_path(wpath)).empty())
            e["detail"] = "the file is in the recent list but no longer on disk; DELETE /project/recent forgets it";
        return e;
    }
    if (plater->is_background_process_slicing()) return {{"error", "slicing"}};
    if (plater->is_loading_project()) return {{"error", "busy"}};

    // Before anything changes: the project must not bring script, G-code or
    // connection settings of its own unless the user allows those edits.
    if (policy_check && !sensitive_writes_allowed()) {
        std::set<std::string> keys;
        try {
            keys = project_sensitive_changes(path, !keep_presets, keep_presets ? bundle->filament_presets.size() : 0);
        } catch (const std::exception &ex) {
            return {{"error", "load_failed"}, {"path", path}, {"detail", ex.what()}};
        }
        if (!keys.empty())
            return {{"error", "blocked_by_remote_api_policy"}, {"path", path}, {"keys", keys},
                    {"detail", "the project brings its own script, G-code or connection settings; open it in "
                               "OrcaSlicer, or tick Preferences > Remote API > Allow script, G-code and "
                               "connection edits"}};
    }

    nlohmann::json out = nlohmann::json::object();
    if (before_load) {
        nlohmann::json e = before_load(out);
        if (e.is_object() && e.contains("error")) return e;
    }

    // What the open will replace. With presets: keep_current these are
    // selected again once the project is in.
    const std::string              printer_before   = bundle->printers.get_selected_preset_name();
    const std::string              print_before     = bundle->prints.get_selected_preset_name();
    const std::vector<std::string> filaments_before = bundle->filament_presets;

    // Edits that are not saved to a preset would open an UnsavedChangesDialog
    // in Plater::load_project's check. The project's config replaces them in
    // any case (it is loaded over the presets it names), so drop them first
    // and report that we did.
    out["discarded_changes"] = discard_preset_edits();

    // Plater::load_project asks "save changes?" through close_with_confirm,
    // which honours the remembered answer in app config; a project whose
    // presets carry custom G-code asks for confirmation unless that warning is
    // off; and with a printer connected, load_files offers to sync its preset
    // unless the choice is remembered. All three are set for the duration of
    // the call and put back.
    {
        AppConfigOverride save_choice("save_project_choise", "no");
        AppConfigOverride gcode_warn("no_warn_when_modified_gcodes", "true");
        AppConfigOverride printer_sync("sync_after_load_file_show_flag", "false");
        plater->load_project(wpath, "<loadall>"); // "<loadall>": open as a project, no import-type dialog
    }

    // load_project returns nothing; a file it could not read leaves the
    // project name alone.
    const boost::filesystem::path now = into_path(plater->get_project_filename(".3mf"));
    const bool opened = !now.empty() && boost::filesystem::exists(now, ec) &&
                        boost::filesystem::equivalent(now, into_path(wpath), ec);
    if (!opened)
        return {{"error", "load_failed"}, {"path", path},
                {"detail", "OrcaSlicer could not open it as a project; a .3mf with no objects "
                           "in it is not opened either"}};

    if (keep_presets) {
        // The project's config sits as edits over the presets it selected;
        // drop those (and reload the tabs, which still show them) and select
        // the earlier presets back, printer first so the print and filament
        // choices are judged against it. The pre-discard is what keeps
        // Tab::select_preset from asking about unsaved edits.
        if (!discard_preset_edits().empty())
            wxGetApp().load_current_presets(false, false);
        nlohmann::json restored = nlohmann::json::array();
        auto reselect = [&restored](Preset::Type type, PresetCollection &c, const std::string &name, const char *group) {
            if (name.empty() || c.get_selected_preset_name() == name) return;
            if (c.find_preset(name, false, true) == nullptr) return; // gone since; keep the project's
            Tab *tab = wxGetApp().get_tab(type);
            if (tab != nullptr && tab->select_preset(name, false, "", /*force_select=*/true, /*force_no_transfer=*/true))
                restored.push_back(group);
        };
        reselect(Preset::TYPE_PRINTER, bundle->printers, printer_before, "printer");
        reselect(Preset::TYPE_PRINT,   bundle->prints,   print_before,   "print");
        bool filaments_changed = false;
        for (size_t i = 0; i < filaments_before.size() && i < bundle->filament_presets.size(); ++i) {
            if (bundle->filament_presets[i] == filaments_before[i]) continue;
            if (bundle->filaments.find_preset(filaments_before[i], false, true) == nullptr) continue;
            bundle->set_filament_preset(i, filaments_before[i]);
            filaments_changed = true;
        }
        if (filaments_changed) {
            restored.push_back("filament");
            plater->sidebar().update_all_preset_comboboxes();
            plater->sidebar().update_dynamic_filament_list();
        }
        // Unconditional: the discard alone changes what the plater slices with.
        bundle->export_selections(*wxGetApp().app_config);
        plater->update_project_dirty_from_presets();
        plater->get_partplate_list().invalid_all_slice_result();
        plater->on_config_change(bundle->full_config());
        out["presets_restored"] = restored;
    }

    if (g_controller != nullptr) g_controller->sync_slice_state_to_current_plate("slice.plate_changed");
    out["opened"]  = project_path_utf8(plater);
    out["project"] = project_json(plater);
    out["plates"]  = plater->get_partplate_list().get_plate_count();
    out["objects"] = (unsigned) plater->model().objects.size();
    out["presets"] = {{"printer", bundle->printers.get_selected_preset_name()},
                      {"print", bundle->prints.get_selected_preset_name()},
                      {"filaments", bundle->filament_presets}};
    return out;
}

// POST /project/open  body {"path": "<orca-host path>.3mf" | "recent": <index>,
//                           "if_unsaved": "save" | "discard" | "fail" (default fail),
//                           "presets": "project" | "keep_current" (default project)}
// File > Open Project: plates, objects, per-object and per-plate settings, the
// slot -> preset mapping and colours, and the project's config. `recent` names
// an entry of GET /project/recent instead of a path (File > Recent). `presets`
// decides whose printer/print/filament presets are selected afterwards: the
// project's own (as the GUI does) or the ones selected before the open.
Response Controller::handle_open_project(const std::string &body)
{
    nlohmann::json in = nlohmann::json::parse(body); // parse_error -> 400 in dispatch
    if (!in.is_object()) return { 400, {{"error", "body_must_be_object"}} };
    std::string path;
    if (in.contains("path") && !in["path"].is_null()) {
        if (!in["path"].is_string()) return { 400, {{"error", "path_must_be_string"}} };
        path = in["path"].get<std::string>();
    }
    if (path.empty() && in.contains("recent") && !in["recent"].is_null()) {
        if (!in["recent"].is_number_integer() || in["recent"].get<long long>() < 0)
            return { 400, {{"error", "bad_recent"}, {"detail", "an index from GET /project/recent"}} };
        const size_t idx = in["recent"].get<size_t>();
        nlohmann::json resolved = run_on_ui([idx]() -> nlohmann::json {
            const std::vector<std::string> paths = wxGetApp().app_config->get_recent_projects();
            if (idx >= paths.size()) return {{"error", "unknown_recent"}, {"index", idx}, {"count", paths.size()}};
            return {{"path", paths[idx]}};
        });
        if (resolved.contains("error")) return project_error_response(resolved);
        path = resolved["path"].get<std::string>();
    }
    if (path.empty()) return { 400, {{"error", "missing_path"}, {"detail", "pass path or recent"}} };
    std::string if_unsaved = "fail";
    if (!parse_if_unsaved(in, if_unsaved))
        return { 400, {{"error", "bad_if_unsaved"}, {"detail", "save | discard | fail"}} };
    std::string presets = "project";
    if (in.contains("presets") && !in["presets"].is_null()) {
        if (!in["presets"].is_string()) return { 400, {{"error", "bad_presets"}, {"detail", "project | keep_current"}} };
        presets = in["presets"].get<std::string>();
        if (presets != "project" && presets != "keep_current")
            return { 400, {{"error", "bad_presets"}, {"detail", "project | keep_current"}} };
    }
    if (!path_is_3mf(path))
        return { 422, {{"error", "unsupported_format"},
                       {"detail", "open takes a .3mf project; POST /model imports geometry"}} };

    // 120 s like POST /model: a large project takes a while to read and render.
    nlohmann::json r = run_on_ui([path, if_unsaved, presets]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        if (nlohmann::json e = mcp_history_refusal(path); !e.is_null()) {
            mcp_refused_reply(mcp_fmt(MCP_T("Open project {name}"), {{"name", mcp_file_name(path)}}), e,
                              "POST /project/open");
            return e;
        }
        nlohmann::json out = open_project_from(plater, path, presets == "keep_current", /*policy_check=*/true,
            [plater, if_unsaved](nlohmann::json &o) -> nlohmann::json {
                const std::string err = answer_unsaved_prompt(plater, if_unsaved, o);
                if (!err.empty()) {
                    nlohmann::json e = {{"error", err}, {"project", project_json(plater)}, {"modified", dirty_preset_groups()}};
                    if (err == "unsaved_changes")
                        e["detail"] = "pass if_unsaved: \"save\" or \"discard\"";
                    return e;
                }
                // MCP panel: the outgoing project's group (and checkpoint) before anything is replaced; the row
                // is recorded after the load, into the opened project's history.
                mcp_before_change();
                return nullptr;
            });
        if (out.contains("error")) {
            mcp_refused_reply(mcp_fmt(MCP_T("Open project {name}"), {{"name", mcp_file_name(path)}}), out,
                              "POST /project/open");
            return out;
        }
        {
            const std::string opened_path = project_path_utf8(plater);
            McpRow row = mcp_row("file", MCP_T("Opened project"), mcp_file_name(opened_path), std::string(), opened_path);
            row.title_hint = "opened";
            row.target     = {{"type", "project_opened"}, {"path", opened_path}};
            mcp_record(std::move(row)); // syncs to the opened project's history first
        }
        return out;
    }, /*timeout_s=*/120);
    const std::string fn = path_to_utf8(path_from_utf8(path).filename());
    if (r.contains("error")) {
        if (r["error"] == "blocked_by_remote_api_policy")
            api_notify("Couldn't open " + fn + ": it changes scripts, G-code or the printer connection (blocked, see Preferences > Remote API)", true);
        else if (r["error"] != "slicing" && r["error"] != "unsaved_changes")
            api_notify("Couldn't open " + fn + " (" + r["error"].get<std::string>() + ")", true);
        return project_error_response(r);
    }
    api_notify("Opened project " + fn);
    return { 200, r };
}

// POST /project/new  body (optional) {"if_unsaved": "save" | "discard" | "fail", "name": "<title>"}
// File > New Project. The GUI's second prompt (keep the modified presets for the
// new project, or discard them?) is answered with Keep, which is what
// Plater::new_project does when its confirmation is skipped: nothing is lost
// that was not asked for, and the reply lists the groups carried over. `name`
// gives the new project its title (Untitled otherwise); the file name is chosen
// when it is saved.
Response Controller::handle_new_project(const std::string &body)
{
    std::string if_unsaved = "fail";
    std::string name;
    if (!body.empty()) {
        nlohmann::json in = nlohmann::json::parse(body); // parse_error -> 400 in dispatch
        if (!in.is_object()) return { 400, {{"error", "body_must_be_object"}} };
        if (!parse_if_unsaved(in, if_unsaved))
            return { 400, {{"error", "bad_if_unsaved"}, {"detail", "save | discard | fail"}} };
        if (in.contains("name") && !in["name"].is_null()) {
            if (!in["name"].is_string()) return { 400, {{"error", "name_must_be_string"}} };
            name = in["name"].get<std::string>();
            boost::algorithm::trim(name);
        }
    }
    nlohmann::json r = run_on_ui([this, if_unsaved, name]() -> nlohmann::json {
        auto refuse = [](nlohmann::json e) -> nlohmann::json {
            mcp_refused_reply(MCP_T("New project"), e, "POST /project/new");
            return e;
        };
        Plater *plater = wxGetApp().plater();
        if (plater->is_background_process_slicing()) return refuse({{"error", "slicing"}});
        if (plater->is_loading_project()) return refuse({{"error", "busy"}});
        nlohmann::json out = nlohmann::json::object();
        const std::string err = answer_unsaved_prompt(plater, if_unsaved, out);
        if (!err.empty()) {
            nlohmann::json e = {{"error", err}, {"project", project_json(plater)}, {"modified", dirty_preset_groups()}};
            if (err == "unsaved_changes")
                e["detail"] = "pass if_unsaved: \"save\" or \"discard\"";
            return refuse(e);
        }
        out["kept_changes"] = dirty_preset_groups();
        // MCP panel: the outgoing project's checkpoint first; the row goes to the new project's history.
        mcp_before_change();
        // close_with_confirm drops the old project's backup folder before the
        // model is replaced; with the confirmation skipped, do it here.
        plater->model().set_backup_path("");
        if (plater->new_project(/*skip_confirm=*/true, /*silent=*/false, from_u8(name)) == wxID_CANCEL)
            return refuse({{"error", "cancelled"}});
        sync_slice_state_to_current_plate("slice.plate_changed");
        {
            const std::string title = name.empty() ? std::string(MCP_T("Untitled")) : name;
            McpRow row = mcp_row("file", MCP_T("New project"), title);
            row.target = {{"type", "project_new"}, {"name", title}};
            mcp_record(std::move(row));
        }
        out["ok"]      = true;
        out["project"] = project_json(plater);
        return out;
    }, /*timeout_s=*/120);
    if (r.contains("error")) {
        if (r["error"] != "slicing" && r["error"] != "unsaved_changes")
            api_notify("Couldn't start a new project (" + r["error"].get<std::string>() + ")", true);
        return project_error_response(r);
    }
    api_notify(name.empty() ? std::string("New project") : "New project " + name);
    return { 200, r };
}

// --- project files: recent list, browse, move, delete ----------------------------

// GET /project/recent - File > Recent, most recent first, with whether each file
// is still on disk.
Response Controller::handle_get_recent_projects()
{
    nlohmann::json r = run_on_ui([]() -> nlohmann::json {
        return recent_projects_json(wxGetApp().plater());
    });
    return { 200, r };
}

// DELETE /project/recent  body {"path": "<path>"} | {"index": <n>} | {"all": true}
// Takes an entry (or every entry) off File > Recent. Nothing on disk changes.
Response Controller::handle_forget_recent_project(const std::string &body)
{
    if (body.empty()) return { 400, {{"error", "missing_target"}, {"detail", "pass path, index or all:true"}} };
    nlohmann::json in = nlohmann::json::parse(body); // parse_error -> 400 in dispatch
    if (!in.is_object()) return { 400, {{"error", "body_must_be_object"}} };
    std::string path;
    long long   index = -1;
    bool        all   = false;
    if (in.contains("all") && !in["all"].is_null()) {
        if (!in["all"].is_boolean()) return { 400, {{"error", "all_must_be_boolean"}} };
        all = in["all"].get<bool>();
    }
    if (in.contains("path") && !in["path"].is_null()) {
        if (!in["path"].is_string()) return { 400, {{"error", "path_must_be_string"}} };
        path = in["path"].get<std::string>();
    }
    if (in.contains("index") && !in["index"].is_null()) {
        if (!in["index"].is_number_integer() || in["index"].get<long long>() < 0)
            return { 400, {{"error", "bad_index"}, {"detail", "an index from GET /project/recent"}} };
        index = in["index"].get<long long>();
    }
    if (!all && path.empty() && index < 0)
        return { 400, {{"error", "missing_target"}, {"detail", "pass path, index or all:true"}} };

    nlohmann::json r = run_on_ui([path, index, all]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        std::vector<std::string> before = wxGetApp().app_config->get_recent_projects();
        nlohmann::json forgotten = nlohmann::json::array();
        if (all) {
            wxGetApp().mainframe->remove_recent_project(size_t(-1), wxString());
            forgotten = before;
        } else {
            std::string target = path;
            if (target.empty()) {
                if (size_t(index) >= before.size())
                    return {{"error", "unknown_recent"}, {"index", index}, {"count", before.size()}};
                target = before[size_t(index)];
            }
            // The stored spelling, or any entry naming the same file.
            std::vector<std::string> entries;
            for (const std::string &s : before)
                if (s == target) entries.push_back(s);
            if (entries.empty()) entries = recent_entries_for(path_from_utf8(target));
            if (entries.empty()) return {{"error", "not_in_recent"}, {"path", target}};
            forget_recent_entries(entries);
            forgotten = entries;
        }
        nlohmann::json out = recent_projects_json(plater);
        out["forgotten"] = forgotten;
        return out;
    });
    if (r.contains("error")) return project_error_response(r);
    return { 200, r };
}

// GET /project/browse?dir=<path>  - the .3mf files in one folder (not recursive),
// newest first. Without dir: the open project's folder, else the folder the last
// file dialog used.
Response Controller::handle_browse_projects(const std::string &target)
{
    const std::string dir = target_query_param(target, "dir");
    nlohmann::json r = run_on_ui([dir]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        std::string folder = dir;
        if (folder.empty()) {
            const std::string current = project_path_utf8(plater);
            if (!current.empty()) folder = path_to_utf8(path_from_utf8(current).parent_path());
        }
        if (folder.empty()) folder = wxGetApp().app_config->get_last_dir();
        if (folder.empty())
            return {{"error", "missing_dir"}, {"detail", "no project is open and no folder has been used yet; pass dir"}};
        const boost::filesystem::path bp = path_from_utf8(folder);
        boost::system::error_code ec;
        if (!boost::filesystem::is_directory(bp, ec)) return {{"error", "directory_not_found"}, {"dir", folder}};

        struct Entry { std::time_t t; nlohmann::json j; };
        std::vector<Entry> entries;
        for (boost::filesystem::directory_iterator it(bp, ec), end; !ec && it != end; it.increment(ec)) {
            const boost::filesystem::path p = it->path();
            const std::string fn = path_to_utf8(p.filename());
            if (fn.empty() || fn[0] == '.' || !path_is_3mf(fn)) continue;
            boost::system::error_code fec;
            if (!boost::filesystem::is_regular_file(p, fec)) continue;
            const std::time_t t = boost::filesystem::last_write_time(p, fec);
            entries.push_back({fec ? 0 : t, project_file_json(plater, p, path_to_utf8(p))});
        }
        std::stable_sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.t > b.t; });
        const size_t limit = 500;
        nlohmann::json files = nlohmann::json::array();
        for (size_t i = 0; i < entries.size() && i < limit; ++i) files.push_back(std::move(entries[i].j));
        return {{"dir", path_to_utf8(bp)}, {"files", files}, {"count", entries.size()},
                {"truncated", entries.size() > limit}};
    }, /*timeout_s=*/30);
    if (r.contains("error")) return project_error_response(r);
    return { 200, r };
}

// POST /project/move  body {"from": "<path>.3mf", "to": "<path>.3mf", "overwrite": false}
// Renames or moves a project file. When it is the open project's file the
// window follows (title, project name, folder, File > Recent); otherwise a
// recent-list entry naming the old path is pointed at the new one.
Response Controller::handle_move_project_file(const std::string &body)
{
    nlohmann::json in = nlohmann::json::parse(body); // parse_error -> 400 in dispatch
    if (!in.is_object() || !in.contains("from") || !in["from"].is_string() || !in.contains("to") || !in["to"].is_string())
        return { 400, {{"error", "missing_path"}, {"detail", "pass from and to"}} };
    const std::string from = in["from"].get<std::string>();
    const std::string to   = in["to"].get<std::string>();
    bool overwrite = false;
    if (in.contains("overwrite")) {
        if (!in["overwrite"].is_boolean()) return { 400, {{"error", "overwrite_must_be_boolean"}} };
        overwrite = in["overwrite"].get<bool>();
    }
    if (!path_is_3mf(from) || !path_is_3mf(to))
        return { 400, {{"error", "not_3mf"}, {"detail", "both paths must end in .3mf"}} };

    nlohmann::json r = run_on_ui([from, to, overwrite]() -> nlohmann::json {
        auto refuse = [&from](nlohmann::json e) -> nlohmann::json {
            mcp_refused_reply(mcp_fmt(MCP_T("Move {name}"), {{"name", mcp_file_name(from)}}), e, "POST /project/move");
            return e;
        };
        Plater *plater = wxGetApp().plater();
        if (nlohmann::json e = mcp_history_refusal(from); !e.is_null()) return refuse(e);
        if (nlohmann::json e = mcp_history_refusal(to); !e.is_null()) return refuse(e);
        const boost::filesystem::path src = path_from_utf8(from);
        const boost::filesystem::path dst = path_from_utf8(to);
        boost::system::error_code ec;
        if (!boost::filesystem::is_regular_file(src, ec)) return refuse({{"error", "file_not_found"}, {"path", from}});
        if (!dst.parent_path().empty() && !boost::filesystem::is_directory(dst.parent_path(), ec))
            return refuse({{"error", "directory_not_found"}, {"path", to}});
        if (boost::filesystem::exists(dst, ec)) {
            if (boost::filesystem::equivalent(src, dst, ec)) {
                // On a case-insensitive disk "a.3mf" -> "A.3mf" names the same
                // file; that is a rename, not a clash. Anything else is a no-op.
                if (src.filename() == dst.filename()) return refuse({{"error", "same_path"}, {"path", to}});
            } else {
                if (!overwrite)
                    return refuse({{"error", "exists"}, {"path", to}, {"detail", "a file is already there; pass overwrite:true to replace it"}});
                if (is_open_project_file(plater, dst))
                    return refuse({{"error", "project_is_open"}, {"path", to}, {"detail", "the destination is the open project's file"}});
                if (!project_file_changes_allowed())
                    return refuse({{"error", "blocked_by_remote_api_policy"}, {"path", to},
                                   {"detail", "replacing a file needs Preferences > Remote API > Allow deleting and replacing project files"}});
            }
        }
        const bool was_open = is_open_project_file(plater, src);
        const std::vector<std::string> recent = recent_entries_for(src);
        // MCP panel: for the open project its history is keyed by the Plater's own path (spelled as the Plater
        // has it, which may differ from the request's `from`).
        const std::string history_from = was_open ? project_path_utf8(plater) : from;
        mcp_before_change();

        boost::filesystem::rename(src, dst, ec);
        if (ec) {
            // Across volumes rename fails; copy and remove instead.
            boost::system::error_code cec;
            boost::filesystem::copy_file(src, dst,
                                         boost::filesystem::copy_option::overwrite_if_exists, cec);
            if (cec) return refuse({{"error", "move_failed"}, {"path", to}, {"detail", cec.message()}});
            boost::filesystem::remove(src, cec);
            if (cec) return refuse({{"error", "move_failed"}, {"path", from},
                                    {"detail", "copied to the new path but the old file could not be removed: " + cec.message()}});
        }

        forget_recent_entries(recent);
        if (was_open) {
            // The same bookkeeping Save As does for the name: title, project
            // folder, recent list. Nothing about the content or its dirty state changes.
            plater->set_project_filename(from_path(dst));
            plater->update_title_dirty_status();
        } else if (!recent.empty()) {
            wxGetApp().mainframe->add_to_recent_projects(from_path(dst));
        }
        {
            // The history follows the file first, so the row lands in it (also for the open project).
            const std::string new_path = was_open ? project_path_utf8(plater) : path_to_utf8(dst);
            try {
                McpRecorder::get().project_moved(history_from, new_path);
            } catch (const std::exception &e) {
                BOOST_LOG_TRIVIAL(error) << "MCP panel: history not moved: " << e.what();
            }
            const bool same_dir = src.parent_path().lexically_normal() == dst.parent_path().lexically_normal();
            McpRow row = mcp_row("file", same_dir ? MCP_T("Renamed") : MCP_T("Moved"), path_to_utf8(dst.filename()), from);
            row.on_disk = true;
            row.target  = {{"type", "file_moved"}, {"from", from}, {"to", path_to_utf8(dst)}, {"was_open", was_open}};
            mcp_undo(row, MCP_T("Move back"),
                     mcp_fmt(MCP_T("Move {name} back to {path}"), {{"name", path_to_utf8(dst.filename())}, {"path", from}}));
            mcp_record(std::move(row));
        }
        uintmax_t bytes = boost::filesystem::file_size(dst, ec);
        if (ec) bytes = 0;
        return {{"moved", from}, {"to", path_to_utf8(dst)}, {"bytes", bytes}, {"was_open", was_open},
                {"recent_updated", was_open || !recent.empty()}, {"project", project_json(plater)}};
    }, /*timeout_s=*/120);
    if (r.contains("error")) {
        const std::string why = r["error"] == "blocked_by_remote_api_policy"
                                    ? std::string(" (blocked, see Preferences > Remote API)")
                                    : " (" + r["error"].get<std::string>() + ")";
        api_notify("Couldn't move " + path_to_utf8(path_from_utf8(from).filename()) + why, true);
        return project_error_response(r);
    }
    api_notify("Moved project to " + path_to_utf8(path_from_utf8(to).filename()));
    return { 200, r };
}

// DELETE /project/file  body {"path": "<path>.3mf"}
// Removes a project file from disk (permanently: there is no trash) and from
// File > Recent. The open project's own file is refused; start a new project or
// open another one first.
Response Controller::handle_delete_project_file(const std::string &body)
{
    nlohmann::json in = nlohmann::json::parse(body); // parse_error -> 400 in dispatch
    if (!in.is_object() || !in.contains("path") || !in["path"].is_string())
        return { 400, {{"error", "missing_path"}} };
    const std::string path = in["path"].get<std::string>();
    if (!path_is_3mf(path))
        return { 400, {{"error", "not_3mf"}, {"detail", "only .3mf project files can be deleted here"}} };

    nlohmann::json r = run_on_ui([path]() -> nlohmann::json {
        auto refuse = [&path](nlohmann::json e) -> nlohmann::json {
            mcp_refused_reply(mcp_fmt(MCP_T("Delete {name}"), {{"name", mcp_file_name(path)}}), e, "DELETE /project/file");
            return e;
        };
        Plater *plater = wxGetApp().plater();
        if (nlohmann::json e = mcp_history_refusal(path); !e.is_null()) return refuse(e);
        const boost::filesystem::path p = path_from_utf8(path);
        boost::system::error_code ec;
        if (!project_file_changes_allowed())
            return refuse({{"error", "blocked_by_remote_api_policy"}, {"path", path},
                           {"detail", "deleting a file needs Preferences > Remote API > Allow deleting and replacing project files"}});
        if (!boost::filesystem::is_regular_file(p, ec)) return refuse({{"error", "file_not_found"}, {"path", path}});
        if (is_open_project_file(plater, p))
            return refuse({{"error", "project_is_open"}, {"path", path},
                           {"detail", "this is the open project's file; start a new project or open another first"}});
        const std::vector<std::string> recent = recent_entries_for(p);
        uintmax_t bytes = boost::filesystem::file_size(p, ec);
        if (ec) bytes = 0;
        mcp_before_change();
        boost::filesystem::remove(p, ec);
        if (ec) return refuse({{"error", "delete_failed"}, {"path", path}, {"detail", ec.message()}});
        forget_recent_entries(recent);
        McpRow row = mcp_row("file", MCP_T("Deleted file"), path_to_utf8(p.filename()), std::string(), path);
        row.on_disk = true;
        row.target  = {{"type", "file_deleted"}, {"path", path}};
        mcp_record(std::move(row));
        return {{"deleted", path}, {"bytes", bytes}, {"recent_updated", !recent.empty()}};
    }, /*timeout_s=*/120);
    if (r.contains("error")) {
        const std::string why = r["error"] == "blocked_by_remote_api_policy"
                                    ? std::string(" (blocked, see Preferences > Remote API)")
                                    : " (" + r["error"].get<std::string>() + ")";
        api_notify("Couldn't delete " + path_to_utf8(path_from_utf8(path).filename()) + why, true);
        return project_error_response(r);
    }
    api_notify("Deleted project file " + path_to_utf8(path_from_utf8(path).filename()));
    return { 200, r };
}

// --- Elegoo CANVAS: printer slots and the filament slot mapping ----------------
// The MQTT read runs on the io thread (dispatch is never on the GUI thread); run_on_ui only reads the presets and the
// project and reads/writes CanvasState, which is GUI-thread-only.

static const char *canvas_source_name(ElegooCanvas::MatchSource s)
{
    switch (s) {
    case ElegooCanvas::MatchSource::Saved:   return "saved";
    case ElegooCanvas::MatchSource::Matched: return "matched";
    case ElegooCanvas::MatchSource::User:    return "user";
    case ElegooCanvas::MatchSource::Api:     return "api";
    case ElegooCanvas::MatchSource::None:    break;
    }
    return "none";
}

// A JSON integer that fits an int.
static bool canvas_json_int(const nlohmann::json &v, int &out)
{
    if (v.is_number_unsigned()) {
        const uint64_t u = v.get<uint64_t>();
        if (u > static_cast<uint64_t>(std::numeric_limits<int>::max()))
            return false;
        out = static_cast<int>(u);
        return true;
    }
    if (v.is_number_integer()) {
        const int64_t i = v.get<int64_t>();
        if (i < std::numeric_limits<int>::min() || i > std::numeric_limits<int>::max())
            return false;
        out = static_cast<int>(i);
        return true;
    }
    return false;
}

// The filaments the next Send would map: the current plate's (the plate an API-driven Send acts on), as the send flow
// picks them; every project filament when the plate reports none. GUI thread only.
static std::vector<ElegooCanvas::ProjectFilament> canvas_send_filaments()
{
    return elegoo_canvas_project_filaments(elegoo_canvas_send_plate_filaments(PLATE_CURRENT_IDX));
}

// GET /printer/slot_mapping body for `info`: the filaments the next Send would map (canvas_send_filaments), each with
// its tray, so "valid" means that Send can go. The API-set mapping when it belongs to the open project (the send
// flow's rule), else saved choices and the matcher. Warnings are recomputed against the tray each row points at.
// GUI thread only.
static nlohmann::json canvas_mapping_json(const ElegooCanvas::Info &info)
{
    const std::vector<ElegooCanvas::ProjectFilament> filaments = canvas_send_filaments();
    const CanvasState                               &state     = CanvasState::get();
    std::vector<ElegooCanvas::Mapping>               mappings;
    if (state.project_key == elegoo_canvas_project_key() && !state.api_mapping.empty())
        mappings = elegoo_canvas_rows(filaments, state.api_mapping);
    else
        mappings = elegoo_canvas_rows(filaments, ElegooCanvas::auto_match(filaments, info, elegoo_canvas_mapping_store()));

    nlohmann::json rows = nlohmann::json::array();
    for (size_t i = 0; i < filaments.size(); ++i) {
        const ElegooCanvas::ProjectFilament &f    = filaments[i];
        ElegooCanvas::Mapping               &m    = mappings[i];
        nlohmann::json                       tray = nullptr;
        m.warnings.clear();
        if (m.tray) {
            const ElegooCanvas::Tray *t = info.find(m.tray->first, m.tray->second);
            if (t != nullptr)
                m.warnings = elegoo_canvas_mapping_warnings(f, *t, elegoo_canvas_nozzle_temp(f.name));
            tray = {{"canvas_id", m.tray->first},
                    {"tray_id", m.tray->second},
                    {"label", t != nullptr ? nlohmann::json(t->label()) : nlohmann::json(nullptr)}};
        }
        rows.push_back({{"index", f.index},
                        {"type", f.type},
                        {"name", f.name},
                        {"color", f.color},
                        {"tray", tray},
                        {"source", canvas_source_name(m.source)},
                        {"warnings", m.warnings}});
    }
    const std::vector<std::string> problems = ElegooCanvas::validate(mappings, info);
    return {{"supported", true},
            {"plate", elegoo_canvas_send_plate_index(PLATE_CURRENT_IDX)}, // 0-based plate the next Send would use
            {"filaments", rows},
            {"valid", problems.empty()},
            {"problems", problems}};
}

bool Controller::read_canvas_slots(bool use_cached, ElegooCanvas::Info &info, nlohmann::json &unsupported)
{
    auto unsupported_json = [](const std::string &reason, const std::string &message) {
        return nlohmann::json{{"supported", false}, {"reason", reason}, {"message", message}};
    };

    // Gate + a copy of the printer config (and the cached read) from the GUI thread. Shared pointers, not references:
    // a timed-out run_on_ui must not leave the GUI lambda writing into this frame.
    auto cfg         = std::make_shared<DynamicPrintConfig>();
    auto cached      = std::make_shared<std::optional<ElegooCanvas::Info>>();
    auto printer_key = std::make_shared<std::string>();
    nlohmann::json pre = run_on_ui([cfg, cached, printer_key, use_cached]() -> nlohmann::json {
        const DynamicPrintConfig &printer = wxGetApp().preset_bundle->printers.get_edited_preset().config;
        if (!elegoo_canvas_enabled(printer))
            return {{"gated", false}};
        *cfg         = printer;
        *printer_key = elegoo_canvas_printer_key(printer);
        if (use_cached) // only a read from this printer counts; another printer's slots are no cache
            *cached = CanvasState::get().cached_info(*printer_key);
        return {{"gated", true}};
    });
    if (!pre.value("gated", false)) {
        unsupported = unsupported_json("not_elegoo_cc2",
                                       "The selected printer profile is not an Elegoo Centauri Carbon 2 with multi-filament (CANVAS) support.");
        return false;
    }

    if (cached->has_value() && (*cached)->connected()) {
        info = **cached;
        return true;
    }

    // Network read on this (io) thread: MQTT plus maybe the HTTP serial lookup, bounded by the session's timeouts.
    ElegooCanvas::Session::Result result;
    try {
        ElegooLink link(cfg.get());
        result = link.get_canvas_info(info);
    } catch (const std::exception &e) {
        result.ok         = false;
        result.error_code = "unreachable";
        result.message    = e.what();
    } catch (...) {
        result.ok         = false;
        result.error_code = "unreachable";
        result.message    = "unknown error";
    }
    if (!result.ok) {
        unsupported = unsupported_json(result.error_code.empty() ? "unreachable" : result.error_code,
                                       result.message.empty() ? "The printer did not answer." : result.message);
        return false;
    }
    if (!info.connected()) {
        unsupported = unsupported_json("no_canvas", "The printer reports no connected CANVAS unit.");
        return false;
    }

    auto stored = std::make_shared<ElegooCanvas::Info>(info);
    try {
        run_on_ui([stored, printer_key]() -> nlohmann::json {
            CanvasState::get().store_info(*printer_key, *stored);
            return nullptr;
        });
    } catch (const std::runtime_error &) {
        // ui_timeout: the read itself succeeded, so answer with it; the next call reads the printer again.
    }
    return true;
}

// GET /printer/slots - a fresh read of the CANVAS trays.
Response Controller::handle_printer_slots()
{
    ElegooCanvas::Info info;
    nlohmann::json     unsupported;
    if (!read_canvas_slots(false, info, unsupported))
        return { 200, unsupported };
    nlohmann::json j = ElegooCanvas::info_json(info);
    j["supported"]   = true;
    return { 200, j };
}

// GET /printer/slot_mapping - the mapping the next send pre-fills, against the last slot read (read now when none).
Response Controller::handle_get_slot_mapping()
{
    ElegooCanvas::Info info;
    nlohmann::json     unsupported;
    if (!read_canvas_slots(true, info, unsupported))
        return { 200, unsupported };
    auto shared = std::make_shared<ElegooCanvas::Info>(std::move(info));
    nlohmann::json r = run_on_ui([shared]() -> nlohmann::json { return canvas_mapping_json(*shared); });
    return { 200, r };
}

// PUT /printer/slot_mapping  body {"mapping":[{"filament":<0-based index>,"canvas_id":int,"tray_id":int}]}
// Replaces the API-set mapping of the open project (an empty array clears it, so the send falls back to saved choices
// and the matcher). Drives nothing until the user sends, so it is not a sensitive write.
Response Controller::handle_put_slot_mapping(const std::string &body)
{
    nlohmann::json in = nlohmann::json::parse(body); // parse_error -> 400 invalid_json in dispatch
    if (!in.is_object())
        return { 400, {{"error", "body_must_be_object"}} };
    auto list = in.find("mapping");
    if (list == in.end() || !list->is_array())
        return { 400, {{"error", "mapping_must_be_array"}} };

    struct Entry
    {
        int            filament  = 0;
        int            canvas_id = 0;
        int            tray_id   = 0;
        nlohmann::json raw;
    };
    std::vector<Entry> entries;
    std::set<int>      seen_filaments;
    for (const nlohmann::json &e : *list) {
        if (!e.is_object())
            return { 400, {{"error", "entry_must_be_object"}, {"entry", e}} };
        Entry entry;
        entry.raw = e;
        const std::pair<const char *, int *> fields[] = {{"filament", &entry.filament},
                                                         {"canvas_id", &entry.canvas_id},
                                                         {"tray_id", &entry.tray_id}};
        for (const auto &field : fields) {
            auto v = e.find(field.first);
            if (v == e.end() || !canvas_json_int(*v, *field.second))
                return { 400, {{"error", std::string(field.first) + "_must_be_integer"}, {"entry", e}} };
        }
        if (!seen_filaments.insert(entry.filament).second)
            return { 400, {{"error", "duplicate_filament"}, {"entry", e}} };
        entries.push_back(std::move(entry));
    }

    ElegooCanvas::Info info;
    nlohmann::json     unsupported;
    if (!read_canvas_slots(true, info, unsupported)) {
        unsupported["error"] = "slots_unavailable";
        try {
            run_on_ui([unsupported]() -> nlohmann::json {
                nlohmann::json e = {{"error", "slots_unavailable"}};
                if (unsupported.contains("message") && unsupported["message"].is_string()) e["detail"] = unsupported["message"];
                mcp_refused_reply(MCP_T("Filament trays"), e, "PUT /printer/slot_mapping");
                return nullptr;
            });
        } catch (const std::exception &) {} // the panel row is not worth failing the reply over
        return { 422, unsupported };
    }

    auto shared = std::make_shared<ElegooCanvas::Info>(std::move(info));
    nlohmann::json r = run_on_ui([shared, entries]() -> nlohmann::json {
        auto refuse = [](nlohmann::json e) -> nlohmann::json {
            nlohmann::json shown = e;
            if (e.contains("entry")) shown["detail"] = e["entry"].dump();
            mcp_refused_reply(MCP_T("Filament trays"), shown, "PUT /printer/slot_mapping");
            return e;
        };
        const ElegooCanvas::Info                         &slots     = *shared;
        // Only the filaments the next Send would map (current plate) are accepted, looked up by project index.
        const std::vector<ElegooCanvas::ProjectFilament>  filaments = canvas_send_filaments();
        std::set<std::pair<int, int>>                     used;
        std::vector<ElegooCanvas::Mapping>                mapping;
        for (const Entry &e : entries) {
            auto fit = std::find_if(filaments.begin(), filaments.end(),
                                    [&e](const ElegooCanvas::ProjectFilament &pf) { return pf.index == e.filament; });
            if (fit == filaments.end())
                return refuse({{"error", "unknown_filament_index"}, {"entry", e.raw}});
            const ElegooCanvas::Tray *t = slots.find(e.canvas_id, e.tray_id);
            if (t == nullptr)
                return refuse({{"error", "unknown_tray"}, {"entry", e.raw}});
            if (!t->ready())
                return refuse({{"error", "tray_not_ready"}, {"entry", e.raw}});
            if (!used.insert({e.canvas_id, e.tray_id}).second)
                return refuse({{"error", "duplicate_tray"}, {"entry", e.raw}});
            const ElegooCanvas::ProjectFilament &f = *fit;
            ElegooCanvas::Mapping                m;
            m.index    = e.filament;
            m.tray     = std::make_pair(e.canvas_id, e.tray_id);
            m.source   = ElegooCanvas::MatchSource::Api;
            m.warnings = elegoo_canvas_mapping_warnings(f, *t, elegoo_canvas_nozzle_temp(f.name));
            mapping.push_back(std::move(m));
        }
        CanvasState &state = CanvasState::get();
        // MCP panel: the trays each filament would use before and after, and the API mapping it replaces.
        const nlohmann::json before_json = canvas_mapping_json(slots);
        const std::string    key_before  = state.project_key;
        nlohmann::json       api_before  = nlohmann::json::array();
        if (state.project_key == elegoo_canvas_project_key())
            for (const ElegooCanvas::Mapping &m : state.api_mapping)
                if (m.tray)
                    api_before.push_back({{"filament", m.index}, {"canvas_id", m.tray->first}, {"tray_id", m.tray->second}});
        state.reset_if_project_changed(elegoo_canvas_project_key());
        state.api_mapping = std::move(mapping);
        api_notify(state.api_mapping.empty() ? "CANVAS slot mapping cleared" : "CANVAS slot mapping set for the next send");
        nlohmann::json after_json = canvas_mapping_json(slots);
        {
            // The mapping is session state (not in the project, so not in a checkpoint either): the group and its
            // checkpoint are opened only when a filament's tray really changed, after the fact.
            std::vector<McpRow> rows;
            auto tray_of = [](const nlohmann::json &row) -> nlohmann::json {
                const nlohmann::json &t = row["tray"];
                if (!t.is_object()) return nullptr;
                return {{"canvas_id", t["canvas_id"]}, {"tray_id", t["tray_id"]}};
            };
            auto label_of = [](const nlohmann::json &row) -> std::string {
                const nlohmann::json &t = row["tray"];
                if (!t.is_object()) return MCP_T("none");
                if (t.contains("label") && t["label"].is_string()) return t["label"].get<std::string>();
                return std::to_string(t["canvas_id"].get<int>()) + "/" + std::to_string(t["tray_id"].get<int>());
            };
            for (const nlohmann::json &a : after_json["filaments"]) {
                const int idx = a["index"].get<int>();
                std::string before_label = MCP_T("none");
                nlohmann::json before_tray = nullptr;
                for (const nlohmann::json &b : before_json["filaments"])
                    if (b["index"].get<int>() == idx) { before_label = label_of(b); before_tray = tray_of(b); }
                const std::string after_label = label_of(a);
                if (after_label == before_label && tray_of(a) == before_tray) continue;
                const std::string label = mcp_fmt(MCP_T("Filament {n} tray"), {{"n", std::to_string(idx + 1)}});
                McpRow row = mcp_row("plate", label, after_label, before_label);
                row.target = {{"type", "slot_mapping"}, {"filament", idx}, {"before_tray", before_tray},
                              {"after_tray", tray_of(a)}, {"api_mapping_before", api_before},
                              {"project_key", key_before}};
                mcp_undo(row, MCP_T("Change back"),
                         mcp_fmt(MCP_T("Change {label} back to {before}"), {{"label", label}, {"before", before_label}}));
                rows.push_back(std::move(row));
            }
            if (!rows.empty()) mcp_before_change();
            for (McpRow &row : rows) mcp_record(std::move(row));
        }
        return after_json;
    }, /*timeout_s=*/120);
    if (r.contains("error"))
        return { 422, r };
    return { 200, r };
}

// --- MCP panel: helpers for McpActions, the activity route and its test hooks -----------------------------------

void Controller::forget_recent(const std::string &path_utf8)
{
    if (path_utf8.empty()) return;
    forget_recent_entries(recent_entries_for(path_from_utf8(path_utf8)));
}

void Controller::slice_state_follows_plate()
{
    if (g_controller != nullptr) g_controller->sync_slice_state_to_current_plate("slice.plate_changed");
}

std::string Controller::move_project_file_back(const std::string &moved_from, const std::string &moved_to)
{
    Plater *plater = wxGetApp().plater();
    const boost::filesystem::path src = path_from_utf8(moved_to);
    const boost::filesystem::path dst = path_from_utf8(moved_from);
    boost::system::error_code ec;
    if (!boost::filesystem::is_regular_file(src, ec)) return "file_not_found";
    if (!dst.parent_path().empty() && !boost::filesystem::is_directory(dst.parent_path(), ec)) return "directory_not_found";
    if (boost::filesystem::exists(dst, ec)) {
        // Only onto a free name; a case-only rename back names the same file and is fine.
        if (!boost::filesystem::equivalent(src, dst, ec)) return "exists";
        if (src.filename() == dst.filename()) return "same_path";
    }
    const bool                     was_open     = is_open_project_file(plater, src);
    const std::vector<std::string> recent       = recent_entries_for(src);
    const std::string              history_from = was_open ? project_path_utf8(plater) : moved_to;
    boost::filesystem::rename(src, dst, ec);
    if (ec) {
        boost::system::error_code cec;
        boost::filesystem::copy_file(src, dst, cec); // fails if the name is taken meanwhile
        if (cec) return "move_failed";
        boost::filesystem::remove(src, cec);
        if (cec) return "move_failed";
    }
    forget_recent_entries(recent);
    if (was_open) {
        plater->set_project_filename(from_path(dst));
        plater->update_title_dirty_status();
    } else if (!recent.empty()) {
        wxGetApp().mainframe->add_to_recent_projects(from_path(dst));
    }
    try {
        McpRecorder::get().project_moved(history_from, was_open ? project_path_utf8(plater) : path_to_utf8(dst));
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "MCP panel: history not moved: " << e.what();
    }
    return {};
}

bool Controller::slot_mapping_label(int filament, std::string &label)
{
    const CanvasState &state = CanvasState::get();
    if (!state.last_info.has_value()) return false;
    const nlohmann::json j = canvas_mapping_json(*state.last_info);
    for (const nlohmann::json &row : j["filaments"]) {
        if (row["index"].get<int>() != filament) continue;
        const nlohmann::json &t = row["tray"];
        if (!t.is_object()) label = "none";
        else if (t.contains("label") && t["label"].is_string()) label = t["label"].get<std::string>();
        else label = std::to_string(t["canvas_id"].get<int>()) + "/" + std::to_string(t["tray_id"].get<int>());
        return true;
    }
    return false;
}

std::string Controller::revert_slot_mapping(const nlohmann::json &target)
{
    CanvasState &state = CanvasState::get();
    // The API mapping belongs to one project (and filament count); another one has none of ours to change.
    if (state.project_key != elegoo_canvas_project_key()) return "project_changed";
    const int filament = target.value("filament", -1);
    if (filament < 0) return "bad_target";
    auto &m = state.api_mapping;
    m.erase(std::remove_if(m.begin(), m.end(), [filament](const ElegooCanvas::Mapping &x) { return x.index == filament; }),
            m.end());
    const nlohmann::json bt = target.value("before_tray", nlohmann::json());
    if (bt.is_object()) {
        // The tray it printed from before, now as an explicit choice (the send dialog shows it pre-filled).
        ElegooCanvas::Mapping back;
        back.index  = filament;
        back.tray   = std::make_pair(bt.value("canvas_id", 0), bt.value("tray_id", 0));
        back.source = ElegooCanvas::MatchSource::Api;
        m.push_back(std::move(back));
        std::sort(m.begin(), m.end(), [](const ElegooCanvas::Mapping &a, const ElegooCanvas::Mapping &b) { return a.index < b.index; });
    }
    return {};
}

// The test hooks of the MCP panel exist only when OrcaSlicer was started with ORCA_MCP_PANEL_TEST=1.
static bool mcp_panel_test_routes()
{
    static const bool on = [] {
        const char *v = std::getenv("ORCA_MCP_PANEL_TEST");
        return v != nullptr && std::string(v) == "1";
    }();
    return on;
}

// Result of the last restore started with {"async": true} (GUI thread only).
static nlohmann::json s_mcp_async_result = nullptr;

// POST /api/v1/activity/test/<name>: each answers what the panel would get, {"status", "reason"?}.
Response Controller::handle_activity_test(const std::string &name, const std::string &body)
{
    nlohmann::json in = body.empty() ? nlohmann::json::object() : nlohmann::json::parse(body);
    if (!in.is_object()) return { 400, {{"error", "body_must_be_object"}} };
    if (name == "clock") {
        if (!in.contains("advance_s") || !in["advance_s"].is_number_integer())
            return { 400, {{"error", "advance_s_must_be_integer"}} };
        McpRecorder::test_set_clock_offset(McpRecorder::test_clock_offset() + in["advance_s"].get<int64_t>());
        return { 200, {{"status", "ok"}, {"offset_s", McpRecorder::test_clock_offset()}} };
    }
    if (name == "checkpoint_delay") {
        if (!in.contains("seconds") || !in["seconds"].is_number_integer() || in["seconds"].get<int>() < 0)
            return { 400, {{"error", "seconds_must_be_integer"}} };
        McpRecorder::test_set_checkpoint_delay(in["seconds"].get<int>());
        return { 200, {{"status", "ok"}} };
    }
    if (name == "undo") {
        if (!in.contains("row") || !in["row"].is_number_integer()) return { 400, {{"error", "row_must_be_integer"}} };
        const int row = in["row"].get<int>();
        return { 200, run_on_ui([row]() -> nlohmann::json {
                     McpActions::TestCallScope scope;
                     return McpActions::undo(row);
                 }, /*timeout_s=*/120) };
    }
    if (name == "restore") {
        if (!in.contains("group") || !in["group"].is_number_integer()) return { 400, {{"error", "group_must_be_integer"}} };
        const int group = in["group"].get<int>();
        if (in.value("async", false)) {
            // As the panel does it: on the GUI thread, outside any API task, while this io thread is free to
            // answer other requests (409 busy while the restore runs). The result: POST .../test/result.
            run_on_ui([]() -> nlohmann::json { s_mcp_async_result = nullptr; return nullptr; });
            wxGetApp().CallAfter([group] { s_mcp_async_result = McpActions::restore(group); });
            return { 202, {{"status", "started"}} };
        }
        return { 200, run_on_ui([group]() -> nlohmann::json {
                     McpActions::TestCallScope scope;
                     return McpActions::restore(group);
                 }, /*timeout_s=*/120) };
    }
    if (name == "undo_restore") {
        return { 200, run_on_ui([]() -> nlohmann::json {
                     McpActions::TestCallScope scope;
                     return McpActions::undo_restore();
                 }, /*timeout_s=*/120) };
    }
    if (name == "result") {
        return { 200, run_on_ui([]() -> nlohmann::json {
                     return s_mcp_async_result.is_null() ? nlohmann::json{{"status", "pending"}} : s_mcp_async_result;
                 }) };
    }
    if (name == "corrupt_point") {
        // Spoils a restore point so a restore fails after OrcaSlicer started replacing the project: {"group"} writes
        // bytes that are no 3MF (OrcaSlicer then shows its load-error dialog), {"group", "from": "<file>"} copies
        // that file over it instead (an empty-geometry .3mf fails silently while an API task runs). The point's
        // fingerprint is taken again, so the restore gets as far as opening it; with {"tamper": true} it is not, and
        // the restore must refuse the changed file.
        if (!in.contains("group") || !in["group"].is_number_integer()) return { 400, {{"error", "group_must_be_integer"}} };
        if (in.contains("tamper") && !in["tamper"].is_boolean()) return { 400, {{"error", "tamper_must_be_bool"}} };
        const int         group  = in["group"].get<int>();
        const std::string from   = in.value("from", std::string());
        const bool        tamper = in.value("tamper", false);
        return { 200, run_on_ui([group, from, tamper]() -> nlohmann::json {
                     McpRecorder &rec = McpRecorder::get();
                     rec.sync_project();
                     const std::string file = rec.history().checkpoint_of(group);
                     if (file.empty()) return {{"status", "failed"}, {"reason", "no checkpoint"}};
                     const boost::filesystem::path dst = path_from_utf8(rec.checkpoint_path(file));
                     boost::system::error_code ec;
                     if (!from.empty()) {
                         if (!boost::filesystem::is_regular_file(path_from_utf8(from), ec))
                             return {{"status", "failed"}, {"reason", "file_not_found"}};
                         boost::filesystem::copy_file(path_from_utf8(from), dst,
                                                      boost::filesystem::copy_options::overwrite_existing, ec);
                         if (ec) return {{"status", "failed"}, {"reason", ec.message()}};
                     } else {
                         std::ofstream out(dst.string(), std::ios::binary | std::ios::trunc);
                         out << "not a 3mf";
                         out.close();
                         if (!out) return {{"status", "failed"}, {"reason", "write failed"}};
                     }
                     if (!tamper) {
                         rec.history().set_fingerprint(group, McpRecorder::fingerprint(rec.checkpoint_path(file)));
                         rec.changed();
                     }
                     return {{"status", "ok"}, {"file", file}, {"tampered", tamper}};
                 }) };
    }
    if (name == "pane") {
        // The dock pane as the page sees it: {"clear": true} starts recording what it sends, {"post": message or
        // [messages]} hands it page messages (an undo/restore runs after this reply, from the app's queue, as a
        // click does). Answers {"open", "messages": [sent since the clear], "posted"?}.
        if (in.contains("clear") && !in["clear"].is_boolean()) return { 400, {{"error", "clear_must_be_bool"}} };
        const bool     clear = in.value("clear", false);
        nlohmann::json posts = nlohmann::json::array();
        if (in.contains("post")) {
            if (in["post"].is_array()) posts = in["post"];
            else posts.push_back(in["post"]);
            for (const auto &m : posts)
                if (!m.is_object()) return { 400, {{"error", "post_must_be_object"}} };
        }
        return { 200, run_on_ui([clear, posts]() -> nlohmann::json {
                     if (clear) McpPanel::test_clear();
                     bool posted = !posts.empty();
                     for (const auto &m : posts) posted = McpPanel::test_post(m) && posted;
                     nlohmann::json out = McpPanel::test_state();
                     if (!posts.empty()) out["posted"] = posted;
                     return out;
                 }) };
    }
    if (name == "as_user") {
        if (!in.contains("changes") || !in["changes"].is_object()) return { 400, {{"error", "changes_must_be_object"}} };
        nlohmann::json changes = in["changes"];
        return { 200, run_on_ui([changes]() -> nlohmann::json {
                     McpActions::TestCallScope scope;
                     return McpActions::apply_as_user(changes);
                 }, /*timeout_s=*/120) };
    }
    return { 404, {{"error", "not_found"}} };
}

// The user-change journal: every request diffs the project against the baseline the previous one left (what
// changed in between was the user's) and leaves a new baseline when it is done (its own changes are the API's).
// Both steps are GUI tasks the io thread posts and does NOT wait for: GUI tasks run in the order they were posted
// (parked ones too, see UiTask::run), so the begin task runs before the handler's tasks and the end task after them
// and after whatever they posted. A stuck GUI thread therefore costs a request its own timeout only, and a request
// that times out cannot leave a stale baseline: its end task still runs, after its handler (cancelled or finished).
static bool post_journal_task(void (*fn)())
{
    try {
        auto task       = std::make_shared<UiTask>();
        task->promise   = std::make_shared<std::promise<nlohmann::json>>(); // nobody waits on it
        task->cancelled = std::make_shared<std::atomic<bool>>(false);
        task->fn        = [fn]() -> nlohmann::json {
            fn();
            return nullptr;
        };
        task->schedule();
        return true;
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(warning) << "MCP journal: task not posted: " << e.what();
    }
    return false;
}

// One begin covers every request that arrives before it ran: while the GUI thread is busy, requests do not pile up a
// full capture each (their handlers and end steps still queue behind it, in order).
static std::atomic<bool> s_journal_begin_pending{false};

// m6: requests between their start and their journal end step; the panel's actions wait for them (busy_reason).
static RequestsInFlight s_requests_in_flight;

bool Controller::requests_in_flight() { return s_requests_in_flight.any(); }

static void journal_end_task()
{
    struct Out { ~Out() { s_requests_in_flight.end(); } } out; // also when the baseline throws
    mcp_journal_end();
}

static void journal_begin_task()
{
    s_journal_begin_pending.store(false); // a request after this point posts its own begin
    mcp_journal_begin();
}

void Controller::journal_begin()
{
    if (s_journal_begin_pending.exchange(true)) return;
    // Not posted: no begin will clear the flag, so the next request posts its own.
    if (!post_journal_task(&journal_begin_task)) s_journal_begin_pending.store(false);
}
void Controller::journal_end()
{
    // Not posted: nothing will count the request out, so do it here.
    if (!post_journal_task(&journal_end_task)) s_requests_in_flight.end();
}

// GET /api/v1/user_changes?since=<seq>: the journal entries newer than seq (see McpJournal::since).
Response Controller::handle_user_changes(const std::string &target)
{
    int64_t     since = 0;
    const auto  q     = target.find('?');
    if (q != std::string::npos) {
        const std::string query = target.substr(q + 1);
        size_t            pos   = 0;
        while (pos <= query.size()) {
            size_t            amp = query.find('&', pos);
            const std::string kv  = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
            if (kv.rfind("since=", 0) == 0) {
                const std::string v = kv.substr(6);
                try {
                    size_t used = 0;
                    since       = std::stoll(v, &used);
                    if (used != v.size()) throw std::invalid_argument("since");
                } catch (const std::exception &) {
                    return { 400, {{"error", "bad_since"}, {"detail", "since must be an integer seq"}} };
                }
            }
            if (amp == std::string::npos) break;
            pos = amp + 1;
        }
    }
    return { 200, run_on_ui([since]() -> nlohmann::json { return McpJournal::get().since(since); }) };
}

// R45: X-Orca-Mcp-Trial is honoured only on settings writes and slicing (what compare_slices sends it on). Any other
// route records as usual with it, so a client cannot hide other changes from the panel.
static bool trial_route(const Request &req)
{
    const std::string path = req.target.substr(0, req.target.find('?'));
    if (req.method == "PUT" && path == "/api/v1/config") return true;
    if (req.method == "POST" && (path == "/api/v1/slice" || path == "/api/v1/slice/cancel")) return true;
    static const std::string obj = "/api/v1/objects/", cfg = "/config";
    return req.method == "PUT" && path.size() > obj.size() + cfg.size() && path.compare(0, obj.size(), obj) == 0 &&
           path.compare(path.size() - cfg.size(), cfg.size(), cfg) == 0 &&
           path.find('/', obj.size()) == path.size() - cfg.size();
}

Response Controller::dispatch(const Request &req)
{
    // The trial flag of this request, for every run_on_ui it makes (M4).
    struct TrialScope {
        bool prev;
        explicit TrialScope(bool t) : prev(t_io_trial) { t_io_trial = t; }
        ~TrialScope() { t_io_trial = prev; }
    } trial_scope(req.trial && trial_route(req));
    // A restore replaces the whole project: nothing, the journal included, may run in between.
    if (McpActions::restoring())
        return { 409, {{"error", "busy"}, {"detail", "the user is restoring"}} };
    const std::string path = req.target.substr(0, req.target.find('?'));
    // GET /slice/status never waits for the GUI thread (wait loops poll it); the next request diffs instead.
    const bool journal = !(req.method == "GET" && path == "/api/v1/slice/status");
    // The panel's test hooks act as the user (a panel action takes its own baseline), and reading the journal
    // changes nothing: neither leaves a baseline.
    const bool leaves_baseline = path.rfind("/api/v1/activity/test/", 0) != 0 && path != "/api/v1/user_changes";
    if (!journal) return dispatch_route(req);
    if (!leaves_baseline) {
        journal_begin();
        return dispatch_route(req);
    }
    // Counted in before its begin is posted, counted out by its end step on the GUI thread (or here if the handler
    // throws past dispatch_route's own handlers: the end step is posted all the same).
    s_requests_in_flight.begin();
    struct End {
        Controller *c;
        ~End() { c->journal_end(); }
    } end{this};
    journal_begin();
    return dispatch_route(req);
}

Response Controller::dispatch_route(const Request &req)
{
    try {
        const std::string &t = req.target;
        // Match method + exact path, allowing only a query string after it
        // (so /api/v1/statuses does NOT prefix-match /api/v1/status).
        auto is = [&](const char *m, const char *path) {
            if (req.method != m) return false;
            size_t n = std::char_traits<char>::length(path);
            return t.compare(0, n, path) == 0 && (t.size() == n || t[n] == '?');
        };
        // MCP panel: a restore replaces the whole project; nothing the API does may run in between.
        if (McpActions::restoring())
            return { 409, {{"error", "busy"}, {"detail", "the user is restoring"}} };
        if (is("GET", "/api/v1/status"))        return handle_status();
        if (is("GET", "/api/v1/user_changes"))  return handle_user_changes(t);
        // The MCP panel's timeline, in the shape the panel gets (read-only: records nothing).
        if (is("GET", "/api/v1/activity"))
            return { 200, run_on_ui([]() -> nlohmann::json { return McpActions::activity(); }) };
        // M4 / m12: one row the connector adds itself (a comparison's summary, its own refusals).
        if (is("POST", "/api/v1/activity/note")) {
            nlohmann::json in;
            try {
                in = nlohmann::json::parse(req.body);
            } catch (const nlohmann::json::parse_error &) {
                return { 400, {{"error", "invalid_json"}} };
            }
            if (!in.is_object()) return { 400, {{"error", "body_must_be_object"}} };
            const std::string kind = in.value("kind", std::string());
            if (kind != "slice" && kind != "refused" && kind != "note")
                return { 400, {{"error", "bad_kind"}, {"detail", "kind is slice, refused or note"}} };
            auto text_of = [&in](const char *k, size_t max) -> std::string {
                auto it = in.find(k);
                if (it == in.end() || !it->is_string()) return {};
                // Control and format characters out (Cc, and the Cf marks that reorder or hide text: bidi
                // overrides and isolates, zero-width joiners, the BOM); line breaks and tabs become spaces.
                const std::string raw = it->get<std::string>();
                std::string       t;
                for (size_t i = 0; i < raw.size();) {
                    const unsigned char c = static_cast<unsigned char>(raw[i]);
                    if (c == '\n' || c == '\r' || c == '\t') { t += ' '; ++i; continue; }
                    if (c < 0x20 || c == 0x7F) { ++i; continue; }
                    if (c == 0xC2 && i + 1 < raw.size() && static_cast<unsigned char>(raw[i + 1]) >= 0x80 &&
                        static_cast<unsigned char>(raw[i + 1]) <= 0x9F) { i += 2; continue; } // C1 controls
                    if (c == 0xC2 && i + 1 < raw.size() && static_cast<unsigned char>(raw[i + 1]) == 0xAD) { i += 2; continue; } // soft hyphen
                    if (c == 0xE2 && i + 2 < raw.size()) {
                        const unsigned char b1 = static_cast<unsigned char>(raw[i + 1]), b2 = static_cast<unsigned char>(raw[i + 2]);
                        // U+200B..U+200F, U+202A..U+202E, U+2060..U+2064, U+2066..U+206F
                        if ((b1 == 0x80 && ((b2 >= 0x8B && b2 <= 0x8F) || (b2 >= 0xAA && b2 <= 0xAE))) ||
                            (b1 == 0x81 && b2 >= 0xA0 && b2 <= 0xAF && b2 != 0xA5)) { i += 3; continue; }
                    }
                    if (c == 0xEF && i + 2 < raw.size() && static_cast<unsigned char>(raw[i + 1]) == 0xBB &&
                        static_cast<unsigned char>(raw[i + 2]) == 0xBF) { i += 3; continue; } // U+FEFF
                    t += raw[i++];
                }
                boost::algorithm::trim(t);
                if (t.size() > max) {
                    size_t cut = max;
                    while (cut > 0 && (static_cast<unsigned char>(t[cut]) & 0xC0) == 0x80) --cut; // UTF-8 boundary
                    t = t.substr(0, cut);
                }
                return t;
            };
            const std::string text = text_of("text", 160), value = text_of("value", 80), detail = text_of("detail", 400);
            if (text.empty()) return { 400, {{"error", "missing_text"}} };
            run_on_ui([kind, text, value, detail]() -> nlohmann::json {
                try {
                    McpRecorder::get().record_note(kind, text, value, detail);
                } catch (const std::exception &e) {
                    BOOST_LOG_TRIVIAL(error) << "MCP panel: note not recorded: " << e.what();
                }
                return nullptr;
            });
            return { 200, {{"recorded", true}} };
        }
        {
            static const std::string tpfx = "/api/v1/activity/test/";
            if (req.method == "POST" && t.compare(0, tpfx.size(), tpfx) == 0) {
                if (!mcp_panel_test_routes()) return { 404, {{"error", "not_found"}} };
                try {
                    return handle_activity_test(t.substr(0, t.find('?')).substr(tpfx.size()), req.body);
                } catch (const nlohmann::json::parse_error &) {
                    return { 400, {{"error", "invalid_json"}} };
                }
            }
        }
        if (is("GET", "/api/v1/config"))        return handle_get_config(t);
        if (is("PUT", "/api/v1/config")) {
            try {
                return handle_put_config(req.body);
            } catch (const nlohmann::json::parse_error &) {
                return { 400, {{"error", "invalid_json"}} };
            }
        }
        if (is("POST", "/api/v1/slice"))        return handle_slice();
        if (is("GET",  "/api/v1/slice/status")) return handle_slice_status();
        if (is("POST", "/api/v1/slice/cancel")) return handle_slice_cancel();
        if (is("POST", "/api/v1/model")) {
            try {
                return handle_load_model(req.body);
            } catch (const nlohmann::json::parse_error &) {
                return { 400, {{"error", "invalid_json"}} };
            }
        }
        if (is("PUT", "/api/v1/preset")) {
            try {
                return handle_select_preset(req.body);
            } catch (const nlohmann::json::parse_error &) {
                return { 400, {{"error", "invalid_json"}} };
            }
        }
        if (is("POST", "/api/v1/preset/save")) {
            try {
                return handle_save_preset(req.body);
            } catch (const nlohmann::json::parse_error &) {
                return { 400, {{"error", "invalid_json"}} };
            }
        }
        if (is("POST", "/api/v1/preset/config")) {
            try {
                return handle_get_preset_config(req.body);
            } catch (const nlohmann::json::parse_error &) {
                return { 400, {{"error", "invalid_json"}} };
            }
        }
        if (is("DELETE", "/api/v1/preset")) {
            try {
                return handle_delete_preset(req.body);
            } catch (const nlohmann::json::parse_error &) {
                return { 400, {{"error", "invalid_json"}} };
            }
        }
        if (is("GET",  "/api/v1/gcode"))        return handle_get_gcode();
        if (is("GET",  "/api/v1/objects"))      return handle_get_objects();
        if (is("GET",  "/api/v1/plate/render")) return handle_plate_render(t);
        if (is("GET",  "/api/v1/presets"))      return handle_get_presets();
        if (is("POST", "/api/v1/arrange"))      return handle_arrange();
        if (is("POST", "/api/v1/orient"))       return handle_orient();
        if (is("GET",  "/api/v1/jobs/status"))  return handle_jobs_status();
        if (is("GET",  "/api/v1/plates"))       return handle_get_plates();
        // project files
        if (is("GET",  "/api/v1/project"))        return handle_get_project();
        if (is("GET",  "/api/v1/project/recent")) return handle_get_recent_projects();
        if (is("GET",  "/api/v1/project/browse")) return handle_browse_projects(t);
        if (is("POST", "/api/v1/project/save")) {
            try { return handle_save_project(req.body); }
            catch (const nlohmann::json::parse_error &) { return { 400, {{"error", "invalid_json"}} }; }
        }
        if (is("POST", "/api/v1/project/open")) {
            try { return handle_open_project(req.body); }
            catch (const nlohmann::json::parse_error &) { return { 400, {{"error", "invalid_json"}} }; }
        }
        if (is("POST", "/api/v1/project/new")) {
            try { return handle_new_project(req.body); }
            catch (const nlohmann::json::parse_error &) { return { 400, {{"error", "invalid_json"}} }; }
        }
        if (is("DELETE", "/api/v1/project/recent")) {
            try { return handle_forget_recent_project(req.body); }
            catch (const nlohmann::json::parse_error &) { return { 400, {{"error", "invalid_json"}} }; }
        }
        if (is("POST", "/api/v1/project/move")) {
            try { return handle_move_project_file(req.body); }
            catch (const nlohmann::json::parse_error &) { return { 400, {{"error", "invalid_json"}} }; }
        }
        if (is("DELETE", "/api/v1/project/file")) {
            try { return handle_delete_project_file(req.body); }
            catch (const nlohmann::json::parse_error &) { return { 400, {{"error", "invalid_json"}} }; }
        }
        if (is("GET",  "/api/v1/printer/slots"))        return handle_printer_slots();
        if (is("GET",  "/api/v1/printer/slot_mapping")) return handle_get_slot_mapping();
        if (is("PUT",  "/api/v1/printer/slot_mapping")) {
            try { return handle_put_slot_mapping(req.body); }
            catch (const nlohmann::json::parse_error &) { return { 400, {{"error", "invalid_json"}} }; }
        }
        if (is("POST", "/api/v1/plates")) {
            try { return handle_create_plate(req.body); }
            catch (const nlohmann::json::parse_error &) { return { 400, {{"error", "invalid_json"}} }; }
        }
        {
            // plate sub-routes: /api/v1/plates/<index> and /api/v1/plates/<index>/<action>
            // ("plates/", distinct from the GET /api/v1/plate/render route above)
            static const std::string pfx = "/api/v1/plates/";
            std::string path = t.substr(0, t.find('?'));
            if (path.size() > pfx.size() && path.compare(0, pfx.size(), pfx) == 0) {
                std::string rest = path.substr(pfx.size());
                size_t slash = rest.find('/');
                std::string idx_str = (slash == std::string::npos) ? rest : rest.substr(0, slash);
                std::string action  = (slash == std::string::npos) ? std::string() : rest.substr(slash + 1);
                int idx = -1;
                try {
                    size_t used = 0;
                    idx = std::stoi(idx_str, &used);
                    if (used != idx_str.size() || idx < 0) idx = -1;
                } catch (...) { idx = -1; }
                if (idx < 0) return { 400, {{"error", "bad_plate_index"}} };
                if (req.method == "DELETE" && action.empty())  return handle_delete_plate(idx);
                if (req.method == "POST"   && action == "select") return handle_select_plate(idx);
                return { 404, {{"error", "not_found"}} };
            }
        }
        {
            // M4b object sub-routes: /api/v1/objects/<id> and /api/v1/objects/<id>/<action>
            static const std::string pfx = "/api/v1/objects/";
            std::string path = t.substr(0, t.find('?'));
            if (path.size() > pfx.size() && path.compare(0, pfx.size(), pfx) == 0) {
                std::string rest = path.substr(pfx.size());
                size_t slash = rest.find('/');
                std::string id_str = (slash == std::string::npos) ? rest : rest.substr(0, slash);
                std::string action = (slash == std::string::npos) ? std::string() : rest.substr(slash + 1);
                uint64_t oid = 0;
                try { oid = std::stoull(id_str); }
                catch (...) { return { 400, {{"error", "bad_object_id"}} }; }
                if (req.method == "DELETE" && action.empty())
                    return handle_delete_object(oid);
                if (req.method == "POST" && action == "transform") {
                    try { return handle_transform_object(oid, req.body); }
                    catch (const nlohmann::json::parse_error &) { return { 400, {{"error", "invalid_json"}} }; }
                }
                if (req.method == "POST" && action == "duplicate")
                    return handle_duplicate_object(oid);
                if (req.method == "POST" && action == "plate") {
                    try { return handle_put_object_plate(oid, req.body); }
                    catch (const nlohmann::json::parse_error &) { return { 400, {{"error", "invalid_json"}} }; }
                }
                if (req.method == "PUT" && action == "config") {
                    try { return handle_put_object_config(oid, req.body); }
                    catch (const nlohmann::json::parse_error &) { return { 400, {{"error", "invalid_json"}} }; }
                }
                if (req.method == "PUT" && action == "layer_height") {
                    try { return handle_put_layer_height(oid, req.body); }
                    catch (const nlohmann::json::parse_error &) { return { 400, {{"error", "invalid_json"}} }; }
                }
                if (req.method == "PUT" && action == "height_range") {
                    try { return handle_put_height_range(oid, req.body); }
                    catch (const nlohmann::json::parse_error &) { return { 400, {{"error", "invalid_json"}} }; }
                }
                return { 404, {{"error", "not_found"}} };
            }
        }
        return { 404, {{"error", "not_found"}} };
    } catch (const std::exception &e) {
        if (std::string(e.what()) == "ui_timeout")
            return { 504, {{"error", "ui_timeout"}} };
        return { 500, {{"error", "internal"}, {"detail", e.what()}} };
    }
}

}}} // namespace
