#include "McpRecorder.hpp"
#include "RemoteAPIController.hpp" // set_backup_in_progress: the F8 gate around the checkpoint export
#include "McpText.hpp"

#include "libslic3r/Format/bbs_3mf.hpp" // backup_defer
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp" // print_config_def: row labels and units
#include "libslic3r/Utils.hpp" // data_dir, ScopeGuard
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <wx/datetime.h>
#include <wx/thread.h>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <openssl/evp.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <exception>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace RemoteAPI {

namespace fs = boost::filesystem;
using nlohmann::json;

std::atomic<bool>     McpRecorder::s_project_opened{false};
std::atomic<uint64_t> McpRecorder::s_load_gen{0};
bool                  McpRecorder::s_opening_point = false;
std::atomic<int64_t> McpRecorder::s_clock_offset{0};
std::atomic<int>     McpRecorder::s_checkpoint_delay{0};

static const char *const kMiddleDot = "\xC2\xB7"; // U+00B7, as the page writes it

static fs::path history_root() { return fs::path(data_dir()) / "mcp_history"; }

// Writes text to a temp file beside dst, then renames it over dst (replaces an existing file).
static bool write_file_atomic(const fs::path &dst, const std::string &text)
{
    const fs::path tmp = dst.parent_path() / (dst.filename().string() + ".tmp");
    boost::system::error_code ec;
    try {
        {
            boost::nowide::ofstream out(tmp.string().c_str(), std::ios::binary | std::ios::trunc);
            if (!out) throw std::runtime_error("cannot open for writing");
            out << text;
            out.close();
            if (!out) throw std::runtime_error("write failed");
        }
        fs::rename(tmp, dst, ec);
        if (ec) throw std::runtime_error(ec.message());
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "MCP panel: " << dst.string() << " not written: " << e.what();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

// Moves a history dir (replacing what is at `to`); rename, else copy file by file.
static bool move_history_dir(const fs::path &from, const fs::path &to)
{
    boost::system::error_code ec;
    if (!fs::is_directory(from, ec)) return true; // nothing recorded there yet
    fs::remove_all(to, ec);
    ec.clear();
    fs::rename(from, to, ec);
    if (!ec) return true;
    ec.clear();
    fs::create_directories(to, ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(error) << "MCP panel: history not moved to " << to.string() << ": " << ec.message();
        return false;
    }
    for (fs::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        boost::system::error_code cec;
        fs::copy_file(it->path(), to / it->path().filename(), fs::copy_options::overwrite_existing, cec);
        if (cec) {
            BOOST_LOG_TRIVIAL(error) << "MCP panel: history not moved to " << to.string() << ": " << cec.message();
            return false;
        }
    }
    fs::remove_all(from, ec);
    return true;
}

// The page's formatDuration: 40s, 48m, 1h 48m.
static std::string duration_text(double sec)
{
    const long s = std::lround(std::max(0.0, sec));
    if (s < 60) return std::to_string(s) + "s";
    const long mins = std::lround(std::max(0.0, sec) / 60.0);
    if (mins < 60) return std::to_string(mins) + "m";
    return std::to_string(mins / 60) + "h " + std::to_string(mins % 60) + "m";
}

static std::string grams_text(double g)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f g", g);
    return buf;
}

McpRecorder &McpRecorder::get()
{
    static McpRecorder instance;
    return instance;
}

McpRecorder::McpRecorder() : m_stash(std::make_unique<Model>())
{
    // Unique per app run: a history reloaded after a restart names stash ids of an earlier run.
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    m_stash_session  = std::to_string(static_cast<long long>(std::time(nullptr))) + "-" +
                      std::to_string(static_cast<long long>(ticks % 1000000007LL));
}

McpRecorder::~McpRecorder() = default;

int McpRecorder::stash_object(const ModelObject &obj, int plate)
{
    // Model::add_object clones (new ids): volumes, modifiers, paint, config, layer ranges and instances.
    ModelObject *copy = m_stash->add_object(obj);
    const int    id   = m_next_stash++;
    m_stashed[id]     = {copy, plate};
    while (m_stashed.size() > kMaxStashed) {
        auto oldest = m_stashed.begin();
        m_stash->delete_object(oldest->second.first);
        m_stashed.erase(oldest);
    }
    return id;
}

const ModelObject *McpRecorder::stashed_object(int stash, const std::string &session) const
{
    if (session != m_stash_session) return nullptr;
    auto it = m_stashed.find(stash);
    return it == m_stashed.end() ? nullptr : it->second.first;
}

int McpRecorder::stashed_plate(int stash) const
{
    auto it = m_stashed.find(stash);
    return it == m_stashed.end() ? -1 : it->second.second;
}

void McpRecorder::note_created_plate(int row, uint64_t plate_object_id)
{
    if (row > 0 && plate_object_id != 0) m_created_plates[row] = plate_object_id;
}

int McpRecorder::created_plate_index(int row, const std::vector<uint64_t> &plate_object_ids) const
{
    auto it = m_created_plates.find(row);
    if (it == m_created_plates.end()) return -1;
    for (size_t i = 0; i < plate_object_ids.size(); ++i)
        if (plate_object_ids[i] == it->second) return int(i);
    return -1;
}

std::string McpRecorder::num_text(double v)
{
    if (!std::isfinite(v)) return "0";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    std::string t = buf;
    while (!t.empty() && t.back() == '0') t.pop_back();
    if (!t.empty() && t.back() == '.') t.pop_back();
    if (t == "-0") t = "0";
    return t;
}

std::string McpRecorder::config_label(const std::string &key)
{
    const ConfigOptionDef *d = print_config_def.get(key);
    if (d == nullptr) return key;
    if (!d->full_label.empty()) return d->full_label;
    return d->label.empty() ? key : d->label;
}

// Cuts at a UTF-8 character boundary at or before `max` bytes.
static std::string utf8_cut(const std::string &s, size_t max)
{
    if (s.size() <= max) return s;
    size_t cut = max;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut);
}

std::string McpRecorder::config_value_text(const std::string &key, const std::string &raw)
{
    static const char *const kEllipsis = "\xE2\x80\xA6"; // U+2026
    const ConfigOptionDef   *d         = print_config_def.get(key);
    std::string              v         = raw;
    bool                     cut       = false;
    const size_t             nl        = v.find_first_of("\r\n");
    if (nl != std::string::npos) {
        v   = v.substr(0, nl);
        cut = true;
    }
    bool labelled = false; // an enum label or On/Off: no unit after it
    if (d != nullptr && !cut) {
        if (d->type == coBool && (v == "1" || v == "0" || v == "true" || v == "false")) {
            v        = (v == "1" || v == "true") ? MCP_T("On") : MCP_T("Off");
            labelled = true;
        } else if (d->type == coEnum && d->enum_values.size() == d->enum_labels.size()) {
            for (size_t i = 0; i < d->enum_values.size(); ++i)
                if (d->enum_values[i] == v) {
                    if (!d->enum_labels[i].empty()) v = d->enum_labels[i];
                    labelled = true;
                    break;
                }
        }
    }
    if (v.size() > 40) {
        v   = utf8_cut(v, 40);
        cut = true;
    }
    if (cut) return v + kEllipsis;
    if (d != nullptr && !labelled && !v.empty() && !d->sidetext.empty() && v.back() != '%') {
        // "mm or %" (float-or-percent) reads as mm for a plain number; a percentage carries its own sign.
        std::string unit = d->sidetext;
        const size_t orp = unit.find(" or ");
        if (orp != std::string::npos) unit = unit.substr(0, orp);
        if (!unit.empty() && unit != "%") v += " " + unit;
    }
    return v;
}

nlohmann::json McpRecorder::instance_matrix(const ModelInstance &mi)
{
    const double  *m = mi.get_matrix().matrix().data();
    nlohmann::json a = nlohmann::json::array();
    for (int i = 0; i < 16; ++i) a.push_back(m[i]);
    return a;
}

int64_t McpRecorder::now_s() const { return static_cast<int64_t>(std::time(nullptr)) + s_clock_offset.load(); }

std::string McpRecorder::hhmm(int64_t epoch_s)
{
    return std::string(wxDateTime(static_cast<time_t>(epoch_s)).Format("%H:%M").ToUTF8().data());
}

std::string McpRecorder::history_dir() const
{
    return m_loaded ? (history_root() / m_key).string() : std::string();
}

std::string McpRecorder::checkpoint_path(const std::string &file) const
{
    if (!m_loaded || file.empty()) return std::string();
    return (history_root() / m_key / file).string();
}

std::string McpRecorder::history_root_dir() { return history_root().string(); }

McpFingerprint McpRecorder::fingerprint(const std::string &path_utf8)
{
    McpFingerprint fp;
    boost::nowide::ifstream in(path_utf8.c_str(), std::ios::binary);
    if (!in) return fp;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (ctx == nullptr) return fp;
    ScopeGuard free_ctx([ctx] { EVP_MD_CTX_free(ctx); });
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) return fp;
    std::vector<char> buf(size_t(1) << 16);
    uint64_t          size = 0;
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize n = in.gcount();
        if (n <= 0) break;
        if (EVP_DigestUpdate(ctx, buf.data(), static_cast<size_t>(n)) != 1) return fp;
        size += static_cast<uint64_t>(n);
    }
    if (in.bad()) return fp;
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int  len = 0;
    if (EVP_DigestFinal_ex(ctx, md, &len) != 1) return fp;
    static const char *const hex = "0123456789abcdef";
    std::string h;
    h.reserve(size_t(len) * 2);
    for (unsigned int i = 0; i < len; ++i) {
        h += hex[md[i] >> 4];
        h += hex[md[i] & 15];
    }
    fp.size   = size;
    fp.sha256 = std::move(h);
    return fp;
}

std::string McpRecorder::load_stamp() const { return m_stash_session + "/" + std::to_string(s_load_gen.load()); }

void McpRecorder::notify()
{
    if (m_listener) m_listener();
}

void McpRecorder::changed()
{
    if (!wxThread::IsMain()) return;
    save();
    notify();
}

void McpRecorder::schedule_flush()
{
    m_dirty = true;
    if (m_flush_posted) return;
    m_flush_posted = true;
    // After the current GUI task: every row of an API request (a PUT with ten keys) is written in one go.
    wxGetApp().CallAfter([] { McpRecorder::get().flush(); });
}

void McpRecorder::flush()
{
    if (!wxThread::IsMain()) return;
    m_flush_posted = false;
    if (!m_dirty) return;
    save();
    notify();
}

void McpRecorder::save()
{
    m_dirty = false;
    if (!m_loaded) return;
    const fs::path dir = history_root() / m_key;
    boost::system::error_code ec;
    // A bounded timeline: the oldest groups beyond the row / group caps go, with their restore points.
    for (const std::string &gone : m_history.cap_rows()) {
        boost::system::error_code rec;
        fs::remove(dir / gone, rec);
    }
    // Nothing recorded: no folder for every project the pane merely looked at.
    if (m_history.rows().empty() && m_history.groups().empty()) return;
    fs::create_directories(dir, ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(error) << "MCP panel: cannot create " << dir.string() << ": " << ec.message();
        return;
    }
    write_file_atomic(dir / "history.json", m_history.to_json().dump());
}

void McpRecorder::load(const std::string &key, bool fresh)
{
    m_key    = key;
    m_loaded = true;
    m_open   = false;
    m_deferred_point = -1;
    m_history = McpHistory();
    ++m_history_epoch;
    m_created_plates.clear();
    const fs::path dir = history_root() / key;
    boost::system::error_code ec;
    if (fresh) {
        // Untitled keeps only the latest session.
        fs::remove_all(dir, ec);
        return;
    }
    const fs::path file = dir / "history.json";
    if (!fs::exists(file, ec)) return;
    json j;
    try {
        boost::nowide::ifstream in(file.string().c_str(), std::ios::binary);
        if (!in) throw std::runtime_error("cannot open for reading");
        j = json::parse(in);
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(warning) << "MCP panel: " << file.string() << " ignored: " << e.what();
        return;
    }
    m_history = McpHistory::from_json(j, [dir](const std::string &name) {
        boost::system::error_code fec;
        return !name.empty() && fs::exists(dir / name, fec);
    });
    // Reopened: the next change starts a group of its own, whatever the file says.
    m_history.close_group();
    // "Not opened for 30 days" is judged by this file's mtime: opening counts, not only API changes.
    fs::last_write_time(file, std::time(nullptr), ec);
}

void McpRecorder::sweep_stale_dirs(const std::string &keep)
{
    const fs::path root = history_root();
    boost::system::error_code ec;
    if (!fs::is_directory(root, ec)) return;
    std::vector<std::pair<std::string, int64_t>> dirs;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        boost::system::error_code dec;
        if (!fs::is_directory(it->path(), dec)) continue;
        const std::string name = it->path().filename().string();
        if (name == keep) continue;
        const fs::path h = it->path() / "history.json";
        const std::time_t t = fs::exists(h, dec) ? fs::last_write_time(h, dec) : fs::last_write_time(it->path(), dec);
        if (dec) continue;
        dirs.emplace_back(name, static_cast<int64_t>(t));
    }
    // Real time, not the test clock: a test hook must never age out real history.
    for (const std::string &name : mcp_stale_history_dirs(dirs, static_cast<int64_t>(std::time(nullptr)), kStaleAgeS)) {
        boost::system::error_code rec;
        fs::remove_all(root / name, rec);
        BOOST_LOG_TRIVIAL(info) << "MCP panel: deleted unused history " << name;
    }
}

void McpRecorder::sync_project()
{
    if (!wxThread::IsMain()) return;
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr) return;
    // Mid-load (events pumped inside Plater::load_project, the pane refreshing) the project name may
    // already be the new file's while notify_project_opened has not run yet: judging the path now would
    // take the open for a Save As / first save. Wait for the load to end. notify_project_opened runs at
    // the end of load_project while m_loading_project is still true (its ScopeGuard clears it on return),
    // so by the time this check passes, the opened flag of that load is set. The flag is read only after
    // this check, so a mid-load sync cannot consume it either. Every other path change (save, Save As,
    // New project, an API move via project_moved) happens outside a load and needs no flag.
    if (plater->is_loading_project()) return;
    const std::string path   = plater->get_project_filename(".3mf").ToUTF8().data();
    const std::string key    = mcp_history_key(path);
    const bool        opened = s_project_opened.exchange(false);

    if (!m_swept) {
        m_swept = true;
        sweep_stale_dirs(key);
    }

    if (!m_loaded) {
        load(key, key == "untitled");
    } else if (key == m_key) {
        // The same project opened again: keep the timeline, start a new group.
        if (opened) {
            m_history.close_group();
            m_open = false;
        }
        return;
    } else if (opened || key == "untitled") {
        // Another project opened, or a new (Untitled) project.
        save();
        load(key, key == "untitled");
    } else if (m_key == "untitled") {
        // First save of an Untitled project: its history becomes the file's.
        move_history_dir(history_root() / m_key, history_root() / key);
        m_key = key;
        save();
    } else {
        // Save As to another path: the old path keeps its history, the new one starts empty here.
        save();
        boost::system::error_code ec;
        fs::remove_all(history_root() / key, ec);
        m_key     = key;
        m_open    = false;
        m_deferred_point = -1;
        m_history = McpHistory();
        ++m_history_epoch;
        m_created_plates.clear();
    }
    notify();
}

void McpRecorder::project_moved(const std::string &old_path_utf8, const std::string &new_path_utf8)
{
    if (!wxThread::IsMain()) return;
    if (old_path_utf8.empty() || new_path_utf8.empty()) return;
    const std::string from = mcp_history_key(old_path_utf8);
    const std::string to   = mcp_history_key(new_path_utf8);
    if (from == to) return;
    if (m_loaded && m_key == from) {
        save();
        move_history_dir(history_root() / from, history_root() / to);
        m_key = to;
        save();
        notify();
    } else if (m_loaded && m_key == to) {
        // A sync ran between the move and this call and took it for a Save As (empty history): adopt the
        // moved history instead. Never replace a history that already has rows.
        if (!m_history.rows().empty()) return;
        move_history_dir(history_root() / from, history_root() / to);
        load(to, false);
        notify();
    } else {
        // Not the open project: its history (if any) follows the file.
        move_history_dir(history_root() / from, history_root() / to);
    }
}

bool McpRecorder::write_preset_checkpoint(const std::string &path_utf8)
{
    const PresetBundle *b = wxGetApp().preset_bundle;
    if (b == nullptr) return false;
    // The edited preset's values of each dirty option (the unsaved edits over the selected preset).
    auto edits = [](const PresetCollection &c) {
        json o = json::object();
        const DynamicPrintConfig &cfg = c.get_edited_preset().config;
        for (const std::string &k : c.current_dirty_options())
            if (const ConfigOption *opt = cfg.option(k)) o[k] = opt->serialize();
        return o;
    };
    json j = json::object();
    j["printer"]   = b->printers.get_selected_preset_name();
    j["print"]     = b->prints.get_selected_preset_name();
    j["filaments"] = b->filament_presets;
    j["edits"]     = {{"print", edits(b->prints)}, {"filament", edits(b->filaments)}, {"printer", edits(b->printers)}};
    return write_file_atomic(fs::path(path_utf8), j.dump());
}

std::string McpRecorder::write_checkpoint(const std::string &stem)
{
    if (!wxThread::IsMain()) return std::string();
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr || !m_loaded) return std::string();
    // Mid-load (an arrange job finishing inside a restore's load pump, say) the model is half there: no point.
    if (s_opening_point || plater->is_loading_project()) {
        BOOST_LOG_TRIVIAL(warning) << "MCP panel: no checkpoint " << stem << " while a project is loading";
        return std::string();
    }
    const fs::path dir = history_root() / m_key;
    boost::system::error_code ec;
    fs::create_directories(dir, ec);
    // An empty project cannot be opened as a 3MF: keep the presets and their edits instead.
    const bool        empty_plate = plater->model().objects.empty();
    const std::string name        = stem + (empty_plate ? ".json" : ".3mf");
    const fs::path    file        = dir / name;

    const auto t0 = std::chrono::steady_clock::now();
    bool ok = false;
    if (!ec) {
        const int delay = s_checkpoint_delay.load();
        if (delay > 0) std::this_thread::sleep_for(std::chrono::seconds(delay));
        if (empty_plate) {
            ok = write_preset_checkpoint(file.string());
        } else {
            // export_3mf pumps the event queue: park API tasks for its duration (F8), as the auto-backup does.
            // Also safe outside an API task (the restore snapshot): the periodic auto-backup is pushed out
            // past the longest checkpoint we keep, and MainFrame skips it while backup_in_progress() anyway.
            Slic3r::backup_defer(static_cast<long>(kCheckpointMaxS));
            Controller::set_backup_in_progress(true);
            m_writing_point = true;
            ScopeGuard gate([this] {
                m_writing_point = false;
                Controller::set_backup_in_progress(false);
                if (!m_deferred_slices.empty())
                    wxGetApp().CallAfter([] {
                        McpRecorder &rec = McpRecorder::get();
                        std::vector<DeferredSlice> slices;
                        slices.swap(rec.m_deferred_slices);
                        for (const DeferredSlice &d : slices) rec.slice_finished(d.stats, d.ok, d.error);
                    });
            });
            try {
                ok = plater->export_3mf_copy(file);
            } catch (const std::exception &e) {
                BOOST_LOG_TRIVIAL(error) << "MCP panel: checkpoint export failed: " << e.what();
                ok = false;
            } catch (...) {
                ok = false;
            }
        }
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (!ok || secs > double(kCheckpointMaxS)) {
        boost::system::error_code rec;
        fs::remove(file, rec);
        BOOST_LOG_TRIVIAL(warning) << "MCP panel: no checkpoint " << name << (ok ? " (took " : " (failed after ") << secs
                                   << " s)";
        return std::string();
    }
    return name;
}

void McpRecorder::take_checkpoint(int point)
{
    if (!m_loaded) return;
    // A panel restore already gave this point its end state (the pre-restore snapshot); the state now is the
    // restored one, which belongs to the point restored to, not to this one.
    if (!m_history.checkpoint_of(point).empty()) return;
    std::string    name = write_checkpoint("cp-" + std::to_string(point));
    McpFingerprint fp;
    if (!name.empty()) {
        fp = fingerprint(checkpoint_path(name));
        if (fp.sha256.empty()) {
            // Unreadable right after writing: a point that can never be checked is no point.
            boost::system::error_code rec;
            fs::remove(history_root() / m_key / name, rec);
            name.clear();
        }
    }
    m_history.set_checkpoint(point, name, name.empty(), fp);
    prune_checkpoints();
}

void McpRecorder::prune_checkpoints()
{
    const fs::path dir = history_root() / m_key;
    for (const std::string &gone : m_history.prune_checkpoints()) {
        boost::system::error_code rec;
        fs::remove(dir / gone, rec);
    }
}

void McpRecorder::set_point_checkpoint(int point, const std::string &file, const McpFingerprint &fingerprint, bool prune)
{
    if (!m_loaded || file.empty()) return;
    m_history.set_checkpoint(point, file, false, fingerprint);
    if (prune) prune_checkpoints();
}

void McpRecorder::close_group()
{
    m_history.close_group();
    m_open           = false;
    m_deferred_point = -1;
}

void McpRecorder::before_change() { open_change(true); }

void McpRecorder::open_change(bool checkpoint)
{
    if (!wxThread::IsMain()) return;
    sync_project();
    const McpChange c = m_history.begin_change(now_s());
    m_open = true;
    if (!c.opened) {
        // A refusal opened this group without a checkpoint. Its first real change takes the skipped one now:
        // nothing has changed through the API since, so it is still the state before the group.
        if (checkpoint && m_deferred_point >= 0) {
            take_checkpoint(m_deferred_point);
            m_deferred_point = -1;
            schedule_flush();
        }
        return;
    }
    // No checkpoint when slice_finished or a refusal opens the group: a slice result runs from the
    // slicing-completed event, not inside an API task, and neither changes anything in the project.
    m_deferred_point = checkpoint ? -1 : c.prev_point;
    if (checkpoint) take_checkpoint(c.prev_point);
    schedule_flush();
}

int McpRecorder::record(McpRow row)
{
    if (!wxThread::IsMain()) return 0;
    sync_project();
    if (!m_open) before_change();
    row.ts   = now_s();
    row.time = hhmm(row.ts);
    const int id = m_history.add_row(std::move(row));
    schedule_flush();
    return id;
}

void McpRecorder::record_refusal(std::string label, std::string why, std::string value, json target)
{
    if (!wxThread::IsMain()) return;
    // A refused change still belongs to the burst, but changed nothing: no checkpoint when it opens a group.
    open_change(false);
    McpRow r;
    r.kind   = "refused";
    r.label  = std::move(label);
    r.value  = value.empty() ? std::string(MCP_T("Not allowed")) : std::move(value);
    r.where  = std::move(why);
    r.error  = true;
    r.target = std::move(target);
    record(std::move(r));
}

void McpRecorder::record_note(const std::string &kind, const std::string &text, const std::string &value,
                              const std::string &where)
{
    if (!wxThread::IsMain()) return;
    if (kind == "refused") {
        record_refusal(text, where, value.empty() ? std::string(MCP_T("Not allowed")) : value, {{"type", "refused"}, {"action", "note"}});
        return;
    }
    if (kind == "note") {
        // The assistant's own words (orcaslicer-mcp #21), in the user's language: a plain row in the group still open
        // from the changes it explains, or a new group without a restore point, like a refusal. It changes nothing,
        // has no undo and does not end the group.
        open_change(false);
        McpRow r;
        r.kind   = "note";
        r.label  = text;
        r.value  = value;
        r.where  = where;
        r.target = {{"type", "note"}};
        record(std::move(r));
        return;
    }
    sync_project();
    // P4: a note sums up something the connector did on its own (a comparison whose trial requests recorded
    // nothing), so it never joins a group left open earlier: it starts one of its own, without a restore point.
    close_group();
    open_change(false);
    McpRow r;
    r.kind       = "slice";
    r.label      = text;
    r.value      = value;
    r.where      = where;
    r.title_hint = "note";
    r.target     = {{"type", "note"}};
    r.ts         = now_s();
    r.time       = hhmm(r.ts);
    m_history.add_row(std::move(r));
    // Like a slice result, the note ends its group.
    m_history.close_group();
    m_open = false;
    save();
    notify();
}

void McpRecorder::slice_finished(const json &stats, bool ok, const std::string &error)
{
    if (!wxThread::IsMain()) return;
    if (m_writing_point) {
        // The slice ended inside a checkpoint export's event pump: recorded once the export is over (posted from
        // write_checkpoint), not in the middle of the group that export is opening. Queued, not re-posted: a
        // re-posted call would run again inside the same pump.
        m_deferred_slices.push_back({stats, ok, error});
        return;
    }
    sync_project();
    if (!m_open) open_change(false);
    Plater *plater = wxGetApp().plater();
    const int plate_no = plater != nullptr ? plater->get_partplate_list().get_curr_plate_index() + 1 : 1;

    McpRow r;
    r.kind  = "slice";
    r.where = mcp_fmt(MCP_T("plate {n}"), {{"n", std::to_string(plate_no)}});
    if (ok) {
        double secs = -1, grams = -1;
        if (stats.is_object()) {
            auto it = stats.find("estimated_time_seconds");
            if (it != stats.end() && it->is_number()) secs = it->get<double>();
            it = stats.find("filament_used_g");
            if (it != stats.end() && it->is_number()) grams = it->get<double>();
        }
        r.label = MCP_T("Sliced");
        if (secs >= 0 && grams >= 0) r.value = duration_text(secs) + " " + kMiddleDot + " " + grams_text(grams);
        else if (secs >= 0)          r.value = duration_text(secs);
        else if (grams >= 0)         r.value = grams_text(grams);
        if (secs >= 0) r.result = {{"seconds", secs}, {"grams", std::max(0.0, grams)}};
        // The same plate's previous API slice, if any.
        const std::vector<McpRow> &rows = m_history.rows();
        for (auto it = rows.rbegin(); it != rows.rend(); ++it)
            if (it->kind == "slice" && !it->error && it->where == r.where && it->result.is_object()) {
                r.before = it->value;
                break;
            }
    } else if (error == "cancelled") {
        r.label = MCP_T("Slicing cancelled");
    } else {
        r.label = MCP_T("Slicing failed");
        r.error = true;
        std::string reason = error.substr(0, error.find('\n'));
        if (reason.size() > 80) {
            size_t cut = 77;
            while (cut > 0 && (static_cast<unsigned char>(reason[cut]) & 0xC0) == 0x80) --cut; // UTF-8 boundary
            reason = reason.substr(0, cut) + "...";
        }
        r.value = reason.empty() ? MCP_T("OrcaSlicer reported an error") : reason;
        if (!error.empty() && error != reason)
            r.where = mcp_fmt(MCP_T("plate {n} \xC2\xB7 {error}"), {{"n", std::to_string(plate_no)}, {"error", error}});
    }
    r.ts   = now_s();
    r.time = hhmm(r.ts);
    m_history.add_row(std::move(r));
    // A group normally ends with its result.
    m_history.close_group();
    m_open = false;
    save();
    notify();
}

}}} // namespace Slic3r::GUI::RemoteAPI
