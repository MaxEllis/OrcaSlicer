#pragma once
// Recorder behind the OrcaSlicer MCP dock panel: turns Remote API changes into McpHistory rows, takes the
// restore-point checkpoints, and keeps the history per project in <data_dir>/mcp_history/<key>/.
// GUI thread only (every member except the static test setters and note_project_opened).
//
// ---- Row text and the target schema, consumed by McpActions -------------------------------------------------
// The API handlers (RemoteAPIController.cpp) record one row per change. `kind` is one of setting, preset,
// model, plate, slice, file, refused. `value` and `before` of setting rows are config_value_text() of the raw
// values (unit added, enum label, first line only, cut at 40 bytes): a live value compared with `value` must be
// formatted the same way. `target` is host-only (never sent to the page) and says what the row changed:
//
//   {"type":"config", "key", "scope":"print|filament|printer|project", "preset": <edited preset name>
//    (preset scopes only), "index": n (only for one element of a vector: filament_colour = filament slot,
//    wipe_tower_x/y = plate index), "before_raw", "after_raw"}
//      PUT /config. Raw = ConfigOption::serialize() of the whole option, or of element `index`. No raws for a
//      credential key or a host URL holding a login (those rows have no undo).
//   {"type":"object_config", "object_id", "key", "had_override": bool, "before_raw" (the old override, or the
//    merged config's value when there was none), "after_raw"}                      PUT /objects/{id}/config
//   {"type":"layer_profile", "object_id", "before": [doubles] (empty = no custom profile), "after": [doubles]}
//                                                                                   PUT /objects/{id}/layer_height
//   {"type":"height_range", "object_id", "min_z", "max_z", "before": null | {key: raw}, "after": {key: raw}}
//      one range set (before null: the range is new; else the range's earlier config)
//   {"type":"height_ranges_cleared", "object_id", "ranges": [{"min_z","max_z","config":{key: raw}}]}
//                                                                                   PUT /objects/{id}/height_range
//   {"type":"added", "object_id"}                                                   POST /model, one per object
//   {"type":"removed", "object_id" (the gone id), "stash", "stash_session", "plate"}  DELETE /objects/{id};
//      stashed_object(stash, stash_session) is the full copy (null after a restart or once dropped)
//   {"type":"transform", "object_id", "instance", "instance_id", "before": [16], "after": [16],
//    "plate_before", "plate_after"}                                               POST /objects/{id}/transform
//   {"type":"duplicate", "object_id", "instance", "instance_id", "after": [16], "plate"}  POST .../duplicate
//   {"type":"instances", "op":"arrange|orient", "plates_before", "plates_after", "items": [{"object_id",
//    "instance", "instance_id", "plate_before", "plate_after", "before": [16], "after": [16]}]}
//      POST /arrange, POST /orient; recorded when the job has finished, items = instances that moved
//   {"type":"instance_plate", "object_id", "instance", "instance_id", "plate_before", "plate_after",
//    "before": [16], "after": [16]}                                                POST /objects/{id}/plate
//   {"type":"plate_added", "index"}   {"type":"plate_deleted", "index", "objects_moved_off": n}
//   {"type":"preset", "preset_type":"print|filament|printer", "slot": n | null, "before", "after",
//    "discarded": ["print", ...]}                                                  PUT /preset
//   {"type":"preset_saved", "preset_type", "name", "created": bool}                POST /preset/save
//   {"type":"preset_deleted", "preset_type", "name"}                               DELETE /preset
//   {"type":"project_opened", "path"}  {"type":"project_new", "name"}  {"type":"project_saved", "path"}
//   {"type":"file_moved", "from", "to", "was_open"}  {"type":"file_deleted", "path"}
//   {"type":"slot_mapping", "filament", "before_tray": {"canvas_id","tray_id"} | null, "after_tray": ... | null,
//    "api_mapping_before": [{"filament","canvas_id","tray_id"}], "project_key"}    PUT /printer/slot_mapping
//   {"type":"refused", "action"}                                                   any refused change
// Matrices: ModelInstance::get_matrix() as 16 doubles, column-major (Eigen's matrix().data() order).
// object_id / instance_id: ObjectBase::id().id. Slice rows (slice_finished) carry no target.
// Object targets (and each arrange/orient item) also carry "object_name", and the target "load" = load_stamp() at
// recording: ObjectIDs are renewed by every project load, so after one the actions find the object by its name
// (mcp_find_object in McpHistory.hpp).
#include "McpHistory.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace Slic3r { class Model; class ModelObject; class ModelInstance; }

namespace Slic3r { namespace GUI { namespace RemoteAPI {

class McpRecorder {
public:
    static constexpr int64_t kCheckpointMaxS = 30;          // a slower checkpoint is dropped
    static constexpr int64_t kStaleAgeS      = 30 * 86400;  // history dirs unused this long are deleted

    static McpRecorder &get();

    // Before a mutating API handler changes anything: burst check, and on a new group the checkpoint of the
    // state now (the previous group's end state, or the start point), then prune. Calling it again within
    // the same burst is a no-op.
    void before_change();
    // Adds a row (ts, time, id and group are filled in). Opens a group itself (with its checkpoint) only when none
    // is open: no group yet, or the last one was closed by a slice result. Returns the row id. The history file is
    // written, and the listener called, once for all the rows of a request: from a posted flush() after the
    // current GUI task.
    int  record(McpRow row);
    // A change the API refused: kind "refused", `value` ("Not allowed" for a policy block, else a short reason),
    // where = why. It joins the open group (within the burst); otherwise it opens a group WITHOUT a checkpoint
    // (nothing changed), and the first real change of that group takes the skipped checkpoint then.
    void record_refusal(std::string label, std::string why, std::string value = "Not allowed",
                        nlohmann::json target = nullptr);
    // An API slice ended: a "slice" row in the current group, then the group closes. ok=false with
    // error "cancelled" records "Slicing cancelled"; any other failure records "Slicing failed" (error row).
    // When no group is open (the last one was closed by a slice) it opens one WITHOUT a checkpoint: this runs
    // from the slicing-completed event, outside any API task.
    // stats = the GET /slice/status stats (estimated_time_seconds, filament_used_g).
    void slice_finished(const nlohmann::json &stats, bool ok, const std::string &error);
    // A note from the assistant's connector (POST /activity/note, M4): kind "slice" adds one slice-kind row (label =
    // text, title_hint "note": it names its group when the group holds no change) and closes the group like a slice
    // result ("Compared 3 options · nothing kept" after a comparison whose trial requests recorded nothing); kind
    // "refused" adds a refusal the connector made itself (label = text, value, where = detail), as record_refusal.
    // Changes nothing in the project; no checkpoint.
    void record_note(const std::string &kind, const std::string &text, const std::string &value, const std::string &where);

    // Does nothing while a project is loading (Plater::is_loading_project). Follows the open project: loads its history on an open, rekeys untitled/ on the first save of an
    // Untitled project, starts an empty history on a Save As. Called by the members above and by the pane.
    void sync_project();
    // An API move/rename of a project file: its history dir moves with it (also when it is the open project).
    // Call right after the file moved and BEFORE recording the row for it.
    void project_moved(const std::string &old_path_utf8, const std::string &new_path_utf8);

    // A panel restore: the next change starts a group of its own (and takes the checkpoint of the restored state
    // for the newest point, unless that point already has one). Call changed() afterwards.
    void close_group();
    // Gives a restore point a checkpoint file written by write_checkpoint (a panel restore keeps its pre-restore
    // snapshot as the newest point's end state) with its fingerprint, then prunes beyond the cap unless prune is
    // false (a restore must not lose the point it is about to open: it prunes once the open is over).
    void set_point_checkpoint(int point, const std::string &file, const McpFingerprint &fingerprint, bool prune = true);
    // Clears checkpoints beyond the cap (the start point always stays) and removes their files.
    void prune_checkpoints();

    // Called after any change of the history (the pane refreshes).
    void set_listener(std::function<void()> fn) { m_listener = std::move(fn); }
    // After editing history() directly (mark_undone, ...): saves and calls the listener.
    void changed();
    // Writes the rows recorded since the last save and calls the listener (no-op when nothing is pending).
    void flush();

    McpHistory &history() { return m_history; }
    // Counts every time the history is replaced (a project load, a Save As): state kept outside the recorder for
    // one history (the restore line, the greyed rows) is stale once it changes.
    unsigned history_epoch() const { return m_history_epoch; }
    // The loaded project's history dir (UTF-8), empty before the first sync.
    std::string history_dir() const;
    // Full path (UTF-8) of a checkpoint file name stored in the history (cp-<point>.3mf / .json).
    std::string checkpoint_path(const std::string &file) const;
    // <data_dir>/mcp_history (UTF-8): every project's history. The API's file routes refuse paths inside it.
    static std::string history_root_dir();
    // Size and SHA-256 of a file (UTF-8 path); an empty sha256 when it cannot be read.
    static McpFingerprint fingerprint(const std::string &path_utf8);

    // Identifies the current project load: the app run plus a count bumped by every project open and restore.
    // Stamped on object targets ("load"); see the target schema above.
    std::string load_stamp() const;
    // A restore replaced the project (new ObjectIDs) without notify_project_opened. Any thread.
    static void note_project_reloaded() { s_load_gen.fetch_add(1); }
    // While a restore opens its point: no checkpoint may be written (the model is half loaded). GUI thread.
    static void set_opening_point(bool on) { s_opening_point = on; }

    // Writes the current state as <stem>.3mf (<stem>.json with the presets and their unsaved edits when no
    // object is loaded) in the history dir, inside set_backup_in_progress(true/false). Returns the file name,
    // or "" when it failed or took over kCheckpointMaxS (a partial file is removed), and also "" while a project
    // is loading or a restore is opening its point (the model is half there). Used for the restore points and
    // the snapshot before a restore.
    std::string write_checkpoint(const std::string &stem);

    // "Put back" for a removed object: a full copy (volumes, modifiers, paint, per-object config, layer ranges,
    // instances) kept in memory for this app run. Returns the stash id; the oldest copies are dropped beyond
    // kMaxStashed. stashed_object() is null for an unknown/dropped id or another run's session.
    static constexpr size_t kMaxStashed = 50;
    int                stash_object(const ModelObject &obj, int plate);
    const ModelObject *stashed_object(int stash, const std::string &session) const;
    int                stashed_plate(int stash) const; // -1 when unknown
    // Identifies this app run: stash ids are only meaningful within it.
    const std::string &stash_session() const { return m_stash_session; }

    // POST /plates: which plate the API created for a row, by the plate's ObjectID (never reused in an app run,
    // unlike an address). Memory only, like the stash: after a restart (or another project) the plate is unknown.
    void note_created_plate(int row, uint64_t plate_object_id);
    // Index of that plate in `plate_object_ids` (the plates' ObjectIDs in plate order), -1 when it is gone or unknown.
    int  created_plate_index(int row, const std::vector<uint64_t> &plate_object_ids) const;
    bool knows_created_plate(int row) const { return m_created_plates.count(row) > 0; }

    // Row text shared by the handlers and McpActions' live values.
    // ConfigOptionDef::full_label when set, else label, else the key.
    static std::string config_label(const std::string &key);
    // Display text of a raw (serialized) value: enum label, On/Off for a bool, the unit (sidetext) after a
    // number, first line only and at most 40 bytes (then "..." as U+2026).
    static std::string config_value_text(const std::string &key, const std::string &raw);
    // ModelInstance::get_matrix() as 16 doubles, column-major.
    static nlohmann::json instance_matrix(const ModelInstance &mi);
    // A number without trailing zeros (up to 3 decimals): 10, 0.12, 150.5.
    static std::string num_text(double v);

    // Epoch seconds plus the test clock offset, and its local "HH:MM".
    int64_t            now_s() const;
    static std::string hhmm(int64_t epoch_s);

    // From Controller::notify_project_opened (end of Plater::load_project): the next sync treats a path
    // change as an open, not a save; and the objects have new ObjectIDs (load_stamp changes). Any thread.
    static void note_project_opened()
    {
        s_project_opened.store(true);
        s_load_gen.fetch_add(1);
    }
    // A restore opens a checkpoint file as a project and then puts the real path back: call this after it
    // so the restore is not taken for an open.
    static void forget_project_opened() { s_project_opened.store(false); }

    // Test hooks (routes only with ORCA_MCP_PANEL_TEST=1). Any thread.
    static void    test_set_clock_offset(int64_t seconds) { s_clock_offset.store(seconds); }
    static int64_t test_clock_offset() { return s_clock_offset.load(); }
    static void    test_set_checkpoint_delay(int seconds) { s_checkpoint_delay.store(seconds); }

private:
    McpRecorder();
    ~McpRecorder();

    void open_change(bool checkpoint); // before_change; checkpoint=false when slice_finished opens a group
    void take_checkpoint(int point);
    bool write_preset_checkpoint(const std::string &path_utf8);
    void load(const std::string &key, bool fresh);
    void save();
    void notify();
    void schedule_flush();
    void sweep_stale_dirs(const std::string &keep);

    McpHistory            m_history;
    unsigned              m_history_epoch = 0;
    std::string           m_key;            // loaded project key, "" before the first sync
    bool                  m_loaded = false;
    bool                  m_open   = false;  // a group is open for further rows
    bool                  m_swept  = false;  // stale dirs deleted (once per app start)
    std::function<void()> m_listener;
    // A refusal opened the current group without a checkpoint: the restore point it skipped (-1: none).
    int                   m_deferred_point = -1;

    std::unique_ptr<Model>                    m_stash;          // owns the stashed copies
    std::map<int, std::pair<ModelObject*, int>> m_stashed;      // id -> (copy, plate)
    int                                       m_next_stash = 1;
    std::string                               m_stash_session;
    std::map<int, uint64_t>                   m_created_plates; // row id -> plate ObjectID (POST /plates)
    bool                                      m_dirty         = false; // rows recorded since the last save
    bool                                      m_flush_posted  = false;
    bool                                      m_writing_point = false; // inside write_checkpoint's export
    struct DeferredSlice { nlohmann::json stats; bool ok; std::string error; };
    std::vector<DeferredSlice>                m_deferred_slices; // slice results that arrived inside that export

    static std::atomic<bool>     s_project_opened;
    static std::atomic<uint64_t> s_load_gen;
    static bool                  s_opening_point;
    static std::atomic<int64_t> s_clock_offset;
    static std::atomic<int>     s_checkpoint_delay;
};

}}} // namespace Slic3r::GUI::RemoteAPI
