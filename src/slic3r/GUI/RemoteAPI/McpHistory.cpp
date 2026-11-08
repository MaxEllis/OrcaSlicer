#include "McpHistory.hpp"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <filesystem>

namespace Slic3r { namespace GUI { namespace RemoteAPI {

using nlohmann::json;

bool McpHistory::begin_change(int64_t now_s)
{
    const bool open_new = m_groups.empty() || m_closed || now_s - m_last_ts >= kBurstGapS;
    if (open_new) {
        McpGroup g;
        g.id = m_groups.empty() ? 1 : m_groups.back().id + 1;
        g.ts = now_s;
        m_groups.push_back(std::move(g));
    }
    m_closed  = false;
    m_last_ts = now_s;
    return open_new;
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

int McpHistory::restore_point_for_new_group() const { return m_groups.empty() ? 0 : m_groups.back().id; }

void McpHistory::set_checkpoint(int point, std::string file, bool failed)
{
    if (point == 0) {
        m_has_start = true;
        m_start.id = 0;
        m_start.checkpoint = std::move(file);
        m_start.checkpoint_failed = failed;
        return;
    }
    for (McpGroup &g : m_groups)
        if (g.id == point) {
            g.checkpoint = std::move(file);
            g.checkpoint_failed = failed;
            return;
        }
}

std::vector<std::string> McpHistory::prune_checkpoints()
{
    // Oldest first: the start point, then groups by id.
    std::vector<McpGroup *> holders;
    if (m_has_start && !m_start.checkpoint.empty()) holders.push_back(&m_start);
    for (McpGroup &g : m_groups)
        if (!g.checkpoint.empty()) holders.push_back(&g);
    std::vector<std::string> out;
    if (holders.size() <= kMaxCheckpoints) return out;
    const size_t drop = holders.size() - kMaxCheckpoints;
    for (size_t i = 0; i < drop; ++i) {
        out.push_back(std::move(holders[i]->checkpoint));
        holders[i]->checkpoint.clear();
    }
    return out;
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

json McpHistory::page_history() const
{
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
                        {"result", r.result.is_null() ? json(nullptr) : r.result}, {"error", r.error},
                        {"on_disk", r.on_disk}});
    }
    json groups = json::array();
    if (m_has_start && !m_groups.empty())
        groups.push_back({{"id", 0}, {"ts", m_groups.front().ts}, {"time", m_groups.front().time},
                          {"restore", restore_json(m_start)}});
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
                    {"checkpoint_failed", g.checkpoint_failed}};
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
            if (!o.checkpoint.empty() && !(file_exists && file_exists(o.checkpoint))) o.checkpoint.clear();
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

std::string mcp_history_key(const std::string &project_path_utf8)
{
    if (project_path_utf8.empty()) return "untitled";
    const std::string norm = std::filesystem::path(project_path_utf8).lexically_normal().generic_string();
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : norm) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(hash));
    return buf;
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
