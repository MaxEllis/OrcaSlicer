#include "McpHistory.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <set>
#include <system_error>

namespace Slic3r { namespace GUI { namespace RemoteAPI {

using nlohmann::json;

McpChange McpHistory::begin_change(int64_t now_s)
{
    McpChange c;
    c.opened = m_groups.empty() || m_closed || now_s - m_last_ts >= kBurstGapS;
    if (c.opened) {
        c.prev_point = m_groups.empty() ? 0 : m_groups.back().id;
        McpGroup g;
        g.id = c.prev_point + 1;
        g.ts = now_s;
        m_groups.push_back(std::move(g));
    }
    m_closed  = false;
    m_last_ts = now_s;
    return c;
}

int McpHistory::add_row(McpRow row)
{
    if (m_groups.empty()) begin_change(row.ts);
    row.id    = m_next_row++;
    row.group = m_groups.back().id;
    if (m_groups.back().time.empty()) m_groups.back().time = row.time;
    m_rows.push_back(std::move(row));
    return m_rows.back().id;
}

void McpHistory::close_group() { m_closed = true; }

bool mcp_fingerprint_ok(const McpFingerprint &recorded, const McpFingerprint &now)
{
    return !recorded.sha256.empty() && recorded == now;
}

void McpHistory::set_checkpoint(int point, std::string file, bool failed, McpFingerprint fingerprint)
{
    if (point == 0) {
        m_has_start = true;
        m_start.id = 0;
        m_start.checkpoint = std::move(file);
        m_start.checkpoint_failed = failed;
        m_start.fingerprint = std::move(fingerprint);
        return;
    }
    for (McpGroup &g : m_groups)
        if (g.id == point) {
            g.checkpoint = std::move(file);
            g.checkpoint_failed = failed;
            g.fingerprint = std::move(fingerprint);
            return;
        }
}

void McpHistory::set_fingerprint(int point, McpFingerprint fingerprint)
{
    if (point == 0) {
        if (m_has_start) m_start.fingerprint = std::move(fingerprint);
        return;
    }
    for (McpGroup &g : m_groups)
        if (g.id == point) {
            g.fingerprint = std::move(fingerprint);
            return;
        }
}

std::vector<std::string> McpHistory::prune_checkpoints()
{
    // The start point always stays; the group points share the rest of the cap, oldest dropped first.
    const bool start_kept = m_has_start && !m_start.checkpoint.empty();
    const size_t room = kMaxCheckpoints - (start_kept ? 1 : 0);
    std::vector<McpGroup *> holders;
    for (McpGroup &g : m_groups)
        if (!g.checkpoint.empty()) holders.push_back(&g);
    std::vector<std::string> out;
    if (holders.size() <= room) return out;
    const size_t drop = holders.size() - room;
    for (size_t i = 0; i < drop; ++i) {
        out.push_back(std::move(holders[i]->checkpoint));
        holders[i]->checkpoint.clear();
        holders[i]->fingerprint = McpFingerprint();
    }
    return out;
}

std::vector<std::string> McpHistory::cap_rows()
{
    std::vector<std::string> out;
    auto over = [this] { return m_rows.size() > kMaxRows || m_groups.size() > kMaxGroups; };
    // Two passes: groups without a restore point first, then (only if still over) the oldest ones that have one.
    for (int pass = 0; pass < 2 && over(); ++pass) {
        for (size_t i = 0; i + 1 < m_groups.size() && over();) {
            McpGroup &g = m_groups[i];
            // Only the old end is point-less: stop at the first group that has a point, so newer point-less
            // groups are not dropped from the middle of the timeline.
            if (pass == 0 && !g.checkpoint.empty()) break;
            if (i == 0 && m_has_start && m_start.time.empty()) {
                // "Before the assistant started" keeps the time it had (the first group's).
                m_start.ts   = g.ts;
                m_start.time = g.time;
            }
            if (!g.checkpoint.empty()) out.push_back(g.checkpoint);
            const int gid = g.id;
            m_rows.erase(std::remove_if(m_rows.begin(), m_rows.end(), [gid](const McpRow &r) { return r.group == gid; }),
                         m_rows.end());
            m_groups.erase(m_groups.begin() + std::ptrdiff_t(i));
        }
    }
    return out;
}

std::string McpHistory::checkpoint_of(int point) const
{
    if (point == 0) return m_has_start ? m_start.checkpoint : std::string();
    for (const McpGroup &g : m_groups)
        if (g.id == point) return g.checkpoint;
    return std::string();
}

McpFingerprint McpHistory::fingerprint_of(int point) const
{
    if (point == 0) return m_has_start ? m_start.fingerprint : McpFingerprint();
    for (const McpGroup &g : m_groups)
        if (g.id == point) return g.fingerprint;
    return McpFingerprint();
}

McpRow *McpHistory::row(int id)
{
    for (McpRow &r : m_rows)
        if (r.id == id) return &r;
    return nullptr;
}

void McpHistory::mark_undone(int id, std::string hhmm)
{
    if (McpRow *r = row(id)) r->undone_at = std::move(hhmm);
}

static json restore_json(const McpGroup &g)
{
    if (!g.checkpoint.empty()) return {{"available", true}, {"why", ""}};
    if (g.checkpoint_failed) return {{"available", false}, {"why", "No restore point here"}};
    return nullptr;
}

// The page treats `error` as text: has(x) is true for anything but null/undefined/"" (false included).
static std::string page_error(const McpRow &r)
{
    if (!r.error) return std::string();
    if (!r.where.empty()) return r.where;
    if (!r.value.empty()) return r.value;
    return "Failed";
}

bool mcp_row_revealable(const McpRow &row)
{
    if (row.kind != "setting" || !row.target.is_object()) return false;
    const json &t = row.target;
    auto str = [&t](const char *k) {
        auto it = t.find(k);
        return it != t.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    const std::string scope = str("scope");
    return str("type") == "config" && !str("key").empty() &&
           (scope == "print" || scope == "filament" || scope == "printer");
}

std::string mcp_row_subject(const McpRow &row)
{
    if (!row.target.is_object()) return {};
    const json &t = row.target;
    auto part = [&t](const char *k) -> std::string {
        auto it = t.find(k);
        if (it == t.end() || it->is_null()) return {};
        return it->is_string() ? it->get<std::string>() : it->dump();
    };
    const std::string type = part("type");
    if (type == "config")         return "config|" + part("scope") + "|" + part("key") + "|" + part("index") + "|" + part("preset");
    if (type == "object_config")  return "object_config|" + part("object_id") + "|" + part("key");
    if (type == "preset")         return "preset|" + part("preset_type") + "|" + part("slot");
    if (type == "slot_mapping")   return "slot_mapping|" + part("filament");
    if (type == "layer_profile")  return "layer_profile|" + part("object_id");
    if (type == "height_range")   return "height_range|" + part("object_id") + "|" + part("min_z") + "|" + part("max_z");
    if (type == "transform" || type == "instance_plate") return "instance|" + part("object_id") + "|" + part("instance");
    return {};
}

McpTracking mcp_tracking(const std::vector<McpRow> &rows, int newest_group)
{
    McpTracking out;
    std::set<std::string> later; // subjects seen in a later, not undone row (walking from the newest)
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        const McpRow &r = *it;
        if (!r.undone_at.empty()) continue;
        const std::string subj = mcp_row_subject(r);
        if (subj.empty()) {
            if (!r.undo_verb.empty() || r.group == newest_group) out.tracked.insert(r.id);
            continue;
        }
        if (later.insert(subj).second) out.tracked.insert(r.id);
        else                           out.superseded.insert(r.id);
    }
    return out;
}

json McpHistory::page_history() const
{
    const McpTracking tracking = mcp_tracking(m_rows, m_groups.empty() ? -1 : m_groups.back().id);
    json rows = json::array();
    for (const McpRow &r : m_rows) {
        json undo = nullptr;
        if (!r.undo_verb.empty()) {
            undo = {{"verb", r.undo_verb}};
            if (!r.undo_tip.empty()) undo["tip"] = r.undo_tip;
        }
        rows.push_back({{"id", r.id}, {"group", r.group}, {"ts", r.ts}, {"time", r.time}, {"kind", r.kind},
                        {"label", r.label}, {"value", r.value}, {"before", r.before}, {"where", r.where},
                        {"undo", undo}, {"undone_at", r.undone_at},
                        {"title_hint", r.title_hint.empty() ? json(nullptr) : json(r.title_hint)},
                        {"result", r.result.is_null() ? json(nullptr) : r.result}, {"error", page_error(r)},
                        {"on_disk", r.on_disk}, {"reveal", mcp_row_revealable(r)},
                        {"superseded", tracking.superseded.count(r.id) > 0}});
    }
    json groups = json::array();
    if (m_has_start && !m_groups.empty()) {
        // The start point's time is the first group's; once that group was dropped (cap_rows), the time it kept.
        const bool own = !m_start.time.empty();
        groups.push_back({{"id", 0}, {"ts", own ? m_start.ts : m_groups.front().ts},
                          {"time", own ? m_start.time : m_groups.front().time}, {"restore", restore_json(m_start)}});
    }
    for (const McpGroup &g : m_groups)
        groups.push_back({{"id", g.id}, {"ts", g.ts}, {"time", g.time}, {"restore", restore_json(g)}});
    return {{"rows", rows}, {"groups", groups}};
}

json McpHistory::to_json() const
{
    json rows = json::array();
    for (const McpRow &r : m_rows)
        rows.push_back({{"id", r.id}, {"group", r.group}, {"ts", r.ts}, {"time", r.time}, {"kind", r.kind},
                        {"label", r.label}, {"value", r.value}, {"before", r.before}, {"where", r.where},
                        {"undo_verb", r.undo_verb}, {"undo_tip", r.undo_tip}, {"undone_at", r.undone_at},
                        {"title_hint", r.title_hint}, {"target", r.target}, {"result", r.result},
                        {"error", r.error}, {"on_disk", r.on_disk}});
    auto gj = [](const McpGroup &g) {
        return json{{"id", g.id}, {"ts", g.ts}, {"time", g.time}, {"checkpoint", g.checkpoint},
                    {"checkpoint_failed", g.checkpoint_failed}, {"size", g.fingerprint.size},
                    {"sha256", g.fingerprint.sha256}};
    };
    json groups = json::array();
    for (const McpGroup &g : m_groups) groups.push_back(gj(g));
    return {{"version", 1}, {"rows", rows}, {"groups", groups}, {"start", m_has_start ? gj(m_start) : json(nullptr)},
            {"closed", m_closed}, {"last_ts", m_last_ts}, {"next_row", m_next_row}};
}

McpHistory McpHistory::from_json(const json &j, const std::function<bool(const std::string &)> &file_exists)
{
    McpHistory h;
    try {
        if (!j.is_object()) return h;
        auto read_group = [&](const json &g) {
            McpGroup o;
            o.id = g.value("id", 0);
            o.ts = g.value("ts", int64_t(0));
            o.time = g.value("time", std::string());
            o.checkpoint = g.value("checkpoint", std::string());
            o.checkpoint_failed = g.value("checkpoint_failed", false);
            o.fingerprint.size = g.value("size", uint64_t(0));
            o.fingerprint.sha256 = g.value("sha256", std::string());
            if (!o.checkpoint.empty() && !(file_exists && file_exists(o.checkpoint))) {
                o.checkpoint.clear();
                o.fingerprint = McpFingerprint();
            }
            return o;
        };
        for (const json &r : j.value("rows", json::array())) {
            McpRow o;
            o.id = r.value("id", 0);
            o.group = r.value("group", 0);
            o.ts = r.value("ts", int64_t(0));
            o.time = r.value("time", std::string());
            o.kind = r.value("kind", std::string());
            o.label = r.value("label", std::string());
            o.value = r.value("value", std::string());
            o.before = r.value("before", std::string());
            o.where = r.value("where", std::string());
            o.undo_verb = r.value("undo_verb", std::string());
            o.undo_tip = r.value("undo_tip", std::string());
            o.undone_at = r.value("undone_at", std::string());
            o.title_hint = r.value("title_hint", std::string());
            o.target = r.value("target", json());
            o.result = r.value("result", json());
            o.error = r.value("error", false);
            o.on_disk = r.value("on_disk", false);
            h.m_rows.push_back(std::move(o));
        }
        for (const json &g : j.value("groups", json::array())) h.m_groups.push_back(read_group(g));
        const json s = j.value("start", json());
        if (s.is_object()) {
            h.m_start = read_group(s);
            h.m_start.id = 0;
            h.m_has_start = true;
        }
        h.m_closed = j.value("closed", true);
        h.m_last_ts = j.value("last_ts", int64_t(0));
        int next = 1;
        for (const McpRow &r : h.m_rows) next = std::max(next, r.id + 1);
        h.m_next_row = std::max(j.value("next_row", 1), next);
    } catch (const std::exception &) {
        return McpHistory();
    }
    return h;
}

static char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

std::string mcp_history_key(const std::string &project_path_utf8, bool fold_case)
{
    if (project_path_utf8.empty()) return "untitled";
    // u8path/generic_u8string: on Windows the plain std::string forms go through the ANSI code page.
    std::string norm = std::filesystem::u8path(project_path_utf8).lexically_normal().generic_u8string();
    if (fold_case)
        for (char &c : norm) c = ascii_lower(c);
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : norm) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(hash));
    return buf;
}

// Absolute, weakly canonical (existing part with symlinks resolved), lexically normal, as UTF-8 components without
// empty ones (a trailing separator).
static std::vector<std::string> canonical_parts(const std::string &utf8)
{
    namespace sfs = std::filesystem;
    std::error_code ec;
    const sfs::path in  = sfs::u8path(utf8);
    sfs::path       abs = sfs::absolute(in, ec);
    if (ec) abs = in;
    ec.clear();
    sfs::path c = sfs::weakly_canonical(abs, ec);
    if (ec) c = abs;
    c = c.lexically_normal();
    std::vector<std::string> parts;
    for (const sfs::path &part : c) {
        std::string s = part.generic_u8string();
        if (s.empty()) continue;
        parts.push_back(std::move(s));
    }
    return parts;
}

bool mcp_path_within(const std::string &dir_utf8, const std::string &path_utf8, bool fold_case)
{
    if (dir_utf8.empty() || path_utf8.empty()) return false;
    const std::vector<std::string> d = canonical_parts(dir_utf8), p = canonical_parts(path_utf8);
    if (d.empty() || p.size() < d.size()) return false;
    for (size_t i = 0; i < d.size(); ++i) {
        if (d[i].size() != p[i].size()) return false;
        for (size_t k = 0; k < d[i].size(); ++k) {
            const char a = fold_case ? ascii_lower(d[i][k]) : d[i][k];
            const char b = fold_case ? ascii_lower(p[i][k]) : p[i][k];
            if (a != b) return false;
        }
    }
    return true;
}

bool mcp_target_current(const json &target, const std::string &stamp)
{
    if (!target.is_object() || stamp.empty()) return false;
    auto it = target.find("load");
    return it != target.end() && it->is_string() && it->get<std::string>() == stamp;
}

int mcp_find_object(const json &target, bool current, const std::string &fallback_name, const std::vector<McpObjectRef> &now)
{
    if (!target.is_object()) return -2;
    if (current) {
        auto it = target.find("object_id");
        if (it == target.end() || !it->is_number_integer()) return -1;
        const uint64_t id = it->get<uint64_t>();
        for (size_t i = 0; i < now.size(); ++i)
            if (now[i].id == id) return int(i);
        return -1;
    }
    std::string name = fallback_name;
    auto        nit  = target.find("object_name");
    if (nit != target.end() && nit->is_string() && !nit->get<std::string>().empty()) name = nit->get<std::string>();
    if (name.empty()) return -2;
    int found = -2;
    for (size_t i = 0; i < now.size(); ++i)
        if (now[i].name == name) {
            if (found >= 0) return -2; // more than one: can't tell which
            found = int(i);
        }
    return found;
}

int mcp_find_instance(const json &target, bool current, const std::vector<uint64_t> &instance_ids)
{
    if (!target.is_object()) return -1;
    if (current) {
        auto it = target.find("instance_id");
        if (it == target.end() || !it->is_number_integer()) return -1;
        const uint64_t id = it->get<uint64_t>();
        for (size_t i = 0; i < instance_ids.size(); ++i)
            if (instance_ids[i] == id) return int(i);
        return -1;
    }
    auto it = target.find("instance");
    if (it == target.end() || !it->is_number_integer()) return -1;
    const int64_t i = it->get<int64_t>();
    return i >= 0 && i < int64_t(instance_ids.size()) ? int(i) : -1;
}

std::vector<std::string> mcp_stale_history_dirs(const std::vector<std::pair<std::string, int64_t>> &dirs_with_mtime,
                                                int64_t now_s, int64_t max_age_s)
{
    std::vector<std::string> out;
    for (const auto &d : dirs_with_mtime)
        if (now_s - d.second > max_age_s) out.push_back(d.first);
    return out;
}

}}} // namespace Slic3r::GUI::RemoteAPI
