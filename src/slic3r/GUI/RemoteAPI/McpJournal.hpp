#pragma once
// The user-change journal behind GET /api/v1/user_changes: what changed in OrcaSlicer outside the Remote API
// since the assistant's last request (settings edited by hand, presets switched, the project settings PUT /config
// writes (plate type, filament colours, flush volumes, wipe tower position), an object's own settings, objects
// added / removed / moved, plates added / removed, projects opened, and the MCP panel's own undos and restores).
// Not compared: layer height profiles, height ranges, part modifiers' settings and the CANVAS slot mapping.
//
// The host takes a baseline (McpBaseline) at the end of every API request and compares the project with it at
// the start of the next one; each difference becomes an entry. The route answers the entries newer than a seq,
// coalesced per key and with the bulk rules applied (see McpJournal::since). Pure C++ (no wx, no GUI includes):
// the GUI side only fills McpBaseline (mcp_capture_baseline, McpJournalCapture.cpp) and supplies the label /
// value / sensitive-key functions.
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace Slic3r { namespace GUI { namespace RemoteAPI {

// One preset collection's edited config.
struct McpConfigSnap {
    std::string                        preset; // the selected preset's name
    std::map<std::string, std::string> values; // key -> ConfigOption::serialize() of the edited config
    std::map<std::string, std::string> saved;  // keys edited on top of the preset -> the preset's saved value
};

struct McpObjectSnap {
    uint64_t                            id = 0;          // ObjectBase::id().id
    std::string                         name;
    std::vector<uint64_t>               instance_ids;    // ObjectBase::id().id of each instance
    std::vector<std::array<double, 16>> transforms;      // per instance, ModelInstance::get_matrix(), column-major
    std::map<std::string, std::string>  config;          // the object's own settings: key -> serialize()
};

struct McpBaseline {
    uint64_t                             project_gen = 0; // bumped by every project open / new project
    std::string                          project_text;    // "Opened project <name>" / "New project", for project_gen
    McpConfigSnap                        print, printer;
    std::vector<std::string>             filament_slots;  // preset name per filament slot
    std::map<std::string, McpConfigSnap> filaments;       // per distinct preset name in filament_slots
    McpConfigSnap                        project;         // project keys PUT /config writes, filament_colour aside
    std::vector<std::string>             filament_colours; // filament_colour, one per filament slot
    std::vector<McpObjectSnap>           objects;
    int                                  plates = 0;
};

// How the entries word a setting. Defaults: the key itself, the raw value, nothing sensitive.
struct McpTextFns {
    std::function<std::string(const std::string &key)>                         label;     // "Wall loops"
    std::function<std::string(const std::string &key, const std::string &raw)> value;     // "120 mm/s"
    std::function<bool(const std::string &key)>                                sensitive; // no values in the text
};

struct McpJournalEntry {
    int64_t     seq = 0;
    std::string time;  // "HH:MM"
    std::string kind;  // setting | preset | object | plate | project | panel
    std::string group; // Print | Filament | Printer | Model | Plates | Project | Panel
    std::string text;
    std::string key;   // the config key of a setting entry, else ""

    // Host-only, for coalescing (never sent).
    std::string      form;       // setting | switch | count | object | fixed
    std::string      ckey;       // coalescing key ("" = never coalesced)
    std::string      label;      // setting label / object name / count noun
    std::string      old_cmp, new_cmp;   // values compared for "changed back" (a hash for a sensitive key)
    std::string      old_text, new_text; // display values ("" for a sensitive key)
    bool             sensitive = false;
    std::vector<int> slots;      // filament slots (1-based) a filament setting applies to
    bool             slot_suffix = false; // more than one filament: the text names the slots
    std::string      op;         // object entries: added | removed | moved | scaled | rotated
};

// The differences between two baselines, as entries without seq and time. Bulk rules: a new project
// generation is one project entry and nothing else; a preset switch is one entry plus the values edited on
// top of the new preset (compared with its saved values). Project settings are group "Project" (filament colours
// per slot, and not while the filament count changed; flush volumes neither; the wipe tower not while the plate
// count changed); an object's own settings are group "Model", "<label> of <object>", "default" when not set.
std::vector<McpJournalEntry> mcp_diff(const McpBaseline &before, const McpBaseline &after, const McpTextFns &fns);

// Recomposes e.text from its fields (after coalescing).
void mcp_entry_text(McpJournalEntry &e);

// The route's answer for the entries newer than `since` (oldest first), as {"seq": latest, "changes": [...]}:
// - an "opened project" / "new project" entry drops every entry before it;
// - a preset switch drops the earlier setting entries of that preset (filament: of that slot);
// - entries with the same key coalesce: a setting, preset or count keeps its first old and last new value (when
//   the next old value is not the last new one, the API changed it in between and the later entry starts over),
//   and is dropped when they are equal; an object keeps its strongest change (added + removed = nothing).
// A coalesced entry takes the seq and time of its last part.
// A `since` greater than latest_seq (a seq this journal never handed out: the clock stepped back across a restart)
// answers every entry, with "reset": true.
nlohmann::json mcp_journal_reply(const std::deque<McpJournalEntry> &entries, int64_t since, int64_t latest_seq);

// GUI thread only (the host's calls all run there).
class McpJournal {
public:
    static constexpr size_t kMaxEntries = 200;

    McpJournal() = default;
    // The first entry gets first_seq + 1.
    explicit McpJournal(int64_t first_seq) : m_seq(first_seq) {}

    // The host's journal; its seqs start at the wall clock in milliseconds (they grow across restarts).
    static McpJournal &get();

    void set_text_fns(McpTextFns fns) { m_fns = std::move(fns); }

    // Start of an API request: the differences from the baseline become entries (none before the first
    // baseline), and `now` is the baseline from here on.
    void begin_request(McpBaseline now, const std::string &hhmm);
    // End of an API request (after its change): `now` is the baseline.
    void end_request(McpBaseline now);
    // The end of a request could not take a baseline: the next begin_request only takes one (what changed in
    // between is not reported, rather than the API's own change being reported as the user's).
    void invalidate() { m_has_baseline = false; }
    bool has_baseline() const { return m_has_baseline; }
    const McpBaseline &baseline() const { return m_baseline; }
    // An API job (arrange / orient) moved objects after its request ended: those positions are the API's.
    // Also ends the guard of api_job_started.
    void rebaseline_objects(std::vector<McpObjectSnap> objects, int plates);
    // An API arrange / orient job is queued. The job keeps the returned token: while it lives, begin_request does
    // not compare objects and plates (the job moves them after its request). When the job is dropped unfinished
    // the token dies with it, and the next begin_request takes the object positions and the plate count as the
    // API's (objects and copies added or removed are still reported). A project change ends the guard too.
    std::shared_ptr<int> api_job_started();

    // An entry of its own (the panel's undos and restores: kind "panel", group "Panel").
    void add(const std::string &kind, const std::string &group, const std::string &text, const std::string &hhmm);

    // GET /user_changes?since=<seq>.
    nlohmann::json since(int64_t seq) const { return mcp_journal_reply(m_entries, seq, m_seq); }
    int64_t        seq() const { return m_seq; }
    const std::deque<McpJournalEntry> &entries() const { return m_entries; }

    // The GUI's project open / new project: the next baseline carries a new generation and this text.
    void note_project(const std::string &text)
    {
        ++m_project_gen;
        m_project_text    = text;
        m_api_job_pending = false; // the project the job worked on is gone
        m_api_job.reset();
    }
    uint64_t           project_gen() const { return m_project_gen; }
    const std::string &project_text() const { return m_project_text; }

private:
    void push(McpJournalEntry e, const std::string &hhmm);

    McpTextFns                  m_fns;
    McpBaseline                 m_baseline;
    bool                        m_has_baseline = false;
    std::deque<McpJournalEntry> m_entries;
    int64_t                     m_seq = 0;
    uint64_t                    m_project_gen = 0;
    std::weak_ptr<int>          m_api_job;
    bool                        m_api_job_pending = false;
    std::string                 m_project_text;
};

// ---- GUI side (McpJournalCapture.cpp), GUI thread only. Not part of the standalone tests. --------------------
// The project now: selected presets and their edited configs (every filament slot), the project settings PUT /config
// writes, objects with their own settings, plate count.
McpBaseline mcp_capture_baseline();
// Start of an API request: diff + new baseline (with the test clock's HH:MM). Also before a panel action, so the
// user's edits before it are reported as theirs.
void mcp_journal_begin();
// End of an API request: new baseline.
void mcp_journal_end();
// A panel undo / restore went through: its entry, then a new baseline (the change is not reported again as an edit).
void mcp_journal_panel(const std::string &text);
// An API arrange / orient job is queued: the job's finalize keeps the token (McpJournal::api_job_started).
std::shared_ptr<int> mcp_journal_api_job_queued();
// An API arrange / orient job finished after its request: the objects' new places are the API's.
void mcp_journal_api_objects_moved();
// Plater hooks (through Controller::notify_project_opened / notify_project_new).
void mcp_journal_project_opened();
void mcp_journal_project_new();

}}} // namespace Slic3r::GUI::RemoteAPI
