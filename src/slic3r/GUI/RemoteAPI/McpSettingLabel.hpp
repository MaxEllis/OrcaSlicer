#ifndef slic3r_GUI_RemoteAPI_McpSettingLabel_hpp_
#define slic3r_GUI_RemoteAPI_McpSettingLabel_hpp_

// The MCP panel's label for a setting (polish P3): the text the settings sidebar draws for it, with its section only
// when that is needed to tell it apart. Pure (std only): RemoteAPIController.cpp gathers the parts from OrcaSlicer's
// settings index; the standalone tests call it directly.
//
//   "Top shell layers"                    the sidebar's row text (not the full label "Top solid layers")
//   "Acceleration: Normal printing"       the row text says nothing its section says
//   "Other layers speed: Outer wall"      another setting's row reads "Outer wall" too
//   "Brim width"                          shared with the prime tower's "Brim width", but it names its section "Brim"
//   "Machine start G-code"                shared with the filament's "Start G-code"; its section names it already
//   "Nozzle temperature – Other layers"   a row of several fields, its head ("Nozzle") said in full
// Never "X (Y)", and never "Section: text" when one of the two contains the other.

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace RemoteAPI {

struct McpLabelParts
{
    std::string row;      // the text the sidebar draws for the setting (SettingsIndex Option::display_label_en), or ""
    std::string full;     // OrcaSlicer's full label (ConfigOptionDef full_label, else label)
    std::string group;    // the sidebar section (Option::group, English), or ""
    std::string category; // the settings page (Option::category, English), or ""
    bool        shared = false; // another setting's row text is the same (ASCII case ignored)
};

inline std::string mcp_label_lower(std::string s)
{
    for (char &c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

inline bool mcp_label_contains(const std::string &hay, const std::string &needle)
{
    return !needle.empty() && mcp_label_lower(hay).find(mcp_label_lower(needle)) != std::string::npos;
}

// The words of a label: lower-case runs of ASCII letters and digits, without the small linking words.
inline std::vector<std::string> mcp_label_words(const std::string &s)
{
    static const char *const kSmall[] = {"a",  "an", "and", "at",  "by", "for",  "in",
                                         "of", "on", "or",  "the", "to", "when", "with"};
    std::vector<std::string> out;
    std::string              w;
    auto flush = [&]() {
        if (w.empty()) return;
        bool is_small = false;
        for (const char *k : kSmall)
            if (w == k) is_small = true;
        if (!is_small) out.push_back(w);
        w.clear();
    };
    for (const char ch : mcp_label_lower(s)) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) w += ch;
        else flush();
    }
    flush();
    return out;
}

// Whether two labels name something in common: a word they share, where words of four letters or more match on their
// first four ("wall" / "walls", "bridge" / "bridging") and shorter ones exactly ("top").
inline bool mcp_label_words_meet(const std::string &a, const std::string &b)
{
    const std::vector<std::string> wa = mcp_label_words(a), wb = mcp_label_words(b);
    for (const std::string &x : wa)
        for (const std::string &y : wb) {
            if (x.size() >= 4 && y.size() >= 4 ? x.compare(0, 4, y, 0, 4) == 0 : x == y) return true;
        }
    return false;
}

inline std::string mcp_compose_setting_label(const McpLabelParts &p)
{
    std::string text = p.row.empty() ? p.full : p.row;
    if (text.empty()) return p.full;
    // A row of several fields ("Nozzle – Other layers", the sidebar's en dash): its head in full when the full label
    // starts with it and says more ("Nozzle temperature – Other layers").
    static const std::string kDash = " \xE2\x80\x93 ";
    const size_t             d     = text.find(kDash);
    if (d != std::string::npos && !p.full.empty()) {
        const std::string head = text.substr(0, d);
        if (p.full.size() > head.size() && mcp_label_lower(p.full).compare(0, head.size(), mcp_label_lower(head)) == 0)
            text = p.full + text.substr(d);
    }
    // The section that tells it apart: its group, or the page when the group is a generic one.
    const std::string g       = mcp_label_lower(p.group);
    const bool        generic = g.empty() || g == "advanced" || g == "others" || g == "other";
    const std::string section = generic ? p.category : p.group;
    if (section.empty()) return text;
    // Needed when another row reads the same, or (a named group only) when the text says nothing the section says
    // ("Normal printing" in "Acceleration").
    const bool needed = p.shared || (!generic && !mcp_label_words_meet(text, section));
    if (!needed) return text;
    if (mcp_label_contains(section, text)) return section; // "Machine start G-code" for "Start G-code"
    if (mcp_label_contains(text, section)) return text;    // "Brim width" in "Brim"
    return section + ": " + text;
}

}}} // namespace Slic3r::GUI::RemoteAPI

#endif // slic3r_GUI_RemoteAPI_McpSettingLabel_hpp_
