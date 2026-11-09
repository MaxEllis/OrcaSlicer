#include "McpActions.hpp"
#include "McpJournal.hpp"  // the user-change journal: panel actions are the user's
#include "McpRecorder.hpp"
#include "RemoteAPIController.hpp" // the shared apply paths, the F8 gate, open_project_from
#include "RemoteAPIServer.hpp"     // broadcast

#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Slicing.hpp" // t_layer_height_range
#include "libslic3r/Utils.hpp"   // ScopeGuard
#include "slic3r/GUI/GUI.hpp"    // from_u8 / into_path
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/ParamsPanel.hpp"   // switch_to_global (reveal)
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/SettingsIndex.hpp" // the settings sidebar's option index (reveal)
#include "slic3r/GUI/Tab.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace RemoteAPI {

namespace fs = boost::filesystem;
using nlohmann::json;

std::atomic<bool> McpActions::s_restoring{false};

namespace {

int s_test_depth = 0; // TestCallScope nesting

// The last restore of the open project: the "Restored to" line and its Undo. In memory only (a restart forgets
// it, like the stashed copies behind "Put back").
struct RestoreState
{
    std::string                key;              // McpRecorder::history_dir() of the project it belongs to
    unsigned                   epoch = 0;        // McpRecorder::history_epoch() when it was made
    int                        restored_to = -1; // -1: none
    int                        newest_at   = -1; // id of the newest history group right after it; a new group ends it
    std::string                snapshot;         // pre-restore checkpoint file name ("" = no Undo)
    McpFingerprint             snapshot_fp;      // what that file held when it was written
    std::map<int, std::string> marks_before;     // row id -> undone_at before the restore
    std::set<int>              marked_before;    // s_marked before the restore
    int                        prev_restored_to = -1;
    bool                       failed           = false; // the open failed (the project may be half replaced)
};
RestoreState  s_restore;
// Rows a restore marked undone, so a later restore to a later point can unmark them. Per project.
std::set<int> s_marked;
std::string   s_marked_key;
unsigned      s_marked_epoch = 0;

const char *const kMovedSince   = "moved since";
const char *const kChangedSince = "changed since";
const char *const kRemovedSince = "removed since";
const char *const kArrow        = "\xE2\x86\x92"; // U+2192, as the move-to-plate rows write it
const char *const kUntracked    = "Can't track this object after the project was reloaded";
const char *const kPointChanged = "The restore point was changed on disk, so it is not used";
constexpr int     kRevealBlinkSteps = 22; // Tab's highlight: 11 steps of 300 ms for a search hit

struct Live
{
    bool        known   = false; // false: no live value for this row (none is sent)
    bool        present = true;
    std::string value;
};

Live live_value(std::string v)
{
    Live l;
    l.known = true;
    l.value = std::move(v);
    return l;
}

Live live_gone()
{
    Live l;
    l.known   = true;
    l.present = false;
    return l;
}

json result_ok() { return {{"status", "ok"}}; }
json result_failed(const std::string &why) { return {{"status", "failed"}, {"reason", why}}; }
json result_cancelled(const std::string &why) { return {{"status", "cancelled"}, {"reason", why}}; }

Plater *plater() { return wxGetApp().plater(); }

// The page's sameValue: numbers (with the same unit text) within 1e-6 relative, anything else exactly.
bool parse_num(const std::string &s, double &n, std::string &unit)
{
    static const std::regex re(R"(^\s*([-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)\s*(.*?)\s*$)");
    std::smatch m;
    if (!std::regex_match(s, m, re)) return false;
    try {
        n = std::stod(m[1].str());
    } catch (const std::exception &) {
        return false;
    }
    if (!std::isfinite(n)) return false;
    unit = m[2].str();
    return true;
}

bool same_text(const std::string &a, const std::string &b)
{
    double      x = 0, y = 0;
    std::string ux, uy;
    if (parse_num(a, x, ux) && parse_num(b, y, uy)) {
        if (ux != uy) return false;
        return std::abs(x - y) <= 1e-6 * std::max({std::abs(x), std::abs(y), 1e-12});
    }
    return a == b;
}

// Per matrix element, 1e-6 relative (absolute below 1), as the arrange/orient rows were judged when recorded.
bool same_matrix(const json &a, const json &b)
{
    if (!a.is_array() || !b.is_array() || a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!a[i].is_number() || !b[i].is_number()) return false;
        const double x = a[i].get<double>(), y = b[i].get<double>();
        if (std::abs(x - y) > 1e-6 * std::max({std::abs(x), std::abs(y), 1.0})) return false;
    }
    return true;
}

Transform3d matrix_from(const json &a)
{
    Transform3d t = Transform3d::Identity();
    if (a.is_array() && a.size() == 16)
        for (int i = 0; i < 16; ++i) t.matrix().data()[i] = a[size_t(i)].get<double>();
    return t;
}

// The model's objects with their ids, names and instance ids, in model order (indices are object indices).
std::vector<McpObjectRef> object_refs()
{
    std::vector<McpObjectRef> out;
    for (const ModelObject *mo : plater()->model().objects) {
        McpObjectRef o;
        o.id   = (uint64_t) mo->id().id;
        o.name = mo->name;
        for (const ModelInstance *mi : mo->instances) o.instance_ids.push_back((uint64_t) mi->id().id);
        out.push_back(std::move(o));
    }
    return out;
}

// Was the row's target recorded in this project load (else its ObjectIDs are stale and the object is found by name)?
bool target_current(const json &t) { return mcp_target_current(t, McpRecorder::get().load_stamp()); }

PresetCollection *collection(const std::string &scope)
{
    auto *b = wxGetApp().preset_bundle;
    if (scope == "print") return &b->prints;
    if (scope == "filament") return &b->filaments;
    if (scope == "printer") return &b->printers;
    return nullptr;
}

Preset::Type preset_type(const std::string &scope)
{
    return scope == "print" ? Preset::TYPE_PRINT : scope == "filament" ? Preset::TYPE_FILAMENT : Preset::TYPE_PRINTER;
}

json config_map(const ModelConfig &c)
{
    json m = json::object();
    for (const std::string &k : c.keys()) m[k] = c.opt_serialize(k);
    return m;
}

// A {key: raw} map as a config; throws on a value OrcaSlicer rejects.
DynamicPrintConfig config_from_map(const json &m)
{
    DynamicPrintConfig c;
    if (m.is_object())
        for (auto it = m.begin(); it != m.end(); ++it)
            if (it.value().is_string()) c.set_deserialize_strict(it.key(), it.value().get<std::string>());
    return c;
}

std::string before_sep(const std::string &s, const std::string &sep)
{
    const size_t p = s.find(sep);
    return p == std::string::npos ? s : s.substr(0, p);
}

// The object a row is about, for "<name> is no longer on the plate" and the journal line.
std::string object_name_of(const McpRow &r)
{
    const std::string t = r.target.value("type", std::string());
    if (t == "added" || t == "removed" || t == "layer_profile") return r.value;
    if (t == "transform" || t == "height_range" || t == "height_ranges_cleared") return r.where;
    if (t == "object_config") return before_sep(r.where, " only");
    if (t == "duplicate") return before_sep(r.value, ", now ");
    if (t == "instance_plate") return before_sep(r.value, std::string(" ") + kArrow);
    if (t == "instances") return r.where.empty() ? std::string("An object") : r.where;
    return !r.where.empty() ? r.where : (!r.value.empty() ? r.value : r.label);
}

// ---- live values ------------------------------------------------------------------------------------------------

// Current index of the plate a POST /plates row created: -1 when it is gone, -2 when it is not known (another app
// run: the recorder keeps the plate's ObjectID in memory only).
int created_plate_now(int row_id)
{
    PartPlateList        &plates = plater()->get_partplate_list();
    std::vector<uint64_t> ids;
    for (int i = 0; i < plates.get_plate_count(); ++i) {
        PartPlate *pl = plates.get_plate(i);
        ids.push_back(pl != nullptr ? (uint64_t) pl->id().id : 0);
    }
    if (!McpRecorder::get().knows_created_plate(row_id)) return -2;
    return McpRecorder::get().created_plate_index(row_id, ids);
}

std::string project_element_raw(const DynamicPrintConfig &proj, const std::string &key, const json &index, bool &ok)
{
    ok = true;
    if (index.is_number_integer()) {
        const size_t i = index.get<size_t>();
        if (key == "filament_colour") {
            const auto *o = proj.option<ConfigOptionStrings>(key);
            if (o == nullptr || i >= o->values.size()) { ok = false; return {}; }
            return o->values[i];
        }
        const auto *o = proj.option<ConfigOptionFloats>(key);
        if (o == nullptr || o->values.empty()) { ok = false; return {}; }
        return ConfigOptionFloat(o->get_at(i)).serialize();
    }
    if (!proj.has(key)) { ok = false; return {}; }
    return proj.opt_serialize(key);
}

// A setting row's live value from the setting's raw now. A multi-line row (target "multiline": a G-code template) is
// judged on the raws, as its display form depends on both sides of the change: the row's own value or before when the
// raw is one of them, else the raw's form against the assistant's value ("changed since" when that reads like either).
std::string config_live_text(const McpRow &r, const std::string &key, const std::string &raw)
{
    const json &t = r.target;
    if (!t.is_object() || !t.value("multiline", false)) return McpRecorder::config_value_text(key, raw);
    const std::string after_raw = t.value("after_raw", std::string());
    if (raw == after_raw) return r.value;
    if (raw == t.value("before_raw", std::string())) return r.before;
    std::string now, after;
    std::string shown;
    if (Controller::config_text(key, raw, now)) {
        const bool has_after = Controller::config_text(key, after_raw, after);
        shown = mcp_multiline_display(now, has_after ? &after : nullptr);
    }
    if (shown.empty()) shown = McpRecorder::config_value_text(key, raw);
    if (same_text(shown, r.value) || same_text(shown, r.before)) shown = kChangedSince;
    return shown;
}

Live live_of(const McpRow &r)
{
    const json       &t    = r.target;
    const std::string type = t.is_object() ? t.value("type", std::string()) : std::string();
    Plater           *p    = plater();
    if (p == nullptr || type.empty()) return {};
    auto *bundle = wxGetApp().preset_bundle;

    if (type == "config") {
        if (!t.contains("after_raw")) return {}; // a credential: never read back
        const std::string key   = t.value("key", std::string());
        const std::string scope = t.value("scope", std::string());
        if (scope == "project") {
            bool              ok  = true;
            const std::string raw = project_element_raw(bundle->project_config, key, t.value("index", json()), ok);
            if (!ok) return live_value(kRemovedSince);
            return live_value(config_live_text(r, key, raw));
        }
        PresetCollection *c = collection(scope);
        if (c == nullptr) return {};
        const ConfigOption *opt  = c->get_edited_preset().config.option(key);
        std::string         text = opt != nullptr ? config_live_text(r, key, opt->serialize()) : std::string("not set");
        // Another preset is being edited now: the assistant's value is not the one shown any more.
        const std::string sel = c->get_selected_preset_name();
        if (t.contains("preset") && t["preset"].is_string() && t["preset"].get<std::string>() != sel) text += " in " + sel;
        return live_value(text);
    }

    if (type == "slot_mapping") {
        std::string label;
        if (!Controller::slot_mapping_label(t.value("filament", -1), label)) return {};
        return live_value(label);
    }

    if (type == "preset") {
        const json slot = t.value("slot", json());
        if (slot.is_number_integer()) {
            const size_t s = slot.get<size_t>();
            if (s >= bundle->filament_presets.size()) return live_value(kRemovedSince);
            return live_value(bundle->filament_presets[s]);
        }
        PresetCollection *c = collection(t.value("preset_type", std::string()));
        if (c == nullptr) return {};
        return live_value(c->get_selected_preset_name());
    }

    if (type == "preset_saved") {
        PresetCollection *c = collection(t.value("preset_type", std::string()));
        if (c == nullptr) return {};
        return live_value(c->find_preset(t.value("name", std::string()), false) != nullptr ? r.value : std::string("deleted since"));
    }

    if (type == "plate_added") {
        PartPlateList &plates = p->get_partplate_list();
        const int      idx    = created_plate_now(r.id);
        if (idx == -2) return {}; // not known in this app run: the undo says so on click
        if (idx < 0) return live_value("deleted since");
        PartPlate *plate = plates.get_plate(idx);
        return live_value(plate != nullptr && plate->empty() ? r.value : std::string("has objects now"));
    }

    if (type == "file_moved") {
        boost::system::error_code ec;
        const fs::path from = into_path(from_u8(t.value("from", std::string())));
        const fs::path to   = into_path(from_u8(t.value("to", std::string())));
        if (!fs::is_regular_file(to, ec)) return live_value(kMovedSince);
        if (fs::exists(from, ec) && !fs::equivalent(from, to, ec)) return live_value("old name taken");
        return live_value(r.value);
    }

    // Object rows. After a project load the ids are new: found by name, else no live value (the undo says why).
    if (type == "removed") return live_value(r.value); // its availability is decided by the stash, on click
    const bool                      current = target_current(t);
    const std::vector<McpObjectRef> refs    = object_refs();
    if (type == "instances") {
        const json items = t.value("items", json::array());
        for (const json &it : items) {
            const int oi = mcp_find_object(it, current, std::string(), refs);
            if (oi == -2) return {};
            if (oi < 0) return live_gone();
            const ModelObject *mo = p->model().objects[oi];
            const int          ii = mcp_find_instance(it, current, refs[size_t(oi)].instance_ids);
            if (ii < 0) return live_value(kMovedSince);
            if (!same_matrix(McpRecorder::instance_matrix(*mo->instances[ii]), it.value("after", json()))) return live_value(kMovedSince);
        }
        return live_value(r.value);
    }
    if (!t.contains("object_id")) return {};
    const int oi = mcp_find_object(t, current, object_name_of(r), refs);
    if (oi == -2) return {};
    if (oi < 0) return live_gone();
    const ModelObject *mo = p->model().objects[oi];

    if (type == "added") return live_value(r.value);
    if (type == "object_config") {
        const std::string key = t.value("key", std::string());
        std::string       raw;
        if (mo->config.has(key)) raw = mo->config.opt_serialize(key);
        else {
            const DynamicPrintConfig merged = bundle->full_config();
            raw = merged.has(key) ? merged.opt_serialize(key) : std::string();
        }
        return live_value(config_live_text(r, key, raw));
    }
    if (type == "layer_profile") {
        const std::vector<double> now   = mo->layer_height_profile.get();
        const json                after = t.value("after", json::array());
        bool same = after.is_array() && after.size() == now.size();
        for (size_t i = 0; same && i < now.size(); ++i) {
            const double y = after[i].is_number() ? after[i].get<double>() : NAN;
            same = std::abs(now[i] - y) <= 1e-6 * std::max({std::abs(now[i]), std::abs(y), 1.0});
        }
        return live_value(same ? r.value : std::string(kChangedSince));
    }
    if (type == "height_range") {
        const t_layer_height_range key{t.value("min_z", 0.0), t.value("max_z", 0.0)};
        auto it = mo->layer_config_ranges.find(key);
        if (it == mo->layer_config_ranges.end()) return live_value(kRemovedSince);
        return live_value(config_map(it->second) == t.value("after", json()) ? r.value : std::string(kChangedSince));
    }
    if (type == "height_ranges_cleared")
        return live_value(mo->layer_config_ranges.empty() ? r.value : std::string("ranges set since"));
    if (type == "transform" || type == "duplicate" || type == "instance_plate") {
        const int ii = mcp_find_instance(t, current, refs[size_t(oi)].instance_ids);
        if (ii < 0) return live_value(type == "duplicate" ? "copy removed since" : kMovedSince);
        return live_value(same_matrix(McpRecorder::instance_matrix(*mo->instances[ii]), t.value("after", json())) ? r.value
                                                                                                            : std::string(kMovedSince));
    }
    return {};
}

// ---- the undo of each kind ---------------------------------------------------------------------------------------

// Drops every collection's unsaved edits, as PUT /preset does before a switch (Tab::select_preset would otherwise
// open its unsaved-changes dialog). Returns the groups dropped.
std::vector<std::string> discard_edits()
{
    auto                    *b = wxGetApp().preset_bundle;
    std::vector<std::string> out;
    auto one = [&out](PresetCollection &c, const char *g) {
        if (c.current_is_dirty()) {
            c.discard_current_changes();
            out.push_back(g);
        }
    };
    one(b->prints, "print");
    one(b->filaments, "filament");
    one(b->sla_materials, "sla_material");
    one(b->printers, "printer");
    return out;
}

// Writes {key: raw} into the edited preset of each collection holding the key, through the GUI's edit path
// (Tab::load_config + Plater::on_config_change, as PUT /config). "" or the first error.
std::string apply_preset_edits(const json &changes)
{
    auto *b = wxGetApp().preset_bundle;
    struct T { Preset::Type type; DynamicPrintConfig cfg; bool touched; };
    std::vector<T> ts{{Preset::TYPE_PRINT, b->prints.get_edited_preset().config, false},
                      {Preset::TYPE_FILAMENT, b->filaments.get_edited_preset().config, false},
                      {Preset::TYPE_PRINTER, b->printers.get_edited_preset().config, false}};
    for (auto it = changes.begin(); it != changes.end(); ++it) {
        T *tgt = nullptr;
        for (T &x : ts)
            if (x.cfg.option(it.key()) != nullptr) { tgt = &x; break; }
        if (tgt == nullptr) return it.key() + ": not a print, filament or printer setting";
        // A copy of the value there now, for the range check (set_deserialize_strict replaces it).
        std::unique_ptr<ConfigOption> was(tgt->cfg.option(it.key())->clone());
        std::string raw;
        const json &v = it.value();
        if (v.is_string()) raw = v.get<std::string>();
        else if (v.is_boolean()) raw = v.get<bool>() ? "1" : "0";
        else if (v.is_number()) raw = v.dump();
        else return it.key() + ": unsupported value";
        try {
            tgt->cfg.set_deserialize_strict(it.key(), raw);
        } catch (const std::exception &e) {
            return it.key() + ": " + e.what();
        }
        json        rd;
        std::string rtext;
        if (!Controller::config_value_in_range(it.key(), was.get(), *tgt->cfg.option(it.key()), rd, rtext)) return rtext;
        tgt->touched = true;
    }
    for (T &x : ts) {
        if (!x.touched) continue;
        Tab *tab = wxGetApp().get_tab(x.type);
        if (tab == nullptr) return "the settings tab is not ready";
        tab->load_config(x.cfg);
        plater()->on_config_change(x.cfg);
    }
    return {};
}

// After an object's instances changed: plates follow the geometry, the scene and the object list refresh.
void instances_changed(int oi, const std::vector<int> &instances)
{
    Plater        *p      = plater();
    PartPlateList &plates = p->get_partplate_list();
    for (int ii : instances) plates.notify_instance_update(oi, ii, /*is_new=*/true);
    p->changed_object(oi);
    wxGetApp().obj_list()->update_plate_values_for_items();
}

void set_matrix(ModelObject *mo, int ii, const json &m)
{
    mo->instances[ii]->set_transformation(Geometry::Transformation(matrix_from(m)));
    mo->invalidate_bounding_box();
}

// The object list's filament column, as PUT /objects/{id}/config refreshes it after an extruder change.
void refresh_extruder_column(int oi, const ModelObject *mo)
{
    ObjectList *obj_list = wxGetApp().obj_list();
    ObjectDataViewModel *om = obj_list == nullptr ? nullptr : obj_list->GetModel();
    if (om == nullptr) return;
    wxDataViewItem item = om->GetItemById(oi);
    if (!item.IsOk()) return;
    const int ext = mo->config.has("extruder") ? mo->config.extruder() : 0;
    om->SetExtruder(ext == 0 ? _L("default") : wxString::Format("%d", ext), item);
    obj_list->Refresh();
}

// The collections a preset switch back in undo_row() discarded (print, filament, sla_material, printer), for the
// row's "reverted" record (McpHistory.hpp mcp_tracking).
std::vector<std::string> s_undo_dropped;

// Returns "" (done), or the reason it failed; cancelled set when the user cancelled a dialog. note: added to the
// journal line (the unsaved edits a switch back dropped).
std::string undo_row(const McpRow &r, bool &cancelled, std::string &note)
{
    cancelled = false;
    note.clear();
    s_undo_dropped.clear();
    const json       &t    = r.target;
    const std::string type = t.value("type", std::string());
    Plater           *p    = plater();
    auto             *bundle = wxGetApp().preset_bundle;
    const std::string gone   = object_name_of(r) + " is no longer on the plate";

    if (type == "config") {
        const std::string key    = t.value("key", std::string());
        const std::string scope  = t.value("scope", std::string());
        if (!t.contains("before_raw") || !t["before_raw"].is_string()) return "Can't set it back";
        const std::string before = t["before_raw"].get<std::string>();
        if (scope == "project") {
            DynamicPrintConfig             proj = bundle->project_config;
            const auto                    *c0   = proj.option<ConfigOptionStrings>("filament_colour");
            const std::vector<std::string> colours_before = c0 == nullptr ? std::vector<std::string>{} : c0->values;
            const json                     index = t.value("index", json());
            try {
                if (index.is_number_integer() && key == "filament_colour") {
                    auto *o = proj.option<ConfigOptionStrings>(key);
                    const size_t i = index.get<size_t>();
                    if (o == nullptr || i >= o->values.size()) return "That filament is gone";
                    o->values[i] = before;
                } else if (index.is_number_integer()) {
                    ConfigOptionFloat one;
                    if (!one.deserialize(before)) return "Can't set it back";
                    proj.option<ConfigOptionFloats>(key, true)->set_at(&one, index.get<size_t>(), 0);
                } else {
                    proj.set_deserialize_strict(key, before);
                }
            } catch (const std::exception &) {
                return "Can't set it back";
            }
            Controller::apply_project_config(proj, {key}, colours_before);
            return {};
        }
        PresetCollection *c = collection(scope);
        if (c == nullptr) return "Can't set it back";
        DynamicPrintConfig cfg = c->get_edited_preset().config;
        try {
            cfg.set_deserialize_strict(key, before);
        } catch (const std::exception &) {
            return "Can't set it back";
        }
        {
            // Never put back a value OrcaSlicer's own fields would refuse.
            const ConfigOption *now = c->get_edited_preset().config.option(key);
            json                rd;
            std::string         rtext;
            if (cfg.option(key) != nullptr && !Controller::config_value_in_range(key, now, *cfg.option(key), rd, rtext))
                return "Can't set it back: " + rtext;
        }
        Tab *tab = wxGetApp().get_tab(preset_type(scope));
        if (tab == nullptr) return "The settings tab is not ready";
        tab->load_config(cfg);
        p->on_config_change(cfg);
        return {};
    }

    if (type == "preset") {
        const std::string before = t.value("before", std::string());
        const json        slot   = t.value("slot", json());
        Sidebar          &sidebar = p->sidebar();
        if (slot.is_number_integer() && sidebar.is_multifilament()) {
            const size_t s = slot.get<size_t>();
            if (s >= bundle->filament_presets.size()) return "That filament is gone";
            const Preset *pr = bundle->filaments.find_preset(before, false, true);
            if (pr == nullptr) return before + " is no longer there";
            if (!pr->is_compatible) return before + " doesn't suit this printer now";
            Controller::select_filament_slot(s, before);
            return {};
        }
        const std::string scope = slot.is_number_integer() ? std::string("filament") : t.value("preset_type", std::string());
        PresetCollection *c = collection(scope);
        if (c == nullptr) return "Can't switch back";
        if (c->find_preset(before, false, true) == nullptr) return before + " is no longer there";
        Tab *tab = wxGetApp().get_tab(preset_type(scope));
        if (tab == nullptr) return "The settings tab is not ready";
        // The way PUT /preset avoids the unsaved-changes dialog: drop the unsaved edits first (the row's tip says so).
        const std::vector<std::string> dropped = discard_edits();
        s_undo_dropped                         = dropped;
        if (!tab->select_preset(before, false, "", /*force_select=*/true, /*force_no_transfer=*/true)) {
            cancelled = true;
            return "Switch cancelled";
        }
        if (!dropped.empty()) {
            note = " (dropped unsaved edits: ";
            for (size_t i = 0; i < dropped.size(); ++i) note += (i ? ", " : "") + dropped[i];
            note += ")";
        }
        return {};
    }

    if (type == "preset_saved") {
        const std::string ts = t.value("preset_type", std::string()), name = t.value("name", std::string());
        const std::string why = Controller::preset_delete_refusal(ts, name);
        if (why == "unknown_preset") return name + " is gone already";
        if (why == "builtin_preset") return "Built-in preset";
        if (why == "preset_selected") return "It is selected; switch to another preset first";
        if (why == "has_children") return "Other presets inherit it";
        if (!why.empty()) return "Can't delete it";
        if (!Controller::delete_user_preset(ts, name)) return "Couldn't delete it";
        return {};
    }

    if (type == "plate_added") {
        // The plate the row added, by its ObjectID: an index or an origin names whatever plate sits in that slot of
        // the grid now (deleting an earlier plate moves every later one down a slot).
        PartPlateList &plates = p->get_partplate_list();
        const int      idx    = created_plate_now(r.id);
        if (idx < 0) return "The plate has changed since; restore an earlier point instead";
        PartPlate *plate = plates.get_plate(idx);
        // Extra guard: still in its recorded slot, it must also have the recorded origin.
        const json origin = t.value("origin", json());
        if (idx == t.value("index", -1) && origin.is_array() && origin.size() == 3) {
            const Vec3d o = plate->get_origin();
            if (!same_matrix(json{o.x(), o.y(), o.z()}, origin))
                return "The plate has changed since; restore an earlier point instead";
        }
        if (!plate->empty()) return "It has objects on it now";
        if (!p->can_delete_plate()) return "It is the last plate";
        if (p->delete_plate(idx) != 0) return "Couldn't delete it";
        Controller::slice_state_follows_plate();
        return {};
    }

    if (type == "file_moved") {
        const std::string err = Controller::move_project_file_back(t.value("from", std::string()), t.value("to", std::string()));
        if (err.empty()) return {};
        if (err == "exists") return "The old name is taken now";
        if (err == "file_not_found") return r.value + " is gone";
        if (err == "directory_not_found") return "The old folder is gone";
        return "Couldn't move it back";
    }

    if (type == "slot_mapping") {
        const std::string err = Controller::revert_slot_mapping(t);
        if (err.empty()) return {};
        if (err == "project_changed") return "The project changed since";
        return "Can't change it back";
    }

    if (type == "removed") {
        const ModelObject *stash = McpRecorder::get().stashed_object(t.value("stash", 0), t.value("stash_session", std::string()));
        if (stash == nullptr) return "Can't put it back after a restart; restore an earlier point instead";
        Plater::TakeSnapshot snapshot(p, "Put back " + stash->name);
        // Model::add_object clones the copy (volumes, modifiers, paint, per-object config, layer ranges,
        // instances with their transforms). Same steps as a paste (Selection::paste_objects_from_clipboard), at
        // the object's own position so it lands on the plate it was on.
        p->model().add_object(*stash);
        const int        oi = int(p->model().objects.size()) - 1;
        std::vector<int> inst;
        for (size_t i = 0; i < p->model().objects[oi]->instances.size(); ++i) inst.push_back(int(i));
        for (int ii : inst) p->get_partplate_list().notify_instance_update(oi, ii, /*is_new=*/true);
        wxGetApp().obj_list()->paste_objects_into_list({size_t(oi)});
        wxGetApp().obj_list()->update_plate_values_for_items();
        return {};
    }

    const bool                      current = target_current(t);
    const std::vector<McpObjectRef> refs    = object_refs();

    if (type == "instances") {
        // Every instance back where it was, on the plates there were: plates the job added are dropped (only
        // when they hold nothing but these instances), plates it deleted are made again first, so the plate
        // grid is the one the matrices were taken in.
        PartPlateList &plates        = p->get_partplate_list();
        const int      plates_before = t.value("plates_before", plates.get_plate_count());
        const json     items         = t.value("items", json::array());
        std::set<std::pair<int, int>> ours;
        for (const json &it : items) {
            const int oi = mcp_find_object(it, current, std::string(), refs);
            if (oi == -2) return kUntracked;
            if (oi < 0) return gone;
            const int ii = mcp_find_instance(it, current, refs[size_t(oi)].instance_ids);
            if (ii < 0) return "Moved again since";
            ours.insert({oi, ii});
        }
        for (int pi = plates_before; pi < plates.get_plate_count(); ++pi) {
            PartPlate *plate = plates.get_plate(pi);
            const Model &model = p->model();
            for (size_t oi = 0; oi < model.objects.size(); ++oi)
                for (size_t ii = 0; ii < model.objects[oi]->instances.size(); ++ii)
                    if (plate->contain_instance(int(oi), int(ii)) && ours.count({int(oi), int(ii)}) == 0)
                        return "Plate " + std::to_string(pi + 1) + " holds other objects now";
        }
        Plater::TakeSnapshot snapshot(p, "Put back");
        while (plates.get_plate_count() < plates_before)
            if (plates.create_plate() < 0) break;
        while (plates.get_plate_count() > plates_before && plates.get_plate_count() > 1)
            if (p->delete_plate(plates.get_plate_count() - 1) != 0) break;
        // Found again: dropping a plate may have taken objects with it, which shifts the object indices.
        const std::vector<McpObjectRef> now = object_refs();
        std::map<int, std::vector<int>> by_object;
        for (const json &it : items) {
            const int oi = mcp_find_object(it, current, std::string(), now);
            if (oi < 0) continue;
            const int ii = mcp_find_instance(it, current, now[size_t(oi)].instance_ids);
            if (ii < 0) continue;
            set_matrix(p->model().objects[oi], ii, it.value("before", json()));
            by_object[oi].push_back(ii);
        }
        for (const auto &kv : by_object) instances_changed(kv.first, kv.second);
        p->update();
        Controller::slice_state_follows_plate();
        return {};
    }

    // Rows about one object.
    const int oi = mcp_find_object(t, current, object_name_of(r), refs);
    if (oi == -2) return kUntracked;
    if (oi < 0) return gone;
    ModelObject *mo = p->model().objects[oi];
    const std::vector<uint64_t> &instance_ids = refs[size_t(oi)].instance_ids;

    if (type == "added") {
        Plater::TakeSnapshot snapshot(p, "Delete Object: " + mo->name);
        p->remove(size_t(oi));
        return {};
    }
    if (type == "object_config") {
        const std::string key = t.value("key", std::string());
        DynamicPrintConfig cfg = mo->config.get();
        try {
            if (t.value("had_override", false)) cfg.set_deserialize_strict(key, t.value("before_raw", std::string()));
            else cfg.erase(key);
        } catch (const std::exception &) {
            return "Can't set it back";
        }
        if (cfg.has(key)) {
            json        rd;
            std::string rtext;
            if (!Controller::config_value_in_range(key, mo->config.get().option(key), *cfg.option(key), rd, rtext))
                return "Can't set it back: " + rtext;
        }
        Plater::TakeSnapshot snapshot(p, "Change Object Settings");
        mo->config.assign_config(std::move(cfg));
        p->changed_object(oi);
        if (key == "extruder") refresh_extruder_column(oi, mo);
        return {};
    }
    if (type == "layer_profile") {
        std::vector<double> before;
        for (const json &v : t.value("before", json::array()))
            if (v.is_number()) before.push_back(v.get<double>());
        if (before.empty()) mo->layer_height_profile.clear();
        else mo->layer_height_profile.set(std::move(before));
        p->schedule_background_process();
        wxGetApp().obj_list()->update_info_items(size_t(oi));
        return {};
    }
    if (type == "height_range" || type == "height_ranges_cleared") {
        std::vector<std::pair<t_layer_height_range, DynamicPrintConfig>> put;
        try {
            if (type == "height_ranges_cleared")
                for (const json &rg : t.value("ranges", json::array()))
                    put.emplace_back(t_layer_height_range{rg.value("min_z", 0.0), rg.value("max_z", 0.0)},
                                     config_from_map(rg.value("config", json())));
            else if (!t.value("before", json()).is_null())
                put.emplace_back(t_layer_height_range{t.value("min_z", 0.0), t.value("max_z", 0.0)},
                                 config_from_map(t["before"]));
        } catch (const std::exception &) {
            return "Can't put the ranges back";
        }
        Plater::TakeSnapshot snapshot(p, type == "height_range" ? "Delete Height Range" : "Add Height Range");
        if (type == "height_range" && t.value("before", json()).is_null())
            mo->layer_config_ranges.erase(t_layer_height_range{t.value("min_z", 0.0), t.value("max_z", 0.0)});
        for (auto &kv : put) mo->layer_config_ranges[kv.first].assign_config(std::move(kv.second));
        p->changed_object(oi);
        wxGetApp().obj_list()->update_info_items(size_t(oi));
        return {};
    }
    if (type == "transform" || type == "instance_plate") {
        const int ii = mcp_find_instance(t, current, instance_ids);
        if (ii < 0) return "Moved again since";
        Plater::TakeSnapshot snapshot(p, type == "transform" ? "Transform Object" : "move to plate");
        set_matrix(mo, ii, t.value("before", json()));
        instances_changed(oi, {ii});
        if (type == "instance_plate") Controller::slice_state_follows_plate();
        return {};
    }
    if (type == "duplicate") {
        const int ii = mcp_find_instance(t, current, instance_ids);
        if (ii < 0) return "The copy is gone already";
        Plater::TakeSnapshot snapshot(p, "Decrease Instances");
        PartPlateList &plates = p->get_partplate_list();
        if (mo->instances.size() <= 1) {
            p->remove(size_t(oi));
            return {};
        }
        if (ii == int(mo->instances.size()) - 1) {
            // Plater::decrease_instances' steps, for this object instead of the selection.
            plates.notify_instance_removed(oi, ii);
            mo->delete_last_instance();
        } else {
            // A later copy follows it: the plates' (object, instance) pairs shift, so rebuild them.
            mo->delete_instance(size_t(ii));
            plates.reload_all_objects();
        }
        p->update();
        wxGetApp().obj_list()->decrease_object_instances(size_t(oi), 1);
        p->changed_object(oi);
        wxGetApp().obj_list()->update_plate_values_for_items();
        return {};
    }
    return "Nothing to undo";
}

// "Reverted in the panel: Wall loops back to 2" and the like, for the user-change journal.
std::string journal_line(const McpRow &r)
{
    static const std::map<std::string, std::string> past{
        {"Revert", "Reverted"},       {"Switch back", "Switched back"}, {"Change back", "Changed back"},
        {"Remove", "Removed"},        {"Put back", "Put back"},         {"Rotate back", "Rotated back"},
        {"Scale back", "Scaled back"}, {"Move back", "Moved back"},      {"Remove copy", "Removed the copy"},
        {"Delete plate", "Deleted"},   {"Delete preset", "Deleted"},     {"Remove range", "Removed"},
        {"Undo", "Undid"},
    };
    auto              it   = past.find(r.undo_verb);
    const std::string verb = it != past.end() ? it->second : r.undo_verb;
    const std::string type = r.target.value("type", std::string());
    std::string       what;
    if (r.undo_verb == "Revert") what = r.label + " back to " + r.before;
    else if (r.undo_verb == "Switch back" || r.undo_verb == "Change back") what = r.label + " to " + r.before;
    else if (type == "file_moved") what = r.value + " to " + r.before;
    else if (type == "plate_added" || type == "preset_saved") what = r.value;
    else if (type == "height_range") what = r.label + " of " + r.where;
    else if (type == "layer_profile") what = "the layer height profile of " + r.value;
    else if (type == "instances") what = r.where.empty() ? r.value : r.where;
    else what = object_name_of(r);
    return verb + " in the panel: " + what;
}

void tell(const std::string &text)
{
    // The journal entry, then a new baseline: the change is reported as this panel action, not again as an edit.
    mcp_journal_panel(text);
}

void publish(json event)
{
    try {
        wxGetApp().remote_api_server().broadcast(event);
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "MCP panel: event not sent: " << e.what();
    }
}

// Why a panel action cannot run now ("" when it can).
std::string busy_reason()
{
    Plater *p = plater();
    if (p == nullptr) return "Not ready";
    if (McpActions::restoring()) return "A restore is running";
    if (p->is_loading_project()) return "Not while a project is loading";
    if (p->is_background_process_slicing()) return "Not while slicing";
    // An arrange or orient job finishes (and records its row) from the job queue, maybe inside a restore's load.
    if (!p->get_ui_job_worker().is_idle()) return "Not while OrcaSlicer is busy with a job";
    // Inside an export's or an API task's event pump the project is being written or changed: wait. Also while an
    // API request is between its change and its journal baseline: the journal would take the API's change for the
    // user's.
    if (Controller::backup_in_progress() ||
        ((Controller::api_ui_task_active() || Controller::requests_in_flight()) && s_test_depth == 0))
        return "Busy with the assistant's request; try again in a moment";
    return {};
}

// The restore state, dropped once it no longer applies (another project, or the assistant changed something).
bool restore_state_current(McpRecorder &rec)
{
    if (s_marked_key != rec.history_dir() || s_marked_epoch != rec.history_epoch()) {
        s_marked.clear();
        s_marked_key = rec.history_dir();
        s_marked_epoch = rec.history_epoch();
    }
    if (s_restore.restored_to < 0 && s_restore.snapshot.empty()) return false;
    const auto &gs = rec.history().groups();
    if (s_restore.key != rec.history_dir() || s_restore.epoch != rec.history_epoch() || (gs.empty() ? -1 : gs.back().id) != s_restore.newest_at) {
        s_restore = RestoreState();
        return false;
    }
    return true;
}

std::string point_time(const McpHistory &h, int point)
{
    const auto &gs = h.groups();
    if (point == 0) return gs.empty() ? std::string() : gs.front().time;
    for (const McpGroup &g : gs)
        if (g.id == point) return g.time;
    return {};
}

// A restore point written while the plate was empty: the presets and their unsaved edits.
std::string open_preset_point(const std::string &path_utf8)
{
    json j;
    try {
        boost::nowide::ifstream in(into_path(from_u8(path_utf8)).string().c_str(), std::ios::binary);
        if (!in) return "The restore point can't be read";
        j = json::parse(in);
    } catch (const std::exception &) {
        return "The restore point can't be read";
    }
    Plater *p      = plater();
    auto   *bundle = wxGetApp().preset_bundle;
    discard_edits();
    // The plate was empty: no objects, one plate.
    if (!p->model().objects.empty()) p->delete_all_objects_from_model();
    while (p->get_partplate_list().get_plate_count() > 1)
        if (p->delete_plate(p->get_partplate_list().get_plate_count() - 1) != 0) break;
    auto select = [](Preset::Type type, PresetCollection &c, const std::string &name) {
        if (name.empty() || c.get_selected_preset_name() == name || c.find_preset(name, false, true) == nullptr) return;
        if (Tab *tab = wxGetApp().get_tab(type); tab != nullptr)
            tab->select_preset(name, false, "", /*force_select=*/true, /*force_no_transfer=*/true);
    };
    select(Preset::TYPE_PRINTER, bundle->printers, j.value("printer", std::string()));
    select(Preset::TYPE_PRINT, bundle->prints, j.value("print", std::string()));
    std::vector<std::string> filaments;
    for (const json &f : j.value("filaments", json::array()))
        if (f.is_string()) filaments.push_back(f.get<std::string>());
    if (!filaments.empty()) {
        if (!p->sidebar().is_multifilament()) {
            select(Preset::TYPE_FILAMENT, bundle->filaments, filaments.front());
        } else {
            // The slots there are now; a different filament count is not changed here.
            for (size_t i = 0; i < filaments.size() && i < bundle->filament_presets.size(); ++i) {
                if (bundle->filament_presets[i] == filaments[i]) continue;
                const Preset *pr = bundle->filaments.find_preset(filaments[i], false, true);
                if (pr == nullptr || !pr->is_compatible) continue;
                Controller::select_filament_slot(i, filaments[i]);
            }
        }
    }
    // The unsaved edits on top. The filament edits belong to the preset the Filament tab showed then: one of the
    // slot presets; skip them when the tab shows another now.
    const json edits = j.value("edits", json::object());
    json       apply = json::object();
    for (const char *g : {"print", "printer"}) {
        const json ge = edits.value(g, json::object());
        for (auto it = ge.begin(); it != ge.end(); ++it) apply[it.key()] = it.value();
    }
    if (std::find(filaments.begin(), filaments.end(), bundle->filaments.get_selected_preset_name()) != filaments.end()) {
        const json fe = edits.value("filament", json::object());
        for (auto it = fe.begin(); it != fe.end(); ++it) apply[it.key()] = it.value();
    }
    if (!apply.empty()) {
        const std::string err = apply_preset_edits(apply);
        if (!err.empty()) BOOST_LOG_TRIVIAL(warning) << "MCP panel: restore point edit not applied: " << err;
    }
    bundle->export_selections(*wxGetApp().app_config);
    p->update_project_dirty_from_presets();
    p->get_partplate_list().invalid_all_slice_result();
    p->on_config_change(bundle->full_config());
    return {};
}

// After a project load OrcaSlicer leaves an edited preset's *_settings_id at the value the 3MF carried, which can
// differ from the selected preset's own (its dirty check, Preset.cpp skipped_in_dirty, ignores those keys, so the GUI
// shows nothing). The API's modified lists would then gain print_settings_id after a restore: put the selected
// preset's value back, so a restore brings back exactly the edits there were.
void settings_ids_follow_selection()
{
    auto *b   = wxGetApp().preset_bundle;
    auto  fix = [](PresetCollection &c, const char *key) {
        Preset             &ed = c.get_edited_preset();
        const ConfigOption *so = c.get_selected_preset().config.option(key);
        ConfigOption       *eo = ed.config.option(key);
        if (so != nullptr && eo != nullptr && so->type() == eo->type() && !(*so == *eo)) eo->set(so);
    };
    fix(b->prints, "print_settings_id");
    fix(b->filaments, "filament_settings_id");
    fix(b->printers, "printer_settings_id");
}

// Opens a restore point (.3mf as a project, .json as presets) and keeps the project's own name and path.
std::string open_point(const std::string &path_utf8)
{
    Plater           *p         = plater();
    const std::string orig_path = p->get_project_filename(".3mf").ToUTF8().data();
    const wxString    orig_name = p->get_project_name();
    // No checkpoint while the point is being opened (an arrange job finishing inside the load's event pump), and
    // the objects get new ids however the open ends: rows recorded before it find them by name from now on.
    McpRecorder::set_opening_point(true);
    ScopeGuard opening([] {
        McpRecorder::set_opening_point(false);
        McpRecorder::note_project_reloaded();
    });
    if (boost::algorithm::iends_with(path_utf8, ".json")) {
        const std::string err = open_preset_point(path_utf8);
        if (!err.empty()) return err;
    } else {
        // The project's own name and path back (an Untitled one keeps its name and no path), and the checkpoint
        // out of File > Recent; also after a failed open, which may have replaced the project half way (load_project
        // resets it before reading the file), and when the open throws: a later Save must never write into the
        // history folder.
        bool put_back      = false;
        auto put_name_back = [&] {
            if (put_back) return;
            put_back = true;
            p->set_project_filename(orig_path.empty() ? orig_name : from_u8(orig_path));
            Controller::forget_recent(path_utf8);
            McpRecorder::forget_project_opened();
        };
        ScopeGuard on_throw([&put_name_back] {
            try {
                put_name_back();
            } catch (...) {
                BOOST_LOG_TRIVIAL(error) << "MCP panel: project name not put back after a failed restore";
            }
        });
        // The API's open steps on the checkpoint: presets from it, unsaved edits dropped, prompts answered, no
        // sensitive-key check (the user's own earlier state, checked against its fingerprint by the caller).
        const json r = Controller::open_project_from(p, path_utf8, /*keep_presets=*/false, /*policy_check=*/false);
        put_name_back();
        if (r.contains("error")) {
            BOOST_LOG_TRIVIAL(error) << "MCP panel: restore point not opened: " << r.dump();
            p->set_plater_dirty(true);
            p->update_title_dirty_status();
            Controller::slice_state_follows_plate();
            return "Couldn't open the restore point";
        }
    }
    settings_ids_follow_selection();
    // Opening the checkpoint is not the project being opened: the recorder keeps this project's timeline.
    McpRecorder::forget_project_opened();
    p->set_plater_dirty(true);
    p->update_title_dirty_status();
    Controller::slice_state_follows_plate();
    return {};
}

} // namespace

McpActions::TestCallScope::TestCallScope() { ++s_test_depth; }
McpActions::TestCallScope::~TestCallScope() { --s_test_depth; }

json McpActions::activity()
{
    McpRecorder &rec = McpRecorder::get();
    rec.sync_project();
    json       j       = rec.history().page_history();
    const bool current = restore_state_current(rec);
    j["restored_to"]   = current && s_restore.restored_to >= 0 ? json(s_restore.restored_to) : json(nullptr);
    j["restore_failed"] = current && s_restore.restored_to >= 0 && s_restore.failed;
    bool can_undo = false;
    if (current && !s_restore.snapshot.empty()) {
        boost::system::error_code ec;
        can_undo = fs::exists(into_path(from_u8(rec.checkpoint_path(s_restore.snapshot))), ec);
    }
    j["can_undo_restore"] = can_undo;
    const json valid = slice_valid();
    if (!valid.is_null()) j["slice_valid"] = valid;
    return j;
}

json McpActions::slice_valid()
{
    Plater *p = plater();
    if (p == nullptr) return nullptr;
    // The newest slice result, as the page's latestSlice picks it: a slice row with a result and no error, the latest
    // time first, then the highest id.
    const McpRow *newest = nullptr;
    for (const McpRow &r : McpRecorder::get().history().rows())
        if (r.kind == "slice" && !r.error && r.result.is_object() &&
            (newest == nullptr || r.ts > newest->ts || (r.ts == newest->ts && r.id > newest->id)))
            newest = &r;
    if (newest == nullptr) return nullptr;
    // Its plate: McpRecorder::slice_finished writes "plate <n>".
    int plate_no = 0;
    if (std::sscanf(newest->where.c_str(), "plate %d", &plate_no) != 1) return nullptr;
    PartPlateList &plates = p->get_partplate_list();
    if (plate_no < 1 || plate_no > plates.get_plate_count()) return nullptr;
    const PartPlate *plate = plates.get_plate(plate_no - 1);
    return plate != nullptr ? json(plate->is_slice_result_valid()) : json(nullptr);
}

json McpActions::live_values()
{
    McpRecorder &rec = McpRecorder::get();
    rec.sync_project();
    json        out = json::object();
    const auto &gs  = rec.history().groups();
    const int   newest = gs.empty() ? -1 : gs.back().id;
    // R43: only the tracked rows (the latest row of each setting, McpHistory.hpp mcp_tracking) get a live entry; the
    // page shows "changed by you" only for those.
    const McpTracking tracking = mcp_tracking(rec.history().rows(), newest);
    for (const McpRow &r : rec.history().rows()) {
        if (tracking.tracked.count(r.id) == 0) continue;
        try {
            const Live l = live_of(r);
            if (!l.known) continue;
            out[std::to_string(r.id)] = {{"value", l.present ? json(l.value) : json(r.value)}, {"present", l.present}};
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(warning) << "MCP panel: no live value for row " << r.id << ": " << e.what();
        }
    }
    return out;
}

json McpActions::undo(int row_id)
{
    const std::string busy = busy_reason();
    if (!busy.empty()) return result_failed(busy);
    mcp_journal_begin(); // the user's edits before this action stay theirs in the journal
    McpRecorder &rec = McpRecorder::get();
    rec.sync_project();
    const McpRow *found = rec.history().row(row_id);
    if (found == nullptr) return result_failed("Not in this project's timeline");
    const McpRow r = *found; // the history may change under the undo (a moved file moves its history)
    if (r.undo_verb.empty()) return result_failed("Nothing to undo");
    if (!r.undone_at.empty()) return result_failed("Reverted at " + r.undone_at);
    const auto &gs     = rec.history().groups();
    // R43: the assistant changed the same setting again later: undoing this row would overwrite that change. P1: its
    // own preset switch dropped the edit, so there is nothing of it left to undo.
    {
        const McpTracking tr = mcp_tracking(rec.history().rows(), gs.empty() ? -1 : gs.back().id);
        if (tr.dropped.count(r.id) > 0) return result_failed("Dropped by a preset switch; restore an earlier point instead");
        if (tr.superseded.count(r.id) > 0) return result_failed("Changed again since; restore an earlier point instead");
    }
    // Never overwrite a later change: the row's value must still be what is there.
    try {
        const Live l = live_of(r);
        if (l.known && !l.present) return result_failed(object_name_of(r) + " is no longer on the plate");
        if (l.known && !same_text(l.value, r.value)) return result_failed("You changed it since");
    } catch (const std::exception &e) {
        return result_failed(std::string("Can't check it now: ") + e.what());
    }
    bool        cancelled = false;
    std::string why, note;
    // Park API tasks (and their journal steps) while the undo runs: a preset switch back or a plate deletion may
    // pump events, and a task run inside would see the undo half applied.
    Controller::set_backup_in_progress(true);
    ScopeGuard gate([] { Controller::set_backup_in_progress(false); });
    try {
        why = undo_row(r, cancelled, note);
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "MCP panel: undo of row " << r.id << " failed: " << e.what();
        why = "Couldn't undo it";
    }
    if (cancelled) return result_cancelled(why);
    if (!why.empty()) return result_failed(why);
    rec.history().mark_undone(r.id, McpRecorder::hhmm(rec.now_s()));
    // P1: a preset switch undone here is a switch back that happened now: it keeps counting for the rows before it
    // (its own discards stay discarded), and the switch back drops what it names (McpHistory.hpp mcp_tracking).
    if (r.target.is_object() && r.target.value("type", std::string()) == "preset") {
        int newest_row = r.id;
        for (const McpRow &x : rec.history().rows()) newest_row = std::max(newest_row, x.id);
        if (McpRow *m = rec.history().row(r.id); m != nullptr && m->target.is_object())
            m->target["reverted"] = {{"after_row", newest_row}, {"discarded", s_undo_dropped}};
    }
    rec.changed();
    publish({{"event", "panel.reverted"}, {"group", r.group}, {"row", r.id}});
    tell(journal_line(r) + note);
    return result_ok();
}

json McpActions::restore(int group)
{
    const std::string busy = busy_reason();
    if (!busy.empty()) return result_failed(busy);
    mcp_journal_begin(); // the user's edits before this action stay theirs in the journal
    McpRecorder &rec = McpRecorder::get();
    rec.sync_project();
    McpHistory &h    = rec.history();
    const std::string file = h.checkpoint_of(group);
    if (file.empty() || h.groups().empty()) return result_failed("No restore point here");
    const int newest = h.groups().back().id;
    if (group >= newest) return result_failed("No restore point here");
    const std::string path = rec.checkpoint_path(file);
    boost::system::error_code ec;
    if (!fs::is_regular_file(into_path(from_u8(path)), ec)) return result_failed("The restore point is gone");

    s_restoring.store(true);
    ScopeGuard done([] { s_restoring.store(false); });
    // Outside an API task: park any API task that arrives while the project is replaced (F8), as an export does.
    Controller::set_backup_in_progress(true);
    ScopeGuard gate([] { Controller::set_backup_in_progress(false); });

    // The point opens without the API's sensitive-key check: it must be the file the recorder wrote. Checked with
    // the API parked (409 while restoring, tasks held by the gate), before anything changes.
    if (!mcp_fingerprint_ok(h.fingerprint_of(group), McpRecorder::fingerprint(path))) {
        BOOST_LOG_TRIVIAL(warning) << "MCP panel: restore point " << file << " does not match its fingerprint";
        return result_failed(kPointChanged);
    }

    // An earlier restore's Undo ends here: its snapshot is about to be replaced, and a failed write must never
    // leave an Undo that opens an older state.
    restore_state_current(rec);
    RestoreState next;
    next.prev_restored_to = s_restore.restored_to;
    next.marked_before    = s_marked;
    s_restore.snapshot.clear();
    for (const char *old : {"pre-restore.3mf", "pre-restore.json"}) {
        boost::system::error_code rec_ec;
        fs::remove(into_path(from_u8(rec.checkpoint_path(old))), rec_ec);
    }

    // The state now, for "Undo" on the "Restored to" line. It is also the newest group's end state.
    const std::string    snapshot    = rec.write_checkpoint("pre-restore");
    const McpFingerprint snapshot_fp = snapshot.empty() ? McpFingerprint() : McpRecorder::fingerprint(rec.checkpoint_path(snapshot));
    if (snapshot.empty() || snapshot_fp.sha256.empty()) return result_failed("Couldn't save the current state first");
    if (h.checkpoint_of(newest).empty()) {
        const std::string ext  = fs::path(snapshot).extension().string();
        const std::string name = "cp-" + std::to_string(newest) + ext;
        boost::system::error_code cec;
        fs::copy_file(into_path(from_u8(rec.checkpoint_path(snapshot))), into_path(from_u8(rec.checkpoint_path(name))),
                      fs::copy_options::overwrite_existing, cec);
        // Not pruned yet: at the 20-point cap that could delete the very point being restored.
        if (!cec) rec.set_point_checkpoint(newest, name, snapshot_fp, /*prune=*/false); // a byte copy: same fingerprint
    }

    // Undoable from here on, also when the open below fails half way (the project may already be replaced).
    for (const McpRow &r : h.rows()) next.marks_before[r.id] = r.undone_at;
    next.key         = rec.history_dir();
    next.epoch       = rec.history_epoch();
    next.restored_to = group;
    next.snapshot    = snapshot;
    next.snapshot_fp = snapshot_fp;
    rec.close_group(); // the next assistant change starts a group of its own
    next.newest_at = h.groups().back().id;
    s_restore      = next;

    const std::string err = open_point(path);
    rec.prune_checkpoints();
    if (!err.empty()) {
        s_restore.failed = true; // the page says "Couldn't restore to"; its Undo stays
        rec.changed();
        // The open may have replaced the project half way: the journal says so (and takes the new baseline), so
        // the next request does not report the half-loaded project as the user's edits.
        tell("Tried to restore in the panel to " + point_time(h, group) + "; it failed");
        return result_failed(err);
    }

    // The groups after the point are undone; ones an earlier restore undid up to here are back.
    const std::string now = McpRecorder::hhmm(rec.now_s());
    std::vector<std::pair<int, std::string>> marks;
    for (const McpRow &r : h.rows()) {
        if (r.group > group && r.undone_at.empty()) {
            marks.emplace_back(r.id, now);
            s_marked.insert(r.id);
        } else if (r.group <= group && s_marked.count(r.id)) {
            marks.emplace_back(r.id, std::string());
            s_marked.erase(r.id);
        }
    }
    for (auto &m : marks) h.mark_undone(m.first, m.second);
    rec.changed();
    publish({{"event", "panel.restored"}, {"group", group}});
    tell("Restored in the panel to " + point_time(h, group));
    return result_ok();
}

json McpActions::undo_restore()
{
    const std::string busy = busy_reason();
    if (!busy.empty()) return result_failed(busy);
    mcp_journal_begin(); // the user's edits before this action stay theirs in the journal
    McpRecorder &rec = McpRecorder::get();
    rec.sync_project();
    if (!restore_state_current(rec) || s_restore.snapshot.empty()) return result_failed("Nothing to undo");
    const std::string path = rec.checkpoint_path(s_restore.snapshot);
    boost::system::error_code ec;
    if (!fs::is_regular_file(into_path(from_u8(path)), ec)) return result_failed("The snapshot before the restore is gone");

    s_restoring.store(true);
    ScopeGuard done([] { s_restoring.store(false); });
    Controller::set_backup_in_progress(true);
    ScopeGuard gate([] { Controller::set_backup_in_progress(false); });

    if (!mcp_fingerprint_ok(s_restore.snapshot_fp, McpRecorder::fingerprint(path))) {
        BOOST_LOG_TRIVIAL(warning) << "MCP panel: the pre-restore snapshot does not match its fingerprint";
        return result_failed("The snapshot before the restore was changed on disk, so it is not used");
    }

    // m6: the note names the point the undone restore had gone back to.
    const std::string undone_time = s_restore.restored_to >= 0 ? point_time(rec.history(), s_restore.restored_to) : std::string();
    const std::string undone_what = undone_time.empty() ? std::string("the restore") : "the restore to " + undone_time;
    const std::string err = open_point(path);
    if (!err.empty()) {
        tell("Tried to undo " + undone_what + " in the panel; it failed");
        return result_failed(err);
    }

    McpHistory &h = rec.history();
    for (const McpRow &r : h.rows()) {
        auto it = s_restore.marks_before.find(r.id);
        const std::string before = it == s_restore.marks_before.end() ? std::string() : it->second;
        if (r.undone_at != before) h.mark_undone(r.id, before);
    }
    s_marked = s_restore.marked_before;
    RestoreState back;
    back.key         = rec.history_dir();
    back.epoch       = rec.history_epoch();
    back.restored_to = s_restore.prev_restored_to;
    back.newest_at   = h.groups().empty() ? -1 : h.groups().back().id;
    s_restore        = std::move(back); // one level: no Undo of the undo
    rec.close_group();
    rec.changed();
    publish({{"event", "panel.restored"}, {"group", nullptr}, {"undone", true}});
    tell("Undid " + undone_what + " in the panel; the project is back as it was before that restore");
    return result_ok();
}

json McpActions::reveal(int row_id)
{
    // Only showing a setting: it waits out a restore or a project load (the tabs are being rebuilt), but not a
    // slice or a job, so a click during background slicing still finds the setting.
    if (restoring()) return result_failed("A restore is running");
    if (Plater *pl = plater(); pl == nullptr || pl->is_loading_project()) return result_failed("Not while a project is loading");
    McpRecorder &rec = McpRecorder::get();
    rec.sync_project();
    const McpRow *r = rec.history().row(row_id);
    if (r == nullptr) return result_failed("Not in this project's timeline");
    if (!mcp_row_revealable(*r)) return result_failed("Nothing to show in the settings");
    const std::string  key   = r->target.value("key", std::string());
    const std::string  scope = r->target.value("scope", std::string());
    const Preset::Type type  = scope == "print" ? Preset::TYPE_PRINT : scope == "filament" ? Preset::TYPE_FILAMENT : Preset::TYPE_PRINTER;
    PresetCollection  *c     = collection(scope);
    if (c == nullptr || !c->get_edited_preset().config.has(key)) return result_failed("The setting is not in this preset any more");
    if (wxGetApp().get_tab(type) == nullptr) return result_failed("The settings tab is not there");

    Sidebar &sidebar = wxGetApp().sidebar();
    // The row changed the global print preset: show that, not an object's own value of the key (get_tab_for_option
    // picks the object tab while the object settings are shown).
    if (type == Preset::TYPE_PRINT)
        if (ParamsPanel *panel = wxGetApp().params_panel()) panel->switch_to_global();
    // As the sidebar's search does: the index entry gives the key as the page lists it (a vector option per element,
    // "nozzle_temperature#0") and its page, so the field is found and blinks. Without an entry (hidden in the current
    // mode) the bare key, as the notifications' "Jump to" links pass it.
    // A linear scan: the index is kept sorted by label for the search box, and get_option() binary-searches by key
    // (its other callers re-sort by key first, which would break the search box's positions).
    const Search::Option *hit = nullptr;
    for (const Search::Option &o : sidebar.settings_index().options()) {
        if (o.type != type) continue;
        const std::string k = o.opt_key();
        if (k == key || (k.size() > key.size() && k.compare(0, key.size(), key) == 0 && k[key.size()] == '#')) { hit = &o; break; }
    }
    const std::string shown_key = hit != nullptr ? hit->opt_key() : key;
    if (hit != nullptr)
        sidebar.jump_to_option(shown_key, type, hit->category);
    else
        sidebar.jump_to_option(shown_key, type, L"");
    // OrcaSlicer's own highlight (the search's): the label blinks in its teal for 11 steps of 300 ms. A reveal from the
    // panel is easy to miss (the user's eyes are on the panel, and in dark mode the teal is dim): twice as long, with
    // the same blink. Nothing when the jump started none (the setting is hidden in this mode).
    if (Tab *tab = get_tab_for_option(shown_key, type)) tab->lengthen_highlight(kRevealBlinkSteps);
    return result_ok();
}

json McpActions::apply_as_user(const json &changes)
{
    if (!changes.is_object() || changes.empty()) return result_failed("No changes");
    const std::string err = apply_preset_edits(changes);
    return err.empty() ? result_ok() : result_failed(err);
}

}}} // namespace Slic3r::GUI::RemoteAPI
