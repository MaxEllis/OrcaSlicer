#ifndef slic3r_GUI_RemoteAPI_McpText_hpp_
#define slic3r_GUI_RemoteAPI_McpText_hpp_

// Words the OrcaSlicer MCP panel shows (orcaslicer-mcp #21). The panel translates them itself when it draws them
// (resources/web/orcaslicer_mcp/i18n/<lang>.json); here they stay English, because the rows, the API's replies and
// the assistant's change journal read them, and code compares some of them ("Revert").
//
//   MCP_T("Sliced")                                      a phrase the panel may show: scripts/mcp_panel_i18n.py
//                                                         collects every MCP_T("...") into the translation catalogue
//   mcp_fmt(MCP_T("Set {label} back to {before}"),       a phrase made of parts: the same English as before, and the
//           {{"label", label}, {"before", before}})      panel matches the template to translate it ("{n}", "{count}",
//                                                         "{index}", "{line}", "{first}", "{last}" hold numbers,
//                                                         "{time}" a clock time, any other name any text)
//
// Never put a variable or a concatenation inside MCP_T: the catalogue reads the literal.

#include <initializer_list>
#include <string>
#include <utility>

#define MCP_T(s) (s)

namespace Slic3r { namespace GUI { namespace RemoteAPI {

// tmpl with each {name} replaced by its value in args; an unknown {name} stays as it is.
inline std::string mcp_fmt(const char *tmpl, std::initializer_list<std::pair<const char *, std::string>> args)
{
    std::string       out;
    const std::string t(tmpl);
    for (size_t i = 0; i < t.size();) {
        if (t[i] == '{') {
            const size_t close = t.find('}', i + 1);
            if (close != std::string::npos) {
                const std::string name = t.substr(i + 1, close - i - 1);
                bool              found = false;
                for (const auto &a : args)
                    if (name == a.first) {
                        out += a.second;
                        found = true;
                        break;
                    }
                if (found) {
                    i = close + 1;
                    continue;
                }
            }
        }
        out += t[i++];
    }
    return out;
}

}}} // namespace Slic3r::GUI::RemoteAPI

#endif // slic3r_GUI_RemoteAPI_McpText_hpp_
