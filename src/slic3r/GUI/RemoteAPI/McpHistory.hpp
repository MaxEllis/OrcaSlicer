#pragma once
// History store behind the OrcaSlicer MCP dock panel: the timeline of an assistant's changes,
// grouped into bursts, with restore-point bookkeeping. Pure C++ (no wx, no GUI includes).
#include <cstddef>
#include <cstdint>
#include <functional>
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

struct McpGroup {
    int         id = 0;
    int64_t     ts = 0;
    std::string time;
    std::string checkpoint;  // checkpoint file, empty when none
    bool        checkpoint_failed = false;
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
    static constexpr size_t  kMaxCheckpoints = 20;

    // opened = this change opens a new group (also for the first change ever); see McpChange.
    McpChange begin_change(int64_t now_s);
    // Fills id and group; returns the row id.
    int  add_row(McpRow row);
    // After an API slice result: the next change starts a new group.
    void close_group();

    // point 0 = start point, otherwise a group id. Unknown points are ignored.
    void set_checkpoint(int point, std::string file, bool failed);
    // Clears checkpoints beyond the kMaxCheckpoints newest; returns their files for deletion.
    std::vector<std::string> prune_checkpoints();

    // The checkpoint file of a restore point (0 = start point, else a group id); "" when it has none.
    std::string checkpoint_of(int point) const;

    McpRow *row(int id);
    void    mark_undone(int id, std::string hhmm);
    const std::vector<McpRow>   &rows() const   { return m_rows; }
    const std::vector<McpGroup> &groups() const { return m_groups; } // real groups only (not the start point)

    // {"rows":[...],"groups":[...]} for the page's history message. A row's "error" is text there:
    // "" when none, else its where (or value, or "Failed").
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

// "untitled" for an empty path, else 16 lowercase hex chars of FNV-1a 64 over the lexically normalised path
// (read as UTF-8 and hashed as UTF-8, so non-ASCII paths key the same on every platform).
std::string mcp_history_key(const std::string &project_path_utf8);

// Names of directories whose mtime is more than max_age_s before now_s.
std::vector<std::string> mcp_stale_history_dirs(const std::vector<std::pair<std::string, int64_t>> &dirs_with_mtime,
                                                int64_t now_s, int64_t max_age_s = 30 * 86400);

}}} // namespace Slic3r::GUI::RemoteAPI
