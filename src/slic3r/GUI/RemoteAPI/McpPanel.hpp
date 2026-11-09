#pragma once
// The OrcaSlicer MCP dock pane: the shared panel page (resources/web/orcaslicer_mcp/panel.html, protocol at its
// top) in a DockPanel on the right of the 3D view, caption "OrcaSlicer MCP", pane name "orcaslicer-mcp".
//
// Host -> page: "state" ({api: on|off}) when the page is ready and after Preferences closes; "history"
// (McpActions::activity() + type, version 1, capabilities) when the page is ready and after every recorder change;
// "live" (McpActions::live_values()) when the page asks and after config / model / project changes; "result" for
// every undo, restore and undo_restore ({action, row|group, status ok|cancelled|failed, reason}, never silent).
// Page -> host: ready, live, undo {row}, restore {group}, undo_restore, open_settings (Preferences > Remote API).
//
// It opens at start (and after the GUI is rebuilt) when the Remote API is enabled and the user has not closed it:
// app_config "remote_api_panel" is "0" after a close from the pane's own button, "1" after View > OrcaSlicer MCP
// panel. GUI thread only; every hook is a no-op while no pane is open. Light header: no wx, no asio.
#include <nlohmann/json.hpp>

namespace Slic3r { namespace GUI { namespace RemoteAPI {

class McpPanel
{
public:
    // Opens the pane once the plater is on screen, when the Remote API is enabled and the user has not closed it.
    // From GUI_App::post_init, the end of recreate_GUI and (through settings_changed) Preferences.
    static void open_when_ready();
    // View > OrcaSlicer MCP panel: opens the pane (or shows it again) and remembers it is wanted.
    static void show();
    static bool is_open();

    // A setting changed (Controller::notify_config_changed): the page's live values are refreshed.
    static void config_changed();
    // The model or the plates changed (Plater::priv::update / object_list_changed): live values refreshed.
    static void model_changed();
    // A project was opened, started or saved under another name: the timeline follows it (history pushed).
    static void project_changed();
    // Preferences closed (GUI_App::open_preferences): the API state is pushed, or the pane opens if it is wanted.
    static void settings_changed();

    // Test hooks (POST /activity/test/pane, only with ORCA_MCP_PANEL_TEST=1). test_clear starts recording what the
    // pane sends to the page; test_post hands the pane a message as if the page had sent it (false: no pane).
    static void           test_clear();
    static bool           test_post(const nlohmann::json &message);
    static nlohmann::json test_state(); // {"open": bool, "messages": [what the pane sent since test_clear]}
};

}}} // namespace Slic3r::GUI::RemoteAPI
