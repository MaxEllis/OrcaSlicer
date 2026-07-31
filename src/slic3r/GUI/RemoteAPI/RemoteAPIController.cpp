// src/slic3r/GUI/RemoteAPI/RemoteAPIController.cpp
#include "RemoteAPIController.hpp"

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r_version.h"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
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
#include "slic3r/GUI/GLCanvas3D.hpp"      // plate_render: offscreen thumbnail/gcode render
#include "slic3r/GUI/Camera.hpp"          // plate_render: ViewAngleType
#include "libslic3r/GCode/ThumbnailData.hpp" // plate_render: ThumbnailData/ThumbnailsParams
#include <miniz.h>                        // plate_render: RGBA -> PNG in memory

#include <algorithm>
#include <cmath>
#include <array>
#include <atomic>
#include <cctype>
#include <deque>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <sstream>
#include <vector>

#include <boost/filesystem.hpp>

namespace Slic3r { namespace GUI { namespace RemoteAPI {

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

static int find_object_index(const Model &model, uint64_t id); // defined with the M4b object handlers below


void Controller::notify_config_changed(int preset_type)
{
    if (g_controller) g_controller->on_config_changed(preset_type);
}

void Controller::notify_project_opened()
{
    if (g_controller == nullptr) return;
    // On the GUI thread at the end of Plater::load_project.
    std::string name = wxGetApp().plater()->get_project_filename().ToUTF8().data();
    wxGetApp().remote_api_server().broadcast({{"event", "project.opened"}, {"project", name}});
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
// bursts. Two-way exclusion: API tasks arriving during an export are parked on
// s_deferred_ui_tasks and flushed when the export ends; the exporter skips a
// cycle (re-arming itself) if an API task is on the stack (nested-pump case).
namespace {

struct UiTask : std::enable_shared_from_this<UiTask>
{
    std::shared_ptr<std::promise<nlohmann::json>> promise;
    std::shared_ptr<std::atomic<bool>>            cancelled;
    std::function<nlohmann::json()>               fn;

    void schedule()
    {
        auto self = shared_from_this();
        wxGetApp().CallAfter([self] { self->run(); });
    }
    void run(); // defined after the gate statics below
};

int  s_api_ui_task_depth  = 0;
bool s_backup_in_progress = false;
std::deque<std::shared_ptr<UiTask>> s_deferred_ui_tasks;

void UiTask::run()
{
    if (cancelled->load()) return; // caller already returned 504; do nothing
    if (s_backup_in_progress) {    // parked until set_backup_in_progress(false)
        s_deferred_ui_tasks.push_back(shared_from_this());
        return;
    }
    ++s_api_ui_task_depth;
    // Push the next periodic backup out of the mutation window so it doesn't
    // fire between the calls of an API burst.
    Slic3r::backup_defer(3);
    try {
        promise->set_value(fn());
    } catch (...) {
        promise->set_exception(std::current_exception());
    }
    --s_api_ui_task_depth;
}

} // namespace

bool Controller::api_ui_task_active() { return s_api_ui_task_depth > 0; }

void Controller::set_backup_in_progress(bool v)
{
    s_backup_in_progress = v;
    if (!v && !s_deferred_ui_tasks.empty()) {
        auto parked = std::move(s_deferred_ui_tasks);
        s_deferred_ui_tasks.clear();
        for (auto &t : parked)
            t->schedule(); // re-queued, not run inline: let the export unwind first
    }
}

nlohmann::json Controller::run_on_ui(std::function<nlohmann::json()> fn, int timeout_s)
{
    auto task       = std::make_shared<UiTask>();
    task->promise   = std::make_shared<std::promise<nlohmann::json>>();
    // Set if the io side gives up (10s timeout) before the GUI lambda runs. The
    // lambda checks it at the top, so a timed-out request does NOT still apply
    // its side effects (PUT config / reslice) on the GUI thread afterwards.
    task->cancelled = std::make_shared<std::atomic<bool>>(false);
    task->fn        = std::move(fn);
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
    nlohmann::json j = run_on_ui([this]() -> nlohmann::json {
        auto *bundle = wxGetApp().preset_bundle;
        auto *plater = wxGetApp().plater();

        nlohmann::json objects = nlohmann::json::array();
        for (const ModelObject *mo : plater->model().objects) {
            auto sz = mo->bounding_box_exact().size();
            objects.push_back({{"name", mo->name},
                               {"size_mm", {sz.x(), sz.y(), sz.z()}}});
        }
        auto plate_valid = plater->get_partplate_list().get_curr_plate()->is_slice_result_valid();
        return {
            {"app", SLIC3R_APP_NAME},
            {"app_version", SoftFever_VERSION},
            {"api_version", "1.0"},
            {"capabilities", {"status", "config", "slice", "events", "model", "preset", "gcode", "objects", "arrange", "orient", "object_config", "slice_breakdown", "plate_render"}},
            {"project", plater->get_project_filename().ToUTF8().data()},
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

        nlohmann::json applied = nlohmann::json::array();
        nlohmann::json errors  = nlohmann::json::object();
        std::vector<std::array<std::string, 3>> changes; // key, old, new -> notification

        for (auto it = in.begin(); it != in.end(); ++it) {
            const std::string &key = it.key();
            Target *tgt = nullptr;
            for (auto &t : targets)
                if (t.cfg.option(key) != nullptr) { tgt = &t; break; }
            if (tgt == nullptr) {
                // F9: distinguish a real typo from a recognized Orca setting that
                // simply isn't editable through the preset configs (project/plate/
                // computed-layer keys: wipe tower position, flush volumes, AMS maps...
                // - GET /config serializes the merged config, which is wider).
                errors[key] = (print_config_def.get(key) != nullptr)
                                  ? "not_editable_in_current_config"
                                  : "unknown_key";
                continue;
            }
            try {
                std::string oldv = tgt->cfg.opt_serialize(key);
                std::string sval = json_value_to_config_string(it.value());
                // Orca's own validation: throws BadOptionTypeException /
                // BadOptionValueException on garbage.
                tgt->cfg.set_deserialize_strict(key, sval);
                tgt->touched = true;
                applied.push_back(key);
                changes.push_back({ key, oldv, tgt->cfg.opt_serialize(key) });
            } catch (const std::exception &e) {
                errors[key] = e.what();
            }
        }

        // Atomic: if any key failed validation, apply NOTHING and report errors.
        // (Previously, valid keys in a mixed batch applied before the 422.)
        if (!errors.empty()) {
            std::string emsg = "Couldn't update settings";
            for (auto it2 = errors.begin(); it2 != errors.end(); ++it2) {
                emsg += "\n - ";
                emsg += api_config_label(it2.key());
            }
            api_notify(emsg, true);
            return {{"applied", nlohmann::json::array()}, {"errors", errors}};
        }

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
    });

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
        if (evt.error()) {
            auto msg = evt.format_error_message();
            set_slice_state([&](SliceState &s) {
                s.state = "error"; s.percent = -1; s.message = msg.first;
                s.stats = nullptr; s.warnings = nlohmann::json::array();
            }, "slice.error");
            return;
        }
        if (evt.cancelled()) {
            set_slice_state([&](SliceState &s) {
                s.state = "idle"; s.percent = -1; s.message = "cancelled";
            }, "slice.cancelled");
            return;
        }
        // Success: harvest stats on the GUI thread (we ARE on it - wx handler).
        nlohmann::json stats, warnings = nlohmann::json::array();
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
        set_slice_state([&](SliceState &s) {
            s.state = "done"; s.percent = 100; s.message = "";
            s.stats = stats; s.warnings = warnings;
        }, "slice.done");
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
        if (plater->get_partplate_list().get_curr_plate()->is_slice_result_valid())
            return {{"started", false}, {"already_valid", true}};
        set_slice_state([](SliceState &s) {
            s.state = "slicing"; s.percent = 0; s.message = "starting";
            s.stats = nullptr; s.warnings = nlohmann::json::array();
        }, "slice.started");
        plater->reslice();
        // F2: reslice() bails out synchronously - with NO completion event -
        // when the plate can't be sliced (update_background_process() INVALID,
        // e.g. an object fully outside the bed; or a prior hard error pinned on
        // the plate). m_is_slicing is only set on the started paths, so if it
        // is false here nothing is coming and the state would wedge at
        // "slicing" forever. Flip it to error instead.
        if (!plater->is_background_process_slicing()) {
            set_slice_state([](SliceState &s) {
                s.state = "error"; s.percent = -1;
                s.message = "plate cannot be sliced (object outside bed or invalid state)";
                s.stats = nullptr; s.warnings = nlohmann::json::array();
            }, "slice.error");
            return {{"error", "slice_not_started"}};
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
        if (plater != nullptr)
            plater->stop_background_slicing();
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
        return { 422, {{"error", "unsupported_format"},
                       {"detail", "remote load supports .stl, .obj, .3mf, .step, .stp"}} };
    }

    // 120 s instead of the default 10: OCCT read + tessellation of a real-world
    // STEP (and mesh import of a very large STL) can exceed 10 s, and the load
    // would still complete on the GUI thread after a ui_timeout was returned.
    nlohmann::json r = run_on_ui([path, is_step]() -> nlohmann::json {
        if (!boost::filesystem::exists(path))
            return {{"error", "not_found"}};
        Plater *plater = wxGetApp().plater();
        // Use the stored deflection defaults instead of StepMeshDialog and skip
        // the non-UTF8 name warning while loading; restore the user's flags after.
        AppConfig *cfg = wxGetApp().app_config;
        const bool prev_mesh_dlg  = cfg->get_bool("enable_step_mesh_setting");
        const bool prev_utf8_warn = cfg->get_bool("step_not_utf8_no_warn");
        if (is_step) {
            cfg->set_bool("enable_step_mesh_setting", false);
            cfg->set_bool("step_not_utf8_no_warn", true);
        }
        std::vector<size_t> idxs = plater->load_files(std::vector<std::string>{ path },
                                                      LoadStrategy::LoadModel);
        if (is_step) {
            cfg->set_bool("enable_step_mesh_setting", prev_mesh_dlg);
            cfg->set_bool("step_not_utf8_no_warn", prev_utf8_warn);
        }
        if (idxs.empty())
            return {{"error", "load_failed"}};
        nlohmann::json objects = nlohmann::json::array();
        for (size_t i : idxs) {
            const ModelObject *mo = plater->model().objects[i];
            auto sz = mo->bounding_box_exact().size();
            objects.push_back({{"index", i}, {"name", mo->name},
                               {"size_mm", {sz.x(), sz.y(), sz.z()}}});
        }
        return {{"loaded", true}, {"objects", objects}};
    }, /*timeout_s=*/120);
    std::string fn = boost::filesystem::path(path).filename().string();
    if (r.contains("error")) {
        api_notify("Couldn't load " + fn, true);
        if (r["error"] == "not_found") return { 404, r };
        return { 422, r };
    }
    api_notify("Added " + fn + " to the plate");
    return { 200, r };
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

    nlohmann::json r = run_on_ui([type, name]() -> nlohmann::json {
        auto *bundle = wxGetApp().preset_bundle;
        PresetCollection &presets = type == Preset::TYPE_PRINT    ? bundle->prints :
                                    type == Preset::TYPE_FILAMENT ? bundle->filaments :
                                                                    bundle->printers;
        // Validate up front: select_preset silently falls back to a visible preset
        // for an unknown name, so its return can't be trusted for a 422.
        if (presets.find_preset(name, false, true) == nullptr)
            return {{"error", "unknown_preset"}};
        Tab *tab = wxGetApp().get_tab(type);
        if (tab == nullptr) return {{"error", "tab_unavailable"}};
        // Discard un-applied GUI edits on EVERY collection Tab::select_preset may
        // inspect. It consults dependent tabs too (switching a print/printer preset
        // checks the filament/print collections' dirty state), and ANY dirty one
        // pops a modal UnsavedChangesDialog that would wedge the GUI thread with no
        // remote way to dismiss it. Discarding is acceptable for an automation API
        // (callers apply changes deliberately via PUT /config).
        auto discard_if_dirty = [](PresetCollection &c) {
            if (c.current_is_dirty()) c.discard_current_changes();
        };
        discard_if_dirty(bundle->prints);
        discard_if_dirty(bundle->filaments);
        discard_if_dirty(bundle->sla_materials);
        discard_if_dirty(bundle->printers);
        bool ok = tab->select_preset(name, false, "", /*force_select=*/true, /*force_no_transfer=*/true);
        if (!ok) return {{"error", "select_cancelled"}};
        return {{"selected", name}};
    });
    if (r.contains("error")) {
        if (r["error"] == "unknown_preset") {
            api_notify("No such " + type_s + " preset '" + name + "'", true);
            return { 422, r };
        }
        return { 500, r };
    }
    api_notify("Switched to " + type_s + " preset '" + name + "'");
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
        const Preset *existing = presets.find_preset(name, false);
        if (existing != nullptr && (existing->is_system || existing->is_default))
            return {{"error", "name_reserved"}};
        Tab *tab = wxGetApp().get_tab(type);
        if (tab == nullptr) return {{"error", "tab_unavailable"}};
        bool created = existing == nullptr;
        tab->save_preset(name, detach, /*save_to_project=*/false);
        // save_preset returns void; confirm the preset actually landed.
        const Preset *saved = presets.find_preset(name, false);
        if (saved == nullptr) return {{"error", "save_failed"}};
        return {{"saved", name}, {"created", created}};
    });
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
        Plater *plater = wxGetApp().plater();
        int idx = find_object_index(plater->model(), id);
        if (idx < 0) return {{"error", "unknown_object"}};
        ModelObject *mo = plater->model().objects[idx];
        if (mode == "reset") {
            mo->layer_height_profile.clear();
            plater->schedule_background_process();
            wxGetApp().obj_list()->update_info_items(idx);
            api_notify("Reset layer height profile on '" + mo->name + "'");
            return {{"id", id}, {"mode", "reset"}};
        }
        const DynamicPrintConfig full_cfg = wxGetApp().preset_bundle->full_config();
        // object_max_z 0 -> computed from the object's raw bounding box inside.
        SlicingParameters sp = PrintObject::slicing_parameters(
            full_cfg, *mo, 0.f, plater->fff_print().shrinkage_compensation());
        std::vector<double> profile = layer_height_profile_adaptive(sp, *mo, (float) quality);
        if (profile.size() < 4)
            return {{"error", "profile_failed"}};
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
        return {{"id", id}, {"mode", "adaptive"}, {"quality", quality},
                {"points", (unsigned) (prof.size() / 2)},
                {"layer_height_min", hmin}, {"layer_height_max", hmax}};
    });
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
        Plater *plater = wxGetApp().plater();
        int idx = find_object_index(plater->model(), id);
        if (idx < 0) return {{"error", "unknown_object"}};
        ModelObject *mo = plater->model().objects[idx];
        auto ranges_json = [mo]() {
            nlohmann::json a = nlohmann::json::array();
            for (const auto &kv : mo->layer_config_ranges)
                a.push_back({{"min_z", kv.first.first}, {"max_z", kv.first.second},
                             {"layer_height", kv.second.has("layer_height") ? kv.second.opt_float("layer_height") : 0.0}});
            return a;
        };
        if (clear) {
            mo->layer_config_ranges.clear();
            plater->changed_object(idx);
            wxGetApp().obj_list()->update_info_items(idx);
            api_notify("Cleared height ranges on '" + mo->name + "'");
            return {{"id", id}, {"height_ranges", ranges_json()}};
        }
        // Printer limits, same rules as ObjectList's get_min/max_layer_height.
        const DynamicPrintConfig &pcfg = wxGetApp().preset_bundle->printers.get_edited_preset().config;
        double minh = pcfg.opt_float("min_layer_height", 0);
        double maxh = pcfg.opt_float("max_layer_height", 0);
        if (maxh < EPSILON) maxh = 0.75 * pcfg.opt_float("nozzle_diameter", 0);
        if (lh < minh || lh > maxh)
            return {{"error", "layer_height_out_of_range"}, {"min", minh}, {"max", maxh}};
        t_layer_height_range key{ min_z, max_z };
        for (const auto &kv : mo->layer_config_ranges) {
            if (kv.first == key) continue;
            if (min_z < kv.first.second && kv.first.first < max_z)
                return {{"error", "overlaps_existing"},
                        {"existing", {kv.first.first, kv.first.second}}};
        }
        DynamicPrintConfig c;
        c.set_key_value("layer_height", new ConfigOptionFloat(lh));
        c.set_key_value("extruder",     new ConfigOptionInt(0));
        mo->layer_config_ranges[key].assign_config(std::move(c));
        plater->changed_object(idx);
        wxGetApp().obj_list()->update_info_items(idx);
        api_notify("Set height range " + std::to_string(min_z) + "-" + std::to_string(max_z) + " mm on '" + mo->name + "'");
        return {{"id", id}, {"height_ranges", ranges_json()}};
    });
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
        for (const std::string &k : p->config.keys()) cfg[k] = p->config.opt_serialize(k);
        return {{"name", p->name}, {"system", p->is_system}, {"config", cfg}};
    });
    if (r.contains("error")) {
        if (r["error"] == "unknown_type") return { 400, r };
        return { 404, r };
    }
    return { 200, r };
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
        PresetCollection *presets = api_preset_collection(type_s);
        const Preset *p = presets->find_preset(name, false);
        if (p == nullptr) return {{"error", "unknown_preset"}};
        if (p->is_system || p->is_default) return {{"error", "builtin_preset"}};
        if (presets->get_selected_preset_name() == name)
            return {{"error", "preset_selected"},
                    {"detail", "select another preset first, then delete"}};
        for (const Preset &other : presets->get_presets())
            if (other.name != name && other.inherits() == name)
                return {{"error", "has_children"},
                        {"detail", "other presets inherit from this one"}};
        if (!presets->delete_preset(name))
            return {{"error", "delete_failed"}};
        // Refresh the tab's preset list + the plater sidebar combo.
        if (Tab *tab = wxGetApp().get_tab(type); tab != nullptr)
            tab->update_tab_ui();
        wxGetApp().plater()->sidebar().update_presets(type);
        api_notify("Deleted " + type_s + " preset '" + name + "'");
        return {{"deleted", name}};
    });
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
        if (idx < 0) { api_notify("Object not found", true); return {{"error", "unknown_object"}}; }
        std::string oname = plater->model().objects[idx]->name;
        plater->remove((size_t) idx);
        api_notify("Removed '" + oname + "'");
        return {{"deleted", true}, {"id", id}, {"count", plater->model().objects.size()}};
    });
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
        Plater *plater = wxGetApp().plater();
        int idx = find_object_index(plater->model(), id);
        if (idx < 0) { api_notify("Object not found", true); return {{"error", "unknown_object"}}; }
        ModelObject *mo = plater->model().objects[idx];
        if (mo->instances.empty()) return {{"error", "no_instance"}};
        ModelInstance *mi = mo->instances.front();
        if (has_t) mi->set_offset(mi->get_offset() + tr);
        if (has_r) mi->set_rotation(mi->get_rotation() + ro * 0.017453292519943295); // deg->rad
        if (has_s) mi->set_scaling_factor(sc);
        plater->changed_object(idx);
        api_notify(std::string(has_t ? "Moved" : (has_r ? "Rotated" : "Resized")) + " '" + mo->name + "'");
        auto off = mi->get_offset(); auto rot = mi->get_rotation(); auto scl = mi->get_scaling_factor();
        return {{"id", id}, {"transform", {
            {"offset",   {off.x(), off.y(), off.z()}},
            {"rotation", {rot.x(), rot.y(), rot.z()}},
            {"scale",    {scl.x(), scl.y(), scl.z()}}}}};
    });
    if (r.contains("error")) return { 404, r };
    return { 200, r };
}

Response Controller::handle_duplicate_object(uint64_t id)
{
    nlohmann::json r = run_on_ui([id]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        Model &model = plater->model();
        int idx = find_object_index(model, id);
        if (idx < 0) { api_notify("Object not found", true); return {{"error", "unknown_object"}}; }
        ModelObject *mo = model.objects[idx];
        if (mo->instances.empty()) return {{"error", "no_instance"}};
        const ModelInstance *src = mo->instances.front();
        Vec3d off = src->get_offset() + Vec3d(10.0, 10.0, 0.0);
        mo->add_instance(off, src->get_scaling_factor(), src->get_rotation(), src->get_mirror());
        plater->changed_object(idx);
        api_notify("Duplicated '" + mo->name + "'");
        return {{"duplicated", true}, {"id", id}, {"instances", (unsigned) mo->instances.size()}};
    });
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
        if (idx < 0) return {{"error", "unknown_object"}};
        ModelObject *mo = plater->model().objects[idx];
        DynamicPrintConfig cfg = mo->config.get(); // copy of existing per-object overrides
        nlohmann::json applied = nlohmann::json::array();
        nlohmann::json errors  = nlohmann::json::object();
        for (auto it = in.begin(); it != in.end(); ++it) {
            const std::string &key = it.key();
            if (print_config_def.get(key) == nullptr) { errors[key] = "unknown_key"; continue; }
            try {
                cfg.set_deserialize_strict(key, json_value_to_config_string(it.value()));
                applied.push_back(key);
            } catch (const std::exception &e) { errors[key] = e.what(); }
        }
        if (!errors.empty()) { // atomic: apply nothing on any error
            api_notify(std::string("Couldn't set object settings on '") + mo->name + "'", true);
            return {{"applied", nlohmann::json::array()}, {"errors", errors}};
        }
        mo->config.assign_config(std::move(cfg));
        plater->changed_object(idx);
        if (!applied.empty())
            api_notify("Updated " + std::to_string(applied.size()) + " setting(s) on '" + mo->name + "'");
        return {{"applied", applied}, {"errors", errors}, {"object", mo->name}};
    });
    if (r.contains("error")) return { 404, r };
    return { r["errors"].empty() ? 200 : 422, r };
}

Response Controller::handle_arrange()
{
    nlohmann::json r = run_on_ui([]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        if (plater->model().objects.empty()) return {{"error", "empty"}};
        if (!plater->get_ui_job_worker().is_idle()) return {{"busy", true}};
        plater->arrange();
        return {{"started", true}};
    });
    if (r.contains("error")) return { 422, r };
    if (r.value("busy", false)) return { 409, {{"error", "job_running"}} };
    return { 202, r };
}

Response Controller::handle_orient()
{
    nlohmann::json r = run_on_ui([]() -> nlohmann::json {
        Plater *plater = wxGetApp().plater();
        if (plater->model().objects.empty()) return {{"error", "empty"}};
        if (!plater->get_ui_job_worker().is_idle()) return {{"busy", true}};
        plater->orient();
        return {{"started", true}};
    });
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

Response Controller::dispatch(const Request &req)
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
        if (is("GET", "/api/v1/status"))        return handle_status();
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
