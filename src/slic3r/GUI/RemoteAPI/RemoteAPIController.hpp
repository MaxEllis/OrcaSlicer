// src/slic3r/GUI/RemoteAPI/RemoteAPIController.hpp
#ifndef slic3r_RemoteAPIController_hpp_
#define slic3r_RemoteAPIController_hpp_

#include "RemoteAPIServer.hpp"

#include <functional>
#include <mutex>
#include <set>

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

    // F8 mutual-exclusion gates between API UI mutations and the periodic
    // auto-backup 3MF exporter (which pumps the event queue mid-export, so the
    // two can interleave on the GUI thread and corrupt what is being written).
    // GUI thread only.
    static bool api_ui_task_active();          // an API task is on the stack
    static void set_backup_in_progress(bool);  // MainFrame brackets export_3mf;
                                               // false flushes deferred API tasks

    SliceState slice_state() const { std::lock_guard<std::mutex> lk(m_mutex); return m_slice; }

    // Call once on the GUI thread after the plater exists (from start_remote_api()).
    // Idempotent: guarded by m_events_bound.
    void bind_plater_events();

private:
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

    // Mutate m_slice under the lock and broadcast a snapshot (event_name) to WS clients.
    void set_slice_state(const std::function<void(SliceState&)> &mut, const char *event_name);

    // Debounced config.changed emitter (GUI thread; mutex guards the pending set).
    void on_config_changed(int preset_type);

    // Runs fn on the GUI thread, blocks the calling (io) thread up to
    // timeout_s seconds (default 10). Throws std::runtime_error("ui_timeout")
    // on expiry.
    nlohmann::json run_on_ui(std::function<nlohmann::json()> fn, int timeout_s = 10);

    mutable std::mutex m_mutex;
    SliceState         m_slice;
    bool               m_events_bound { false };

    std::mutex         m_cc_mutex;
    std::set<int>      m_cc_pending;
    bool               m_cc_timer_armed { false };
};

}}} // namespace

#endif
