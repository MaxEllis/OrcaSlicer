// src/slic3r/GUI/RemoteAPI/RemoteAPIController.hpp
#ifndef slic3r_RemoteAPIController_hpp_
#define slic3r_RemoteAPIController_hpp_

#include "RemoteAPIServer.hpp"

#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace Slic3r { namespace ElegooCanvas { struct Info; } }
namespace Slic3r { class DynamicPrintConfig; class ConfigOption; }
namespace Slic3r { namespace GUI { class Plater; } }

namespace Slic3r { namespace GUI { namespace RemoteAPI {

// Snapshot of the last/current slice, maintained on the GUI thread by wx event
// subscriptions (Task 10), read by API threads under the mutex.
struct SliceState
{
    std::string    state { "idle" };  // idle | slicing | done | error
    int            percent { -1 };
    std::string    message;           // progress text or error message
    nlohmann::json stats;             // filled on success (Task 10)
    nlohmann::json warnings = nlohmann::json::array();
};

class Controller
{
public:
    // Must be constructed on the GUI thread (binds wx events in Task 10).
    Controller();
    ~Controller();

    Response dispatch(const Request &req);

    // GUI-code hooks, callable even when the API is disabled (no-op when no
    // controller exists). preset_type is int to keep Tab.cpp include-light.
    static void notify_config_changed(int preset_type);
    static void notify_project_opened();
    // End of Plater::new_project (the user-change journal's "New project"). GUI thread.
    static void notify_project_new();
    // The script, G-code and connection keys the policy guards (the journal never shows their values). Any thread.
    static bool sensitive_config_key(const std::string &key);

    // F8 mutual-exclusion gates between API UI mutations and the periodic
    // auto-backup 3MF exporter (which pumps the event queue mid-export, so the
    // two can interleave on the GUI thread and corrupt what is being written).
    // GUI thread only.
    static bool api_ui_task_active();          // an API task is on the stack
    static void set_backup_in_progress(bool);  // MainFrame brackets export_3mf;
                                               // false flushes deferred API tasks
                                               // (nests: a depth count, so an inner
                                               // false never clears an outer bracket)
    static bool backup_in_progress();          // an export is bracketed right now
    // An API request has started and its journal end step has not run yet (UiTaskGate.hpp RequestsInFlight). Any thread.
    static bool requests_in_flight();
    // The project-scope keys PUT /config writes (curr_bed_type, filament_colour, flush volumes and multipliers,
    // prime_volume_mode, wipe_tower_x/y). The user-change journal compares them too.
    static std::vector<std::string> writable_project_keys();

    // OrcaSlicer's own range for a setting (ConfigRange.hpp: ConfigOptionDef min / max, the "mm or %" fields' literal
    // limit, every element of a vector). after = the value about to be written, before = the value there now
    // (nullptr: every element is judged; elements equal to it are not, so writing a value back never fails). false
    // when out of range, with detail = {"error":"out_of_range","key","value","min","max","index"?,"detail"} (min /
    // max null when that side has no limit) and text = "<label> must be between A and B (got C)". Any thread.
    static bool config_value_in_range(const std::string &key, const ConfigOption *before, const ConfigOption &after,
                                      nlohmann::json &detail, std::string &text);

    SliceState slice_state() const { std::lock_guard<std::mutex> lk(m_mutex); return m_slice; }

    // ---- Shared by the API handlers and the MCP panel's actions (McpActions). GUI thread only. -------------------
    // POST /project/open's work once the request is read: checks (file, slicing, loading), the sensitive-key check
    // when policy_check, before_load (may add to the reply, or return {"error", ...} to stop), then the open with
    // OrcaSlicer's prompts answered and, with keep_presets, the earlier presets selected back. Returns the reply's
    // fields or {"error", ...}. Records nothing in the MCP panel.
    static nlohmann::json open_project_from(Plater *plater, const std::string &path, bool keep_presets, bool policy_check,
                                            const std::function<nlohmann::json(nlohmann::json &)> &before_load = {});
    // The project-scope epilogue of PUT /config: applies the staged project config; proj_keys = keys changed,
    // colours_before = filament_colour before (for the per-slot colour follow-ups).
    static void apply_project_config(const DynamicPrintConfig &proj_new, const std::set<std::string> &proj_keys,
                                     const std::vector<std::string> &colours_before);
    // The filament preset of one slot, as the sidebar combo switches it (slot and preset already checked).
    static void select_filament_slot(size_t slot, const std::string &name);
    // DELETE /preset's rules ("" = may delete, else the error token) and its deletion. type: print|filament|printer.
    static std::string preset_delete_refusal(const std::string &type, const std::string &name);
    static bool        delete_user_preset(const std::string &type, const std::string &name);
    // Drops the File > Recent entries that name this file.
    static void forget_recent(const std::string &path_utf8);
    // /slice/status describes the current plate again (after a restore replaced the project).
    static void slice_state_follows_plate();
    // POST /project/move undone: moves `moved_to` back to `moved_from` (only onto a free name), with the window,
    // recent list and MCP history following as for the move. "" or an error token (file_not_found, exists,
    // same_path, move_failed).
    static std::string move_project_file_back(const std::string &moved_from, const std::string &moved_to);
    // CANVAS: the tray a filament prints from now, as the slot-mapping rows name it ("A2", "none"); false when no
    // slot list has been read. revert_slot_mapping puts one filament's earlier tray back ("" or an error token).
    static bool        slot_mapping_label(int filament, std::string &label);
    static std::string revert_slot_mapping(const nlohmann::json &target);

    // Call once on the GUI thread after the plater exists (from start_remote_api()).
    // Idempotent: guarded by m_events_bound.
    void bind_plater_events();

private:
    // dispatch() runs the user-change journal around this (diff before, baseline after).
    Response dispatch_route(const Request &req);
    void     journal_begin();
    void     journal_end();
    Response handle_user_changes(const std::string &target); // GET /user_changes?since=
    Response handle_status();
    Response handle_get_config(const std::string &target);
    Response handle_put_config(const std::string &body);
    Response handle_slice();
    Response handle_slice_status();
    Response handle_slice_cancel();   // F2: POST /slice/cancel - unwedge/abort
    Response handle_load_model(const std::string &body);   // M4a: POST /model
    Response handle_select_preset(const std::string &body); // M4a: PUT /preset
    Response handle_save_preset(const std::string &body);   // POST /preset/save
    Response handle_get_presets();                          // GET /presets
    Response handle_get_preset_config(const std::string &body); // POST /preset/config (read named preset)
    Response handle_delete_preset(const std::string &body);      // DELETE /preset
    Response handle_put_layer_height(uint64_t id, const std::string &body);  // M4c: PUT /objects/{id}/layer_height
    Response handle_put_height_range(uint64_t id, const std::string &body);  // M4c: PUT /objects/{id}/height_range
    Response handle_get_gcode();                            // M4a: GET /gcode (raw body)
    Response handle_plate_render(const std::string &target); // GET /plate/render (PNG: editor|preview)
    Response handle_get_objects();                          // M4b: GET /objects
    Response handle_delete_object(uint64_t id);             // M4b: DELETE /objects/{id}
    Response handle_transform_object(uint64_t id, const std::string &body); // M4b: POST /objects/{id}/transform
    Response handle_duplicate_object(uint64_t id);          // M4b: POST /objects/{id}/duplicate
    Response handle_put_object_config(uint64_t id, const std::string &body); // M4c: PUT /objects/{id}/config
    Response handle_arrange();                              // M4b: POST /arrange (async)
    Response handle_orient();                               // M4b: POST /orient (async)
    Response handle_jobs_status();                          // M4b: GET /jobs/status
    Response handle_get_plates();                           // plates: GET /plates
    Response handle_create_plate(const std::string &body);  // plates: POST /plates
    Response handle_select_plate(int index);                // plates: POST /plates/{i}/select
    Response handle_delete_plate(int index);                // plates: DELETE /plates/{i}
    Response handle_put_object_plate(uint64_t id, const std::string &body); // plates: POST /objects/{id}/plate
    Response handle_get_project();                                  // project: GET /project
    Response handle_save_project(const std::string &body);          // project: POST /project/save
    Response handle_open_project(const std::string &body);          // project: POST /project/open
    Response handle_new_project(const std::string &body);           // project: POST /project/new
    Response handle_get_recent_projects();                          // project: GET /project/recent
    Response handle_forget_recent_project(const std::string &body); // project: DELETE /project/recent
    Response handle_browse_projects(const std::string &target);     // project: GET /project/browse?dir=
    Response handle_move_project_file(const std::string &body);     // project: POST /project/move
    Response handle_delete_project_file(const std::string &body);   // project: DELETE /project/file
    // MCP panel: POST /activity/test/<name> (only with ORCA_MCP_PANEL_TEST=1 at start)
    Response handle_activity_test(const std::string &name, const std::string &body);
    Response handle_printer_slots();                        // CANVAS: GET /printer/slots
    Response handle_get_slot_mapping();                     // CANVAS: GET /printer/slot_mapping
    Response handle_put_slot_mapping(const std::string &body); // CANVAS: PUT /printer/slot_mapping

    // CANVAS: the slot list for the selected printer preset. use_cached = take CanvasState::last_info when there is
    // one, else read the printer (MQTT on the calling io thread, never on the GUI thread) and store a good read in
    // last_info. Returns false with `unsupported` = {"supported":false,"reason","message"} when the preset is not a
    // gated CC2, the read fails or no CANVAS is connected.
    bool read_canvas_slots(bool use_cached, Slic3r::ElegooCanvas::Info &info, nlohmann::json &unsupported);

    // Mutate m_slice under the lock and broadcast a snapshot (event_name) to WS clients.
    void set_slice_state(const std::function<void(SliceState&)> &mut, const char *event_name);
    // plates: make the stored slice state describe the current plate (GUI thread).
    void sync_slice_state_to_current_plate(const char *event_name);

    // Debounced config.changed emitter (GUI thread; mutex guards the pending set).
    void on_config_changed(int preset_type);

    // Runs fn on the GUI thread, blocks the calling (io) thread up to
    // timeout_s seconds (default 10). Throws std::runtime_error("ui_timeout")
    // on expiry.
    nlohmann::json run_on_ui(std::function<nlohmann::json()> fn, int timeout_s = 10);

    mutable std::mutex m_mutex;
    SliceState         m_slice;
    bool               m_events_bound { false };
    // MCP panel: a slice started by POST /slice is running; its completion records the slice row. GUI thread.
    bool               m_api_slice { false };

    std::mutex         m_cc_mutex;
    std::set<int>      m_cc_pending;
    bool               m_cc_timer_armed { false };
};

}}} // namespace

#endif
