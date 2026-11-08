#pragma once
// Recorder behind the OrcaSlicer MCP dock panel: turns Remote API changes into McpHistory rows, takes the
// restore-point checkpoints, and keeps the history per project in <data_dir>/mcp_history/<key>/.
// GUI thread only (every member except the static test setters and note_project_opened).
#include "McpHistory.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <nlohmann/json.hpp>

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
    // Adds a row (ts, time, id and group are filled in) and saves. Opens a group itself (with its checkpoint)
    // only when none is open: no group yet, or the last one was closed by a slice result. Returns the row id.
    int  record(McpRow row);
    // A change the API refused: kind "refused", value "Not allowed", where = why.
    void record_refusal(std::string label, std::string why);
    // An API slice ended: a "slice" row in the current group, then the group closes. ok=false with
    // error "cancelled" records "Slicing cancelled"; any other failure records "Slicing failed" (error row).
    // stats = the GET /slice/status stats (estimated_time_seconds, filament_used_g).
    void slice_finished(const nlohmann::json &stats, bool ok, const std::string &error);

    // Follows the open project: loads its history on an open, rekeys untitled/ on the first save of an
    // Untitled project, starts an empty history on a Save As. Called by the members above and by the pane.
    void sync_project();
    // An API move/rename of a project file: its history dir moves with it (also when it is the open project).
    // Call right after the file moved and BEFORE recording the row for it.
    void project_moved(const std::string &old_path_utf8, const std::string &new_path_utf8);

    // Called after any change of the history (the pane refreshes).
    void set_listener(std::function<void()> fn) { m_listener = std::move(fn); }
    // After editing history() directly (mark_undone, ...): saves and calls the listener.
    void changed();

    McpHistory &history() { return m_history; }
    // The loaded project's history dir (UTF-8), empty before the first sync.
    std::string history_dir() const;
    // Full path (UTF-8) of a checkpoint file name stored in the history (cp-<point>.3mf / .json).
    std::string checkpoint_path(const std::string &file) const;

    // Writes the current state as <stem>.3mf (<stem>.json with the presets and their unsaved edits when no
    // object is loaded) in the history dir, inside set_backup_in_progress(true/false). Returns the file name,
    // or "" when it failed or took over kCheckpointMaxS (a partial file is removed). Used for the restore
    // points; Task 6 uses it for the snapshot before a restore.
    std::string write_checkpoint(const std::string &stem);

    // Epoch seconds plus the test clock offset, and its local "HH:MM".
    int64_t            now_s() const;
    static std::string hhmm(int64_t epoch_s);

    // From Controller::notify_project_opened (end of Plater::load_project): the next sync treats a path
    // change as an open, not a save. Any thread.
    static void note_project_opened() { s_project_opened.store(true); }
    // A restore opens a checkpoint file as a project and then puts the real path back: call this after it
    // so the restore is not taken for an open.
    static void forget_project_opened() { s_project_opened.store(false); }

    // Test hooks (routes only with ORCA_MCP_PANEL_TEST=1). Any thread.
    static void    test_set_clock_offset(int64_t seconds) { s_clock_offset.store(seconds); }
    static int64_t test_clock_offset() { return s_clock_offset.load(); }
    static void    test_set_checkpoint_delay(int seconds) { s_checkpoint_delay.store(seconds); }

private:
    McpRecorder() = default;

    void take_checkpoint(int point);
    bool write_preset_checkpoint(const std::string &path_utf8);
    void load(const std::string &key, bool fresh);
    void save();
    void notify();
    void sweep_stale_dirs(const std::string &keep);

    McpHistory            m_history;
    std::string           m_key;            // loaded project key, "" before the first sync
    bool                  m_loaded = false;
    bool                  m_open   = false;  // a group is open for further rows
    bool                  m_swept  = false;  // stale dirs deleted (once per app start)
    std::function<void()> m_listener;

    static std::atomic<bool>    s_project_opened;
    static std::atomic<int64_t> s_clock_offset;
    static std::atomic<int>     s_checkpoint_delay;
};

}}} // namespace Slic3r::GUI::RemoteAPI
