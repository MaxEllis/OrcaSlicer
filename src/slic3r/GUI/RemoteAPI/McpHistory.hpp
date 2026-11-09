#pragma once
// History store behind the OrcaSlicer MCP dock panel: the timeline of an assistant's changes,
// grouped into bursts, with restore-point bookkeeping. Pure C++ (no wx, no GUI includes).
#include <cstddef>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

namespace Slic3r { namespace GUI { namespace RemoteAPI {

struct McpRow {
    int         id    = 0;
    int         group = 0;
    int64_t     ts    = 0;
    std::string time;        // "HH:MM", set by the caller
    std::string kind, label, value;
    std::string before;      // previous value, in the same display form as `value` (unit, enum label); raw in target
    std::string where;       // any other detail note
    std::string undo_verb;   // empty = not undoable
    std::string undo_tip;
    std::string undone_at;   // "" or "HH:MM"
    std::string title_hint;
    nlohmann::json target;   // host-only: what undo/live need
    nlohmann::json result;   // {"seconds":n,"grams":n} or null
    bool        error   = false;
    bool        on_disk = false;
};

// What a checkpoint file held when the recorder wrote it: a restore opens it only while it still matches (a file
// planted or changed in the history folder could otherwise bring scripts or G-code past the API's policy).
struct McpFingerprint {
    uint64_t    size = 0;
    std::string sha256;      // lowercase hex; empty = none recorded (the point cannot be trusted)
    bool operator==(const McpFingerprint &o) const { return size == o.size && sha256 == o.sha256; }
    bool operator!=(const McpFingerprint &o) const { return !(*this == o); }
};
// True only when a fingerprint was recorded and the file still has it.
bool mcp_fingerprint_ok(const McpFingerprint &recorded, const McpFingerprint &now);

struct McpGroup {
    int            id = 0;
    int64_t        ts = 0;
    std::string    time;
    std::string    checkpoint;  // checkpoint file, empty when none
    bool           checkpoint_failed = false;
    McpFingerprint fingerprint; // of the checkpoint file, when written
};

// What begin_change did. prev_point is the restore point a checkpoint taken NOW represents: the previous
// group's id, or 0 (the start point) before any group. Valid only when opened is true.
struct McpChange {
    bool opened     = false;
    int  prev_point = 0;
};

class McpHistory {
public:
    static constexpr int64_t kBurstGapS    = 60;
    static constexpr size_t  kMaxCheckpoints = 20;  // the start point plus the 19 newest groups' points
    static constexpr size_t  kMaxRows      = 500;   // cap_rows: beyond these the oldest groups go
    static constexpr size_t  kMaxGroups    = 100;

    // opened = this change opens a new group (also for the first change ever); see McpChange.
    McpChange begin_change(int64_t now_s);
    // Fills id and group; returns the row id.
    int  add_row(McpRow row);
    // After an API slice result: the next change starts a new group.
    void close_group();

    // point 0 = start point, otherwise a group id. Unknown points are ignored.
    void set_checkpoint(int point, std::string file, bool failed, McpFingerprint fingerprint = {});
    // A new fingerprint for a point's file (the test hook that spoils a point on purpose). Unknown points are ignored.
    void set_fingerprint(int point, McpFingerprint fingerprint);
    // Clears checkpoints beyond kMaxCheckpoints; returns their files for deletion. The start point ("Before the
    // assistant started", the way back to the very beginning) is never pruned: the oldest group points go first.
    std::vector<std::string> prune_checkpoints();
    // Keeps the timeline bounded: while there are more than kMaxRows rows or kMaxGroups groups, drops the oldest
    // group (its rows and its point) other than the newest one. Groups without a restore point go first, so the
    // points the page offers stay as long as possible; the start point is never dropped (it takes the dropped
    // first group's time). Returns the checkpoint files of the dropped groups for deletion.
    std::vector<std::string> cap_rows();

    // The checkpoint file of a restore point (0 = start point, else a group id); "" when it has none.
    std::string checkpoint_of(int point) const;
    // The fingerprint recorded for that file (empty when none).
    McpFingerprint fingerprint_of(int point) const;

    McpRow *row(int id);
    void    mark_undone(int id, std::string hhmm);
    const std::vector<McpRow>   &rows() const   { return m_rows; }
    const std::vector<McpGroup> &groups() const { return m_groups; } // real groups only (not the start point)

    // {"rows":[...],"groups":[...]} for the page's history message. A row's "error" is text there:
    // "" when none, else its where (or value, or "Failed"). "reveal" is mcp_row_revealable(); "superseded" is
    // mcp_tracking()'s (the assistant changed the same setting again later); "dropped" is mcp_tracking()'s too (the
    // assistant's own preset switch dropped the edit).
    nlohmann::json page_history() const;

    nlohmann::json to_json() const;
    // Points whose file is gone are dropped. Unreadable input yields an empty history.
    static McpHistory from_json(const nlohmann::json &j, const std::function<bool(const std::string &)> &file_exists);

private:
    std::vector<McpRow>   m_rows;
    std::vector<McpGroup> m_groups;
    McpGroup              m_start;
    bool                  m_has_start = false;
    bool                  m_closed    = true;
    int64_t               m_last_ts   = 0;
    int                   m_next_row  = 1;
};

// A row the panel can show in the settings sidebar (the page's "reveal"): a setting row whose target names a key of
// the print, filament or printer preset (target type "config", scope print|filament|printer). Per-object settings,
// project-scope keys (filament colours, wipe tower position) and every other kind are not revealable.
bool mcp_row_revealable(const McpRow &row);

// ---- Which rows the panel tracks live (ruling R43) -----------------------------------------------------------------
// The panel shows a row's live value (and "changed by you") only for the latest row of each setting, whichever group
// it is in. A row's subject names what it changed: rows with the same subject changed the same setting (or copy,
// layer profile, height range, preset slot). "" when the row has none (a slice, a refusal, a file, an added object):
// such a row keeps the earlier rule.
//   config         "config|<scope>|<key>|<index>|<preset>"   (index and preset as recorded, "" when none)
//   object_config  "object_config|<object_id>|<key>"
//   preset         "preset|<preset_type>|<slot>"
//   slot_mapping   "slot_mapping|<filament>"
//   layer_profile  "layer_profile|<object_id>"
//   height_range   "height_range|<object_id>|<min_z>|<max_z>"
//   transform, instance_plate  "instance|<object_id>|<instance>"
std::string mcp_row_subject(const McpRow &row);

struct McpTracking
{
    std::set<int> tracked;    // rows that get a live entry
    std::set<int> superseded; // rows a later (not undone) row of the same subject changed again: no live entry, no undo
    std::set<int> dropped;    // superseded rows whose edit the assistant's own preset switch dropped (P1)
};
// tracked: for each subject its latest row that is not undone; rows without a subject that are not undone and have
// an undo or sit in newest_group. superseded: rows not undone with a later not-undone row of the same subject;
// config rows written to a preset the assistant later switched away from (the newest later preset row of that type
// whose "after" is another preset; for a filament slot, one whose "before" or "after" was the row's preset); and
// config rows of a scope a later preset switch discarded (its target "discarded" names the scope; no save of that
// scope in between), whatever was selected afterwards: those are also in dropped. A preset row undone by the panel's
// Switch back (target "reverted": {"after_row", "discarded"}) still counts as the switch it was, and its Switch back
// as a switch back (after row after_row) that discarded what it names; one undone by a restore does not count.
McpTracking mcp_tracking(const std::vector<McpRow> &rows, int newest_group);

// Windows and macOS file systems ignore case (by default): paths are compared, and history keys made, case-folded.
#if defined(_WIN32) || defined(__APPLE__)
constexpr bool kMcpFoldPathCase = true;
#else
constexpr bool kMcpFoldPathCase = false;
#endif

// "untitled" for an empty path, else 16 lowercase hex chars of FNV-1a 64 over the lexically normalised path
// (read as UTF-8 and hashed as UTF-8, so non-ASCII paths key the same on every platform). With fold_case (the
// default on Windows and macOS) ASCII letters are lower-cased first, so C:\Users\Max\a.3mf and c:\users\max\A.3mf
// share one timeline.
std::string mcp_history_key(const std::string &project_path_utf8, bool fold_case = kMcpFoldPathCase);

// True when `path` (UTF-8) is `dir` (UTF-8) or lies inside it. Both are made absolute and weakly canonical first
// (symlinks of the existing part resolved, "." and ".." removed), then compared component by component, ASCII
// case-folded with fold_case. Used to keep the API's file routes out of <data_dir>/mcp_history.
bool mcp_path_within(const std::string &dir_utf8, const std::string &path_utf8, bool fold_case = kMcpFoldPathCase);

// ---- Object identity across project loads -------------------------------------------------------------------
// Object rows name objects by ObjectID, which OrcaSlicer renews whenever a project is loaded (open, restore,
// restart). The recorder stamps each object target with the load it was recorded in ("load") and the object's
// name ("object_name"); while the stamp is current the id is used, after a reload the name, when exactly one
// object has it.
struct McpObjectRef {
    uint64_t              id = 0;
    std::string           name;
    std::vector<uint64_t> instance_ids;
};
// The target was recorded in the current load (its "load" stamp equals `stamp`).
bool mcp_target_current(const nlohmann::json &target, const std::string &stamp);
// Index in `now` of the object a target (or an arrange/orient item) names: by "object_id" when `current`, else by
// "object_name" (fallback_name when the target has none). -1: gone (current, no such id); -2: cannot be tracked
// (after a reload no object, or more than one, has the name).
int mcp_find_object(const nlohmann::json &target, bool current, const std::string &fallback_name,
                    const std::vector<McpObjectRef> &now);
// Index of the instance a target names: by "instance_id" when `current`, else by the recorded "instance" index.
// -1 when there is no such instance.
int mcp_find_instance(const nlohmann::json &target, bool current, const std::vector<uint64_t> &instance_ids);

// Names of directories whose mtime is more than max_age_s before now_s.
std::vector<std::string> mcp_stale_history_dirs(const std::vector<std::pair<std::string, int64_t>> &dirs_with_mtime,
                                                int64_t now_s, int64_t max_age_s = 30 * 86400);

}}} // namespace Slic3r::GUI::RemoteAPI
