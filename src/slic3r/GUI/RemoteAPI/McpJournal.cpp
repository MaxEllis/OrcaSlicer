#include "McpJournal.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <set>
#include <utility>

namespace Slic3r { namespace GUI { namespace RemoteAPI {

using nlohmann::json;

namespace {

const char *const kArrow = "\xE2\x86\x92"; // U+2192, as the timeline writes it

// Bookkeeping keys of a preset's config, not settings anyone edits.
bool skipped_key(const std::string &k)
{
    static const std::set<std::string> skip{"inherits", "print_settings_id", "filament_settings_id",
                                            "printer_settings_id", "compatible_printers", "compatible_prints",
                                            "compatible_printers_condition", "compatible_prints_condition",
                                            "renamed_from", "different_settings_to_system", "version", "name",
                                            "from", "setting_id", "base_id", "updated_time", "user_id",
                                            "filament_id", "instantiation"};
    return skip.count(k) > 0;
}

std::string label_of(const McpTextFns &f, const std::string &k) { return f.label ? f.label(k) : k; }
std::string value_of(const McpTextFns &f, const std::string &k, const std::string &raw)
{
    return f.value ? f.value(k, raw) : raw;
}
bool sensitive(const McpTextFns &f, const std::string &k) { return f.sensitive && f.sensitive(k); }

// Never the value itself: equality is all a sensitive key's entry needs.
std::string hidden(const std::string &raw) { return "#" + std::to_string(std::hash<std::string>{}(raw)); }

std::string slot_suffix(const std::vector<int> &slots)
{
    if (slots.empty()) return {};
    if (slots.size() == 1) return " (filament " + std::to_string(slots.front()) + ")";
    std::string t = " (filaments ";
    for (size_t i = 0; i < slots.size(); ++i) t += (i ? ", " : "") + std::to_string(slots[i]);
    return t + ")";
}

std::string slots_key(const std::vector<int> &slots)
{
    std::string t;
    for (int s : slots) t += std::to_string(s) + ",";
    return t;
}

McpJournalEntry setting_entry(const McpTextFns &f, const char *group, const std::string &scope, const std::string &key,
                              const std::string &old_raw, const std::string &new_raw, std::vector<int> slots = {},
                              bool slot_text = false)
{
    McpJournalEntry e;
    e.kind        = "setting";
    e.group       = group;
    e.key         = key;
    e.form        = "setting";
    e.label       = label_of(f, key);
    e.sensitive   = sensitive(f, key);
    e.slots       = std::move(slots);
    e.slot_suffix = slot_text;
    e.ckey        = "setting:" + scope + ":" + slots_key(e.slots) + key;
    if (e.sensitive) {
        e.old_cmp = hidden(old_raw);
        e.new_cmp = hidden(new_raw);
    } else {
        e.old_cmp  = old_raw;
        e.new_cmp  = new_raw;
        e.old_text = value_of(f, key, old_raw);
        e.new_text = value_of(f, key, new_raw);
    }
    mcp_entry_text(e);
    return e;
}

McpJournalEntry switch_entry(const char *group, const std::string &type_word, const std::string &ckey,
                             const std::string &old_name, const std::string &new_name, std::vector<int> slots = {},
                             bool slot_text = false)
{
    McpJournalEntry e;
    e.kind        = "preset";
    e.group       = group;
    e.form        = "switch";
    e.ckey        = ckey;
    e.label       = type_word;
    e.old_cmp     = e.old_text = old_name;
    e.new_cmp     = e.new_text = new_name;
    e.slots       = std::move(slots);
    e.slot_suffix = slot_text;
    mcp_entry_text(e);
    return e;
}

McpJournalEntry count_entry(const char *kind, const char *group, const std::string &ckey, const std::string &noun,
                            int before, int after)
{
    McpJournalEntry e;
    e.kind    = kind;
    e.group   = group;
    e.form    = "count";
    e.ckey    = ckey;
    e.label   = noun;
    e.old_cmp = std::to_string(before);
    e.new_cmp = std::to_string(after);
    mcp_entry_text(e);
    return e;
}

McpJournalEntry object_entry(uint64_t id, const std::string &name, const char *op)
{
    McpJournalEntry e;
    e.kind  = "object";
    e.group = "Model";
    e.form  = "object";
    e.ckey  = "object:" + std::to_string(id);
    e.label = name;
    e.op    = op;
    mcp_entry_text(e);
    return e;
}

// Settings of one config: a switch (by name) is one entry plus the values edited on top of the new preset;
// otherwise each changed value.
void diff_config(const McpTextFns &f, const char *group, const std::string &scope, const std::string &type_word,
                 const McpConfigSnap &a, const McpConfigSnap &b, std::vector<McpJournalEntry> &out)
{
    if (a.preset != b.preset) {
        out.push_back(switch_entry(group, type_word, "preset:" + scope, a.preset, b.preset));
        for (const auto &kv : b.saved) {
            if (skipped_key(kv.first)) continue;
            auto it = b.values.find(kv.first);
            if (it != b.values.end() && it->second != kv.second)
                out.push_back(setting_entry(f, group, scope, kv.first, kv.second, it->second));
        }
        return;
    }
    for (const auto &kv : b.values) {
        if (skipped_key(kv.first)) continue;
        auto it = a.values.find(kv.first);
        if (it != a.values.end() && it->second != kv.second)
            out.push_back(setting_entry(f, group, scope, kv.first, it->second, kv.second));
    }
}

void diff_filaments(const McpTextFns &f, const McpBaseline &a, const McpBaseline &b, std::vector<McpJournalEntry> &out)
{
    const size_t na = a.filament_slots.size(), nb = b.filament_slots.size();
    const bool   many = nb > 1;
    // Switches, slot by slot.
    for (size_t i = 0; i < std::min(na, nb); ++i)
        if (a.filament_slots[i] != b.filament_slots[i])
            out.push_back(switch_entry("Filament", "Filament", "preset:filament:" + std::to_string(i + 1),
                                       a.filament_slots[i], b.filament_slots[i], {int(i + 1)}, many));
    // Values, per preset: the slots that kept it compare with before, the slots switched to it list the values
    // edited on top of it.
    std::set<std::string> done;
    for (size_t i = 0; i < nb; ++i) {
        const std::string &name = b.filament_slots[i];
        if (!done.insert(name).second) continue;
        auto bit = b.filaments.find(name);
        if (bit == b.filaments.end()) continue;
        std::vector<int> same, switched;
        for (size_t s = 0; s < std::min(na, nb); ++s) {
            if (b.filament_slots[s] != name) continue;
            (a.filament_slots[s] == name ? same : switched).push_back(int(s + 1));
        }
        auto ait = a.filaments.find(name);
        if (!same.empty() && ait != a.filaments.end())
            for (const auto &kv : bit->second.values) {
                if (skipped_key(kv.first)) continue;
                auto it = ait->second.values.find(kv.first);
                if (it != ait->second.values.end() && it->second != kv.second)
                    out.push_back(setting_entry(f, "Filament", "filament", kv.first, it->second, kv.second, same, many));
            }
        if (!switched.empty())
            for (const auto &kv : bit->second.saved) {
                if (skipped_key(kv.first)) continue;
                auto it = bit->second.values.find(kv.first);
                if (it != bit->second.values.end() && it->second != kv.second)
                    out.push_back(setting_entry(f, "Filament", "filament", kv.first, kv.second, it->second, switched, many));
            }
    }
    if (na != nb) out.push_back(count_entry("preset", "Filament", "filament_count", "filament", int(na), int(nb)));
}

// Not "near": <Windows.h> (force-included by the GUI build) defines near and far as empty macros.
bool close_enough(double x, double y) { return std::abs(x - y) <= 1e-6 * std::max({std::abs(x), std::abs(y), 1.0}); }

// The strongest change between two instance matrices (column-major 4x4): "rotated" > "scaled" > "moved", or "".
std::string transform_change(const std::array<double, 16> &a, const std::array<double, 16> &b)
{
    bool rotated = false, scaled = false, moved = false;
    for (int c = 0; c < 3; ++c) {
        double na = 0, nb = 0;
        for (int r = 0; r < 3; ++r) {
            na += a[c * 4 + r] * a[c * 4 + r];
            nb += b[c * 4 + r] * b[c * 4 + r];
        }
        na = std::sqrt(na);
        nb = std::sqrt(nb);
        if (!close_enough(na, nb)) scaled = true;
        for (int r = 0; r < 3; ++r) {
            const double ua = na > 0 ? a[c * 4 + r] / na : 0, ub = nb > 0 ? b[c * 4 + r] / nb : 0;
            if (!close_enough(ua, ub)) rotated = true;
        }
    }
    for (int r = 0; r < 3; ++r)
        if (!close_enough(a[12 + r], b[12 + r])) moved = true;
    return rotated ? "rotated" : scaled ? "scaled" : moved ? "moved" : "";
}

int op_rank(const std::string &op) { return op == "rotated" ? 3 : op == "scaled" ? 2 : op == "moved" ? 1 : 0; }

// An object's own settings: "<label> of <name> <old> -> <new>", "default" for a key it does not set.
void diff_object_config(const McpTextFns &f, const McpObjectSnap &p, const McpObjectSnap &o, std::vector<McpJournalEntry> &out)
{
    static const std::string kDefault = "\x1F(default)"; // compared only, never shown
    std::set<std::string> keys;
    for (const auto &kv : p.config) keys.insert(kv.first);
    for (const auto &kv : o.config) keys.insert(kv.first);
    for (const std::string &k : keys) {
        auto       pi      = p.config.find(k), oi = o.config.find(k);
        const bool has_old = pi != p.config.end(), has_new = oi != o.config.end();
        if (has_old && has_new && pi->second == oi->second) continue;
        McpJournalEntry e = setting_entry(f, "Model", "object:" + std::to_string(o.id), k, has_old ? pi->second : std::string(),
                                          has_new ? oi->second : std::string());
        e.label = label_of(f, k) + " of " + o.name;
        if (!has_old) {
            e.old_cmp = e.sensitive ? hidden(kDefault) : kDefault;
            if (!e.sensitive) e.old_text = "default";
        }
        if (!has_new) {
            e.new_cmp = e.sensitive ? hidden(kDefault) : kDefault;
            if (!e.sensitive) e.new_text = "default";
        }
        mcp_entry_text(e);
        out.push_back(std::move(e));
    }
}

// The project settings PUT /config writes (group "Project"). Keys sized by the filament or plate count are compared
// only while that count stays: a new filament or plate resizes them, which is not an edit of its own.
void diff_project(const McpTextFns &f, const McpBaseline &a, const McpBaseline &b, std::vector<McpJournalEntry> &out)
{
    const bool same_filaments = a.filament_slots.size() == b.filament_slots.size();
    const bool same_plates    = a.plates == b.plates;
    for (const auto &kv : b.project.values) {
        const std::string &k = kv.first;
        if (!same_filaments && (k == "flush_volumes_matrix" || k == "flush_volumes_vector")) continue;
        if (!same_plates && (k == "wipe_tower_x" || k == "wipe_tower_y")) continue;
        auto it = a.project.values.find(k);
        if (it != a.project.values.end() && it->second != kv.second)
            out.push_back(setting_entry(f, "Project", "project", k, it->second, kv.second));
    }
    if (!same_filaments || a.filament_colours.size() != b.filament_colours.size()) return;
    const bool many = b.filament_colours.size() > 1;
    for (size_t i = 0; i < b.filament_colours.size(); ++i)
        if (a.filament_colours[i] != b.filament_colours[i])
            out.push_back(setting_entry(f, "Project", "project", "filament_colour", a.filament_colours[i],
                                        b.filament_colours[i], {int(i + 1)}, many));
}

void diff_objects(const McpTextFns &f, const McpBaseline &a, const McpBaseline &b, std::vector<McpJournalEntry> &out)
{
    std::map<uint64_t, const McpObjectSnap *> before;
    for (const McpObjectSnap &o : a.objects) before[o.id] = &o;
    std::set<uint64_t> seen;
    for (const McpObjectSnap &o : b.objects) {
        seen.insert(o.id);
        auto it = before.find(o.id);
        if (it == before.end()) {
            out.push_back(object_entry(o.id, o.name, "added"));
            continue;
        }
        const McpObjectSnap &p = *it->second;
        std::string          op;
        for (size_t i = 0; i < o.instance_ids.size() && i < o.transforms.size(); ++i)
            for (size_t j = 0; j < p.instance_ids.size() && j < p.transforms.size(); ++j)
                if (p.instance_ids[j] == o.instance_ids[i]) {
                    const std::string c = transform_change(p.transforms[j], o.transforms[i]);
                    if (op_rank(c) > op_rank(op)) op = c;
                }
        if (!op.empty()) out.push_back(object_entry(o.id, o.name, op.c_str()));
        diff_object_config(f, p, o, out);
        if (o.instance_ids.size() != p.instance_ids.size()) {
            McpJournalEntry e = count_entry("object", "Model", "copies:" + std::to_string(o.id), "copy",
                                            int(p.instance_ids.size()), int(o.instance_ids.size()));
            e.label           = o.name; // "Added a copy of <name>"
            mcp_entry_text(e);
            out.push_back(std::move(e));
        }
    }
    for (const McpObjectSnap &o : a.objects)
        if (!seen.count(o.id)) out.push_back(object_entry(o.id, o.name, "removed"));
}

} // namespace

void mcp_entry_text(McpJournalEntry &e)
{
    if (e.form == "setting") {
        const std::string suffix = e.slot_suffix ? slot_suffix(e.slots) : std::string();
        if (e.sensitive) {
            e.text = e.label + " changed" + suffix;
            return;
        }
        auto shown = [](const std::string &v) { return v.empty() ? std::string("(empty)") : v; };
        e.text = e.label + " " + shown(e.old_text) + " " + kArrow + " " + shown(e.new_text) + suffix;
    } else if (e.form == "switch") {
        e.text = e.label + " preset switched to " + e.new_text + (e.slot_suffix ? slot_suffix(e.slots) : std::string());
    } else if (e.form == "count") {
        const int before = std::atoi(e.old_cmp.c_str()), after = std::atoi(e.new_cmp.c_str());
        const int d      = after - before;
        const int n      = std::abs(d);
        const char *verb = d >= 0 ? "Added" : "Removed";
        if (e.ckey.rfind("copies:", 0) == 0)
            e.text = std::string(verb) + (n == 1 ? " a copy" : " " + std::to_string(n) + " copies") + " of " + e.label;
        else if (e.ckey == "filament_count")
            e.text = std::string(verb) + (n == 1 ? " filament " + std::to_string(d > 0 ? after : before)
                                                 : " " + std::to_string(n) + " filaments");
        else
            e.text = std::string(verb) + (n == 1 ? " a " + e.label : " " + std::to_string(n) + " " + e.label + "s");
    } else if (e.form == "object") {
        std::string v = e.op;
        if (!v.empty()) v[0] = char(std::toupper(static_cast<unsigned char>(v[0])));
        e.text = v + " " + e.label;
    }
}

std::vector<McpJournalEntry> mcp_diff(const McpBaseline &a, const McpBaseline &b, const McpTextFns &f)
{
    std::vector<McpJournalEntry> out;
    if (a.project_gen != b.project_gen) {
        McpJournalEntry e;
        e.kind  = "project";
        e.group = "Project";
        e.form  = "fixed";
        e.text  = b.project_text.empty() ? std::string("Opened a project") : b.project_text;
        out.push_back(std::move(e));
        return out; // the whole project is new: nothing else is compared
    }
    diff_config(f, "Printer", "printer", "Printer", a.printer, b.printer, out);
    diff_config(f, "Print", "print", "Print", a.print, b.print, out);
    diff_filaments(f, a, b, out);
    diff_project(f, a, b, out);
    diff_objects(f, a, b, out);
    if (a.plates != b.plates) out.push_back(count_entry("plate", "Plates", "plates", "plate", a.plates, b.plates));
    return out;
}

json mcp_journal_reply(const std::deque<McpJournalEntry> &entries, int64_t since, int64_t latest_seq)
{
    // A seq this journal never handed out (the clock stepped back before a restart): answer from the start.
    const bool reset = since > latest_seq;
    if (reset) since = std::numeric_limits<int64_t>::min();
    std::vector<McpJournalEntry> win;
    for (const McpJournalEntry &e : entries)
        if (e.seq > since) win.push_back(e);
    // Opening a project or starting a new one replaces everything before it.
    for (size_t i = win.size(); i-- > 0;)
        if (win[i].kind == "project") {
            win.erase(win.begin(), win.begin() + std::ptrdiff_t(i));
            break;
        }

    std::vector<McpJournalEntry> out; // kept in seq order: a coalesced entry moves to its last part's place
    auto find = [&out](const std::string &ckey) -> int {
        for (size_t i = 0; i < out.size(); ++i)
            if (out[i].ckey == ckey) return int(i);
        return -1;
    };
    for (McpJournalEntry e : win) {
        if (e.form == "switch") {
            // The settings reported for the old preset no longer describe anything: drop them (a filament slot is
            // taken off the entries that named it).
            const bool filament = e.ckey.rfind("preset:filament:", 0) == 0;
            for (size_t i = 0; i < out.size();) {
                McpJournalEntry &o = out[i];
                if (o.form != "setting" || o.group != e.group) { ++i; continue; }
                if (filament) {
                    const int slot = e.slots.empty() ? 0 : e.slots.front();
                    auto      it   = std::find(o.slots.begin(), o.slots.end(), slot);
                    if (it == o.slots.end()) { ++i; continue; }
                    o.slots.erase(it);
                    if (!o.slots.empty()) {
                        o.ckey = "setting:filament:" + slots_key(o.slots) + o.key;
                        mcp_entry_text(o);
                        ++i;
                        continue;
                    }
                }
                out.erase(out.begin() + std::ptrdiff_t(i));
            }
        }
        const int at = e.ckey.empty() ? -1 : find(e.ckey);
        if (at < 0) {
            out.push_back(std::move(e));
            continue;
        }
        McpJournalEntry cur = out[size_t(at)];
        out.erase(out.begin() + at);
        if (e.form == "object") {
            if (cur.op == "added") cur.op = e.op == "removed" ? "" : "added";
            else if (e.op == "removed") cur.op = "removed";
            else if (cur.op != "removed" && op_rank(e.op) > op_rank(cur.op)) cur.op = e.op;
            else if (cur.op == "removed") cur.op = e.op; // an id never comes back; take the latest
            cur.label = e.label;
            if (cur.op.empty()) continue; // added and removed again: nothing happened
        } else if (e.old_cmp == cur.new_cmp) {
            cur.new_cmp  = e.new_cmp;
            cur.new_text = e.new_text;
            cur.label    = e.label;
        } else {
            cur = e; // changed by the API in between: what the assistant knows is the later old value
        }
        cur.seq  = e.seq;
        cur.time = e.time;
        mcp_entry_text(cur);
        out.push_back(std::move(cur));
    }

    json changes = json::array();
    for (const McpJournalEntry &e : out) {
        if (e.form != "fixed" && e.form != "object" && e.old_cmp == e.new_cmp) continue; // changed back
        json c = {{"seq", e.seq}, {"time", e.time}, {"kind", e.kind}, {"group", e.group}, {"text", e.text}};
        if (!e.key.empty()) c["key"] = e.key;
        changes.push_back(std::move(c));
    }
    json reply = {{"seq", latest_seq}, {"changes", changes}};
    if (reset) reply["reset"] = true;
    return reply;
}

McpJournal &McpJournal::get()
{
    // Seqs start at the clock in milliseconds, so they keep growing across OrcaSlicer restarts: a client holding a
    // seq from the previous run still gets every entry of this one.
    static McpJournal j(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count());
    return j;
}

std::shared_ptr<int> McpJournal::api_job_started()
{
    auto token        = std::make_shared<int>(0);
    m_api_job         = token;
    m_api_job_pending = true;
    return token;
}

void McpJournal::push(McpJournalEntry e, const std::string &hhmm)
{
    e.seq  = ++m_seq;
    e.time = hhmm;
    m_entries.push_back(std::move(e));
    while (m_entries.size() > kMaxEntries) m_entries.pop_front();
}

void McpJournal::begin_request(McpBaseline now, const std::string &hhmm)
{
    if (m_has_baseline && m_api_job_pending) {
        if (!m_api_job.expired()) {
            // The API's job is still moving objects: compare their places once it is done. Their own settings are
            // the user's (a job never changes them), so those are compared now.
            std::vector<McpObjectSnap> kept = m_baseline.objects;
            for (McpObjectSnap &o : kept) {
                auto it = std::find_if(now.objects.begin(), now.objects.end(),
                                       [&o](const McpObjectSnap &n) { return n.id == o.id; });
                if (it != now.objects.end()) o.config = it->config;
            }
            now.objects = std::move(kept);
            now.plates  = m_baseline.plates;
        } else {
            // The job was dropped without finishing (OrcaSlicer clears its queue on a delete, a new or opened
            // project): the positions it may have set, and the plates it may have added, are the API's. Objects and
            // copies added or removed meanwhile (the user's delete that dropped the job) are still compared.
            for (McpObjectSnap &o : m_baseline.objects) {
                auto it = std::find_if(now.objects.begin(), now.objects.end(),
                                       [&o](const McpObjectSnap &n) { return n.id == o.id; });
                if (it == now.objects.end()) continue;
                for (size_t i = 0; i < o.instance_ids.size() && i < o.transforms.size(); ++i)
                    for (size_t k = 0; k < it->instance_ids.size() && k < it->transforms.size(); ++k)
                        if (it->instance_ids[k] == o.instance_ids[i]) o.transforms[i] = it->transforms[k];
            }
            m_baseline.plates = now.plates;
            m_api_job_pending = false;
        }
    }
    if (m_has_baseline)
        for (McpJournalEntry &e : mcp_diff(m_baseline, now, m_fns)) push(std::move(e), hhmm);
    m_baseline     = std::move(now);
    m_has_baseline = true;
}

void McpJournal::end_request(McpBaseline now)
{
    m_baseline     = std::move(now);
    m_has_baseline = true;
}

void McpJournal::rebaseline_objects(std::vector<McpObjectSnap> objects, int plates)
{
    m_api_job_pending = false;
    m_api_job.reset();
    if (!m_has_baseline) return;
    m_baseline.objects = std::move(objects);
    m_baseline.plates  = plates;
}

void McpJournal::add(const std::string &kind, const std::string &group, const std::string &text, const std::string &hhmm)
{
    McpJournalEntry e;
    e.kind  = kind;
    e.group = group;
    e.text  = text;
    e.form  = "fixed";
    push(std::move(e), hhmm);
}

}}} // namespace Slic3r::GUI::RemoteAPI
