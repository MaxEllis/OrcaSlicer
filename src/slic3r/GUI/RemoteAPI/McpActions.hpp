#pragma once
// What the OrcaSlicer MCP dock panel does on the user's behalf: undo one row, restore the project to a point,
// undo that restore, read the live values of the rows, and the timeline in the page's shape (also served as
// GET /api/v1/activity). These are the user's actions, like an edit in the GUI: they skip the Remote API's
// safety boxes and its busy gates, except "slicing" and "loading a project".
//
// GUI thread only, except restoring(). Every action returns the result object the page gets:
//   {"status": "ok" | "cancelled" | "failed", "reason": "<hover text>"}   (reason only when not ok)
// An ok undo marks its row undone (McpHistory::mark_undone, HH:MM) and records nothing else in the timeline; an ok
// action publishes panel.reverted {"group", "row"} / panel.restored {"group"} on the /events stream and adds the
// action to the user-change journal (McpJournal.hpp: the user's edits before it are diffed first, the baseline is
// taken after it), e.g. "Reverted in the panel: Wall loops back to 2", "Restored in the panel to 10:31". A restore
// that fails once it started is journaled too ("Tried to restore in the panel to 10:31; it failed").
#include <atomic>
#include <functional>
#include <string>
#include <nlohmann/json.hpp>

namespace Slic3r { namespace GUI { namespace RemoteAPI {

class McpActions
{
public:
    // McpHistory::page_history() of the open project + "restored_to" (null | the group id the project was last
    // restored to, until the next assistant change or refusal) + "restore_failed" (true when that restore failed once it had
    // started: the page says "Couldn't restore to") + "can_undo_restore" (the pre-restore snapshot is there and
    // nothing was recorded since). Records nothing (it follows the open project, like every recorder call).
    static nlohmann::json activity();

    // {"<row id>": {"value": <text in the row's display form>, "present": bool}} for every row with an undo that
    // is not undone, and every row of the newest group whose value can be read now. Matrices (transform,
    // duplicate, arrange, orient, move to plate) are compared here, 1e-6 relative per element: value is the row's
    // own value when nothing moved, "moved since" otherwise. present=false: the object is gone.
    static nlohmann::json live_values();

    static nlohmann::json undo(int row);
    // Restores the checkpoint of `group` (0 = the start point): refused when the file no longer matches the
    // fingerprint taken when it was written; a pre-restore snapshot first, then the checkpoint opened as the project (presets from it, unsaved edits dropped), the project's own name and path
    // put back, the checkpoint kept out of File > Recent, the project marked unsaved, and the groups after the
    // point marked undone. API requests meanwhile get 409 busy.
    static nlohmann::json restore(int group);
    // Opens the pre-restore snapshot of the last restore (while nothing was recorded since).
    static nlohmann::json undo_restore();
    // Shows a row's setting in the settings sidebar (the page's "reveal", fire and forget: the pane sends no result):
    // Sidebar::jump_to_option for the row's key in the print, filament or printer tab, as OrcaSlicer's own "Jump to"
    // links do. Only rows mcp_row_revealable() accepts (setting rows of those three presets). Changes nothing; does
    // nothing (status failed, reason for the log and the test hook) during a restore or a project load (not a slice or a job), for
    // a row that is gone or not revealable, or when the key is no longer in that preset or its tab is missing.
    static nlohmann::json reveal(int row);

    // A restore is running: the API answers 409 {"error":"busy","detail":"the user is restoring"}. Any thread.
    static bool restoring() { return s_restoring.load(); }

    // Test hook (POST /activity/test/as_user): applies {key: value} edits to the edited print, filament or
    // printer preset through the GUI's edit path, as the user typing them would: no row, no policy check.
    static nlohmann::json apply_as_user(const nlohmann::json &changes);

    // The test routes call the actions from inside an API task (run_on_ui); the panel never does. Outside such a
    // scope an action refuses while an API task or an export is on the stack (it would run inside their event pump).
    struct TestCallScope
    {
        TestCallScope();
        ~TestCallScope();
    };

private:
    static std::atomic<bool> s_restoring;
};

}}} // namespace Slic3r::GUI::RemoteAPI
