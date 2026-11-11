#include "McpPanel.hpp"
// Asio-based Remote API headers before anything that pulls in wx/windows.h (asio-before-windows.h ordering).
#include "RemoteAPIController.hpp" // the F8 gate: backup_in_progress / api_ui_task_active
#include "McpActions.hpp"
#include "McpRecorder.hpp"
#include "McpText.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PrintConfig.hpp" // print_config_def: the choices of a setting row (translations)
#include "libslic3r/Utils.hpp" // resources_dir
#include "slic3r/GUI/DockPanel.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Preferences.hpp" // PreferencesTab::RemoteAPI

#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/filesystem/operations.hpp>

#include <wx/intl.h>
#include <wx/thread.h>
#include <wx/timer.h>
#include <wx/weakref.h>
#include <wx/webview.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace RemoteAPI {

using nlohmann::json;

namespace {

const char *const kPaneName  = "orcaslicer-mcp"; // fixed: the saved window layout finds the pane by it
const char *const kWantedKey = "remote_api_panel"; // "0" after the user closed the pane
const char *const kPagePath  = "/web/orcaslicer_mcp/panel.html";
constexpr int     kProtocolVersion = 1;
constexpr int     kSoonMs          = 30;  // coalesces the recorder's notifications of one request
constexpr int     kChangeMs        = 100; // coalesces bursts of config / model notifications
constexpr int     kBusyRetryMs     = 250; // while a restore, a project load, an export or an API task runs
constexpr size_t  kTestOutboxMax   = 200;

bool                s_test_record = false; // test_clear() was called: keep what the pane sends
std::deque<json>    s_test_outbox;
json                s_test_last_reveal; // what the last reveal did (test hooks), null before any
bool                s_waiting     = false; // an open_when_ready() waits for the plater

std::string text_field(const json &j, const char *key)
{
    if (!j.is_object()) return {};
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// The page sends ids as JS numbers: integers, or doubles with an integral value.
bool int_field(const json &j, const char *key, int &out)
{
    if (!j.is_object()) return false;
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return false;
    double v = 0;
    if (it->is_number_integer()) {
        const int64_t i = it->is_number_unsigned()
                              ? static_cast<int64_t>(std::min<uint64_t>(it->get<uint64_t>(), static_cast<uint64_t>(std::numeric_limits<int64_t>::max())))
                              : it->get<int64_t>();
        v = static_cast<double>(i);
    } else {
        v = it->get<double>();
        if (!std::isfinite(v) || std::floor(v) != v) return false;
    }
    if (v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max()) return false;
    out = static_cast<int>(v);
    return true;
}

bool api_enabled()
{
    const AppConfig *c = wxGetApp().app_config;
    return c != nullptr && c->get_bool("remote_api_enabled");
}

bool user_closed()
{
    const AppConfig *c = wxGetApp().app_config;
    return c != nullptr && c->get(kWantedKey) == "0";
}

// Whether the Remote API server is listening now: it can be enabled in Preferences and still fail to bind its port.
bool api_listening() { return wxGetApp().remote_api_server().running(); }

// The window is being rebuilt or the app is closing: pane actions are dropped ("" when they can run).
std::string closing_reason()
{
    if (wxGetApp().is_closing()) return MCP_T("Not while OrcaSlicer is closing");
    if (wxGetApp().is_recreating_gui()) return MCP_T("Not while the window is being rebuilt");
    return {};
}

void set_wanted(bool wanted)
{
    if (AppConfig *c = wxGetApp().app_config) c->set(kWantedKey, wanted ? "1" : "0");
}

// Nothing may read or change the project in between: a restore, a project load, an export's or an API task's
// event pump. The pane's own pushes wait; the actions themselves refuse with McpActions' busy reasons.
bool busy()
{
    Plater *p = wxGetApp().plater();
    return p == nullptr || McpActions::restoring() || p->is_loading_project() || Controller::backup_in_progress() ||
           Controller::api_ui_task_active();
}

std::string read_page()
{
    const std::string path = resources_dir() + kPagePath;
    boost::nowide::ifstream in(path.c_str(), std::ios::in | std::ios::binary);
    if (!in) {
        BOOST_LOG_TRIVIAL(error) << "MCP panel: page not found: " << path;
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

const char *const kClosedText = MCP_T("OrcaSlicer MCP panel closed. Get it back from View > OrcaSlicer MCP panel.");
const char *const kClosedUndo = MCP_T("Undo");

// ---- Translations (orcaslicer-mcp #21) -----------------------------------------------------------------------
// The page translates what it draws from its own tables (resources/web/orcaslicer_mcp/i18n, inline in the page);
// the host sends OrcaSlicer's language and, for setting rows, OrcaSlicer's own words for their English ones.

// OrcaSlicer's user interface language: the full code ("ca_ES", "zh_TW"), not current_language_code_safe(), which
// turns Catalan, Basque and Hungarian into en_US and Traditional into Simplified Chinese.
std::string ui_language()
{
    return std::string(wxGetApp().current_language_code().ToUTF8().data());
}
bool ui_language_is_english()
{
    const std::string code = ui_language();
    return code.empty() || code.rfind("en", 0) == 0 || code == "C" || code == "POSIX";
}

// OrcaSlicer's own translation of one of its English words (a setting's label, a section, a choice).
std::string orca_word(const std::string &en)
{
    if (en.empty()) return en;
    return std::string(wxGetTranslation(wxString::FromUTF8(en.c_str())).ToUTF8().data());
}

// A setting's English name as mcp_setting_label builds it ("Wall loops", "Acceleration: Normal printing", "Nozzle
// temperature – Other layers"), in OrcaSlicer's words part by part: the whole when OrcaSlicer has it, else the section
// and the text either side of ": ", and the parts either side of the sidebar's en dash. "" when nothing changed.
std::string local_setting_name(const std::string &en)
{
    static const std::string kColon = ": ", kDash = " \xE2\x80\x93 ";
    auto dashed = [](const std::string &t) {
        const size_t d = t.find(kDash);
        if (d == std::string::npos) return orca_word(t);
        return orca_word(t.substr(0, d)) + kDash + orca_word(t.substr(d + kDash.size()));
    };
    std::string out = orca_word(en);
    if (out == en) {
        const size_t c = en.find(kColon);
        out = c == std::string::npos ? dashed(en) : orca_word(en.substr(0, c)) + kColon + dashed(en.substr(c + kColon.size()));
    }
    return out == en ? std::string() : out;
}

// {"<English>": "<OrcaSlicer's word>"} for a row's setting name and the choices of its setting (all of them, so a value
// the user set since reads in the language too), or null. Cached per language: a history push lists every row.
json local_words(const McpRow &r, const std::string &language)
{
    std::string key, type = r.target.is_object() ? r.target.value("type", std::string()) : std::string();
    if (type == "config" || type == "object_config") key = r.target.value("key", std::string());
    else if (type == "refused") {
        const std::string action = r.target.value("action", std::string());
        if (action != "PUT /config" && action != "PUT /objects/{id}/config") return nullptr;
    } else
        return nullptr;
    static std::map<std::string, std::string> s_names; // language + "\n" + English -> local ("" = none)
    // The setting's name, without the filament slots a label may end with ("<name> (filament 2)", "<name> (filaments
    // 1, 3)"); a name that itself ends in "(filament)" keeps it.
    std::string name = r.label;
    if (!name.empty() && name.back() == ')') {
        size_t slots = name.rfind(" (filaments ");
        const size_t one = name.rfind(" (filament ");
        if (one != std::string::npos && (slots == std::string::npos || one > slots)) slots = one;
        const size_t digit = slots == std::string::npos ? slots : name.find_first_of("0123456789", slots);
        if (slots != std::string::npos && digit != std::string::npos && name.find(' ', slots + 2) + 1 == digit)
            name = name.substr(0, slots);
    }
    json out = json::object();
    if (!name.empty()) {
        const std::string ck = language + "\n" + name;
        auto it = s_names.find(ck);
        if (it == s_names.end()) it = s_names.emplace(ck, local_setting_name(name)).first;
        if (!it->second.empty()) out[name] = it->second;
    }
    if (!key.empty()) {
        const ConfigOptionDef *def = print_config_def.get(key);
        if (def != nullptr && (def->type == coEnum || def->type == coEnums))
            for (const std::string &label : def->enum_labels) {
                const std::string word = orca_word(label);
                if (!label.empty() && word != label) out[label] = word;
            }
    }
    return out.empty() ? json(nullptr) : out;
}

// One of the panel's phrases for text the pane shows outside its page (the closed banner), from the same tables:
// i18n/<code>.json, else <language>.json, else the regional table of that language; English when there is none.
std::string panel_phrase(const char *en)
{
    static std::string s_loaded = "\x01";
    static json        s_table;
    const std::string  code = ui_language_is_english() ? std::string() : ui_language();
    if (code != s_loaded) {
        s_loaded = code;
        s_table  = json();
        if (!code.empty()) {
            std::string c = code.substr(0, code.find('.'));
            std::replace(c.begin(), c.end(), '-', '_');
            std::string base = c.substr(0, c.find('_'));
            std::transform(base.begin(), base.end(), base.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
            const std::string dir = resources_dir() + "/web/orcaslicer_mcp/i18n/";
            std::vector<std::string> tries{c, base};
            for (const char *regional : {"pt_BR", "zh_CN", "zh_TW"})
                if (std::string(regional).rfind(base + "_", 0) == 0) tries.emplace_back(regional);
            for (const std::string &name : tries) {
                const std::string path = dir + name + ".json";
                boost::system::error_code ec;
                if (!boost::filesystem::is_regular_file(boost::filesystem::path(path), ec)) continue;
                try {
                    boost::nowide::ifstream in(path.c_str(), std::ios::in | std::ios::binary);
                    s_table = json::parse(in).value("phrases", json::object());
                } catch (const std::exception &e) {
                    BOOST_LOG_TRIVIAL(warning) << "MCP panel: translations not read from " << path << ": " << e.what();
                }
                break;
            }
        }
    }
    const auto it = s_table.is_object() ? s_table.find(en) : s_table.end();
    return s_table.is_object() && it != s_table.end() && it->is_string() && !it->get<std::string>().empty() ? it->get<std::string>()
                                                                                                           : std::string(en);
}

NotificationManager *notifications()
{
    Plater *p = wxGetApp().plater();
    return p != nullptr ? p->get_notification_manager() : nullptr;
}

// The pane is open again (View > OrcaSlicer MCP panel, or the banner's Undo): the banner has nothing left to say.
void close_closed_banner()
{
    if (NotificationManager *nm = notifications()) nm->close_notification_of_type(NotificationType::McpPanelClosed);
}

// The user closed the pane (its close button, or the page's close): a banner at the bottom right says where to get it
// back, with an Undo link that opens it again. Only a user's close reaches this (the app's close and a GUI rebuild
// remove the pane without on_close); not while those run anyway, and not while the Remote API is off. Its own type
// (NotificationType::McpPanelClosed): the change toasts never coalesce with it, and their rule of staying quiet while
// the pane is open does not apply. One at a time: an earlier copy is closed first.
void show_closed_banner()
{
    if (!closing_reason().empty() || !api_enabled()) return;
    NotificationManager *nm = notifications();
    if (nm == nullptr) return;
    nm->close_notification_of_type(NotificationType::McpPanelClosed);
    nm->push_notification(NotificationType::McpPanelClosed, NotificationManager::NotificationLevel::RegularNotificationLevel,
                          panel_phrase(kClosedText), panel_phrase(kClosedUndo), [](wxEvtHandler *) {
                              // The link is clicked inside the 3D view's render: the pane opens from the app's queue.
                              wxGetApp().CallAfter([]() {
                                  if (closing_reason().empty()) McpPanel::show();
                              });
                              return true; // the banner closes
                          });
}

json capabilities()
{
    json c = {{"restore", true},
              {"kinds", {"setting", "preset", "model", "plate", "slice", "file", "refused", "note"}},
              {"open_settings", true},
              {"settings_hint", ""},
              {"reveal", true},        // a click on a setting row shows the setting in the sidebar (McpActions::reveal)
              {"tracked_live", true}}; // live entries only for the latest row of each setting (R43, McpActions::live_values)
    // The page's default off line ("The Remote API is off.") fits a switched-off API; an enabled one that failed to
    // bind its port says so.
    if (api_enabled() && !api_listening()) c["off_text"] = MCP_T("The Remote API couldn't start. Check its port.");
    c["language"] = ui_language(); // the page draws its words in it (resources/web/orcaslicer_mcp/i18n)
    return c;
}

// Runs fn once the plater is on screen (null when the app is closing instead), then deletes itself. A pane added
// before the main window is laid out is sized against the unsized frame and keeps that width
// (AuiMgr::track_docked_size), as for the plugins' dock panels.
class McpPaneWaiter : public wxTimer
{
public:
    explicit McpPaneWaiter(std::function<void(Plater *)> fn) : m_fn(std::move(fn)) {}

    bool try_run()
    {
        const bool closing = wxGetApp().is_closing();
        Plater    *plater  = closing ? nullptr : wxGetApp().plater();
        // While the GUI is rebuilt, the old plater is still on screen but already shut down: wait for the new one.
        if (!closing && (plater == nullptr || wxGetApp().is_recreating_gui() || !plater->IsShownOnScreen())) return false;
        Stop();
        m_fn(plater);
        // Off the timer callback's stack: wxGTK's timeout callback still reads the timer after Notify().
        wxGetApp().CallAfter([this]() { delete this; });
        return true;
    }

    void Notify() override { try_run(); }

private:
    std::function<void(Plater *)> m_fn;
};

void when_plater_shown(std::function<void(Plater *)> fn)
{
    wxGetApp().CallAfter([fn = std::move(fn)]() mutable {
        auto *waiter = new McpPaneWaiter(std::move(fn));
        if (!waiter->try_run()) waiter->Start(100);
    });
}

class McpPane;
McpPane *s_pane = nullptr;

class McpPane : public DockPanel
{
public:
    McpPane(wxWindow *parent, const std::string &html)
        : DockPanel(
              parent, html, [this](const json &data) { on_page(data); },
              // The pane's own close button (or a page close): not reopened at the next start; the banner says where it is.
              []() {
                  set_wanted(false);
                  show_closed_banner();
              },
              // ~McpPane has already let go of everything this pane registered.
              nullptr)
        , m_timer(this)
    {
        Bind(wxEVT_TIMER, &McpPane::on_timer, this);
        // WebPanel's own handler (bootstrap swap, theme) is bound first and skips; this one skips too.
        browser()->Bind(wxEVT_WEBVIEW_LOADED, &McpPane::on_loaded, this);
    }

    ~McpPane() override
    {
        m_timer.Stop();
        if (s_pane == this) {
            s_pane = nullptr;
            // Nothing may call into a dead window: the recorder's listener is the only callback the pane registered.
            McpRecorder::get().set_listener(nullptr);
        }
    }

    // Coalesced pushes: the recorder notifies once per row, Tab::update_dirty and Plater updates come in bursts.
    void schedule(bool history, bool live, int ms)
    {
        m_want_history |= history;
        m_want_live |= live;
        if (!m_timer.IsRunning()) m_timer.StartOnce(ms);
    }

    // A second request while Preferences is up (a double click, delivered inside its modal loop) reaches
    // GUI_App::open_preferences nested, which ignores it: one Preferences at a time for every caller.
    void want_settings()
    {
        m_want_settings = true;
        if (!m_timer.IsRunning()) m_timer.StartOnce(1);
    }

    // "on" only while the server is really listening, not merely enabled in Preferences.
    void push_state() { push({{"type", "state"}, {"api", api_listening() ? "on" : "off"}}); }

    void push_history()
    {
        json m;
        try {
            m = McpActions::activity(); // syncs the recorder with the open project first
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(error) << "MCP panel: no history: " << e.what();
            return;
        }
        if (!m.is_object()) return;
        m["type"]         = "history";
        m["version"]      = kProtocolVersion;
        m["capabilities"] = capabilities();
        // OrcaSlicer's own words for the setting rows (only here: GET /activity and the rows stay English).
        if (!ui_language_is_english() && m.contains("rows") && m["rows"].is_array()) {
            try {
                std::map<int, const McpRow *> by_id;
                for (const McpRow &r : McpRecorder::get().history().rows()) by_id[r.id] = &r;
                const std::string language = ui_language();
                for (json &row : m["rows"]) {
                    const auto it = by_id.find(row.value("id", -1));
                    if (it == by_id.end()) continue;
                    json words = local_words(*it->second, language);
                    if (!words.is_null()) row["local"] = std::move(words);
                }
            } catch (const std::exception &e) {
                BOOST_LOG_TRIVIAL(warning) << "MCP panel: no local words: " << e.what();
            }
        }
        // Cleared after activity(): a project switch it noticed is in this push already. The page asks for the live
        // values itself after a history.
        m_want_history = false;
        m_want_live    = false;
        push(m);
    }

    void push_live()
    {
        json values;
        try {
            values = McpActions::live_values();
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(error) << "MCP panel: no live values: " << e.what();
            return;
        }
        m_want_live = false;
        json m = {{"type", "live"}, {"values", values}};
        // Whether the newest slice result still holds: it changes with the user's own edits too, which add no row.
        try {
            const json valid = McpActions::slice_valid();
            if (!valid.is_null()) m["slice_valid"] = valid;
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(warning) << "MCP panel: no slice state: " << e.what();
        }
        push(m);
    }

    void on_page(const json &data)
    {
        const std::string action = text_field(data, "action");
        if (action == "ready") {
            // The page repeats ready until its first history arrives: each answer is the current state (cheap) and
            // a history push, coalesced with any already pending on the timer.
            m_ready_seen = true;
            push_state();
            schedule(true, false, 1);
        } else if (action == "live") {
            schedule(false, true, 1);
        } else if (action == "undo" || action == "restore" || action == "undo_restore") {
            const std::string why = closing_reason();
            if (!why.empty()) {
                push(dropped_result(action, data, why));
                return;
            }
            run_action(action, data);
        } else if (action == "open_settings") {
            want_settings();
        } else if (action == "reveal") {
            reveal(data);
        } else {
            BOOST_LOG_TRIVIAL(warning) << "MCP panel: unknown page message: " << data.dump(-1, ' ', false, json::error_handler_t::replace);
        }
    }

private:
    // A click on a setting row: show that setting in the settings sidebar. Fire and forget (no result). Run from the
    // app's queue, off the web view's script callback, as OrcaSlicer's own "Jump to" links defer it; dropped while the
    // window is rebuilt or the app closes, and McpActions::reveal does nothing while busy (a restore, a project load).
    static void reveal(const json &data)
    {
        int id = 0;
        if (!closing_reason().empty() || !int_field(data, "row", id)) return;
        wxGetApp().CallAfter([id]() {
            if (!closing_reason().empty()) return;
            json res;
            try {
                res = McpActions::reveal(id);
            } catch (const std::exception &e) {
                res = {{"status", "failed"}, {"reason", mcp_fmt(MCP_T("Something went wrong: {reason}"), {{"reason", e.what()}})}};
            } catch (...) {
                res = {{"status", "failed"}, {"reason", MCP_T("Something went wrong")}};
            }
            const std::string status = text_field(res, "status");
            if (status != "ok") BOOST_LOG_TRIVIAL(debug) << "MCP panel: reveal of row " << id << " not shown: " << text_field(res, "reason");
            if (s_test_record) s_test_last_reveal = {{"row", id}, {"status", status}, {"reason", text_field(res, "reason")}};
            // m3: the sidebar field shows and blinks, but the keyboard stays in the panel (a stray key would
            // otherwise type into the field). After the jump's own deferred focus work.
            if (status == "ok")
                wxGetApp().CallAfter([]() {
                    if (s_pane != nullptr && closing_reason().empty())
                        if (wxWebView *b = s_pane->browser()) b->SetFocus();
                });
        });
    }

    // The result for an action that is not run: the page's id echoed back, status failed, the reason.
    static json dropped_result(const std::string &action, const json &data, const std::string &why)
    {
        json result = {{"type", "result"}, {"action", action}, {"status", "failed"}, {"reason", why}};
        if (action != "undo_restore") {
            const char *key = action == "undo" ? "row" : "group";
            int         id  = 0;
            result[key]     = int_field(data, key, id) ? json(id) : json(nullptr);
        }
        return result;
    }

    // A page finished loading (the bootstrap page first, then ours; a WebKit reload again). When the page has not
    // said ready yet (its message can be lost while the bridge registers), push the state and the timeline anyway.
    void on_loaded(wxWebViewEvent &event)
    {
        event.Skip();
        if (m_ready_seen) return;
        m_want_state = true;
        schedule(true, false, kChangeMs);
    }

    void push(const json &m)
    {
        if (s_test_record) {
            s_test_outbox.push_back(m);
            while (s_test_outbox.size() > kTestOutboxMax) s_test_outbox.pop_front();
        }
        push_message(m);
    }

    // Each action gets one result message: ok, cancelled (the user said no to an OrcaSlicer prompt) or failed with
    // the reason as text. Run from the app's queue, off the web view's script callback (a restore opens a project and
    // pumps events) and off this window's own stack (the pane may be closed meanwhile; its result is then dropped).
    void run_action(const std::string &action, const json &data)
    {
        json result = {{"type", "result"}, {"action", action}};
        int  id     = 0;
        if (action != "undo_restore") {
            const char *key = action == "undo" ? "row" : "group";
            if (!int_field(data, key, id)) {
                result[key]        = data.is_object() && data.contains(key) ? data[key] : json(nullptr);
                result["status"]   = "failed";
                result["reason"]   = action == "undo" ? MCP_T("No such change") : MCP_T("No such restore point");
                push(result);
                return;
            }
            result[key] = id;
        }
        wxWeakRef<McpPane> self(this);
        wxGetApp().CallAfter([self, action, id, result]() mutable {
            // Queued before a GUI rebuild or the app's close: not run against a half torn-down window.
            const std::string why = closing_reason();
            if (!why.empty()) {
                if (!self) return;
                result["status"] = "failed";
                result["reason"] = why;
                self->push(result);
                return;
            }
            json res;
            try {
                if (action == "undo")
                    res = McpActions::undo(id);
                else if (action == "restore")
                    res = McpActions::restore(id);
                else
                    res = McpActions::undo_restore();
            } catch (const std::exception &e) {
                BOOST_LOG_TRIVIAL(error) << "MCP panel: " << action << " failed: " << e.what();
                res = {{"status", "failed"}, {"reason", mcp_fmt(MCP_T("Something went wrong: {reason}"), {{"reason", e.what()}})}};
            } catch (...) {
                res = {{"status", "failed"}, {"reason", MCP_T("Something went wrong")}};
            }
            const std::string status = text_field(res, "status");
            const std::string reason = text_field(res, "reason");
            const std::string shown  = status == "ok" || status == "cancelled" ? status : std::string("failed");
            result["status"]         = shown;
            if (shown != "ok")
                result["reason"] = !reason.empty() ? reason
                                   : action == "undo" ? std::string(MCP_T("Could not revert"))
                                                      : std::string(MCP_T("Could not restore"));
            if (!self) return;
            // The timeline first (marks, "Restored to"), then the result, so the page never shows the old state
            // without its busy mark. Not inside another restore's or export's event pump (this action was refused
            // there and changed nothing; the recorder's notification brings the timeline once that is over).
            if (!busy()) self->push_history();
            self->push(result);
        });
    }

    void on_timer(wxTimerEvent &)
    {
        if (m_want_state) { // touches no project state: also while busy
            m_want_state = false;
            push_state();
        }
        if (busy()) {
            m_timer.StartOnce(kBusyRetryMs);
            return;
        }
        if (m_want_settings) {
            m_want_settings = false;
            // From the app's queue: Preferences may rebuild the GUI (a language change), which destroys this pane.
            wxGetApp().CallAfter([]() {
                if (!closing_reason().empty() || wxGetApp().mainframe == nullptr) return;
                wxGetApp().open_preferences(PreferencesTab::RemoteAPI);
            });
        }
        if (m_want_history)
            push_history();
        else if (m_want_live)
            push_live();
    }

    wxTimer m_timer;
    bool    m_want_history  = false;
    bool    m_want_live     = false;
    bool    m_want_settings = false;
    bool    m_want_state    = false; // a state push before the next history (the page has not said ready)
    bool    m_ready_seen    = false; // the page said ready at least once
};

McpPane *create(Plater *plater)
{
    if (s_pane != nullptr) return s_pane;
    if (plater == nullptr || !wxGetApp().is_editor() || wxGetApp().is_recreating_gui()) return nullptr;
    const std::string html = read_page();
    if (html.empty()) return nullptr;
    auto *pane = new McpPane(plater, html);
    s_pane     = pane;
    McpRecorder::get().set_listener([]() {
        if (s_pane != nullptr) s_pane->schedule(true, false, kSoonMs);
    });
    plater->add_dock_pane(pane, kPaneName, wxString::FromUTF8("OrcaSlicer MCP"), "right", wxSize(300, 480),
                          [pane]() { pane->fire_close(); });
    close_closed_banner();
    return pane;
}

} // namespace

void McpPanel::open_when_ready()
{
    if (s_pane != nullptr || s_waiting) return;
    if (!wxGetApp().is_editor() || !api_enabled() || user_closed()) return;
    s_waiting = true;
    when_plater_shown([](Plater *plater) {
        s_waiting = false;
        if (plater == nullptr || !api_enabled() || user_closed()) return;
        create(plater);
    });
}

void McpPanel::show()
{
    set_wanted(true);
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr) return;
    if (s_pane != nullptr) {
        plater->show_dock_pane(s_pane, true);
        return;
    }
    create(plater);
}

bool McpPanel::is_open() { return s_pane != nullptr; }

void McpPanel::config_changed()
{
    if (s_pane != nullptr && wxThread::IsMain()) s_pane->schedule(false, true, kChangeMs);
}

void McpPanel::model_changed()
{
    if (s_pane != nullptr && wxThread::IsMain()) s_pane->schedule(false, true, kChangeMs);
}

void McpPanel::slice_changed()
{
    if (s_pane != nullptr && wxThread::IsMain()) s_pane->schedule(false, true, kChangeMs);
}

void McpPanel::project_changed()
{
    if (s_pane != nullptr && wxThread::IsMain()) s_pane->schedule(true, false, kChangeMs);
}

void McpPanel::settings_changed()
{
    if (!wxThread::IsMain()) return;
    if (s_pane != nullptr) {
        s_pane->push_state();
        s_pane->schedule(true, false, kChangeMs); // capabilities.off_text follows whether the server came up
    } else
        open_when_ready();
}

void McpPanel::test_clear()
{
    s_test_record = true;
    s_test_outbox.clear();
    s_test_last_reveal = nullptr;
}

bool McpPanel::test_post(const json &message)
{
    s_test_record = true;
    if (s_pane == nullptr) return false;
    s_pane->on_page(message);
    return true;
}

json McpPanel::test_state()
{
    json messages = json::array();
    for (const json &m : s_test_outbox) messages.push_back(m);
    return {{"open", s_pane != nullptr}, {"messages", messages}, {"last_reveal", s_test_last_reveal}};
}

}}} // namespace Slic3r::GUI::RemoteAPI
