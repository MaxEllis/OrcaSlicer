#include "ElegooCanvasMatch.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <stdexcept>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

namespace Slic3r { namespace ElegooCanvas {

namespace {

std::string upper_trim(const std::string &s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    std::string r = s.substr(b, e - b);
    for (char &c : r)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

void erase_all(std::string &s, const std::string &sub)
{
    if (sub.empty())
        return;
    for (size_t p = s.find(sub); p != std::string::npos; p = s.find(sub, p))
        s.erase(p, sub.size());
}

// Upper-case, drop the vendor and "GENERIC" words and any "@..." suffix
// (same steps as ElegooSlicer's standardizeFilamentName).
std::string standardize_name(const std::string &name, const std::string &vendor)
{
    std::string s = upper_trim(name);
    const std::string v = upper_trim(vendor);
    if (!v.empty() && s.find(v) != std::string::npos) {
        erase_all(s, v);
        s = upper_trim(s);
    }
    if (s.find("GENERIC") != std::string::npos) {
        erase_all(s, "GENERIC");
        s = upper_trim(s);
    }
    const size_t at = s.find('@');
    if (at != std::string::npos)
        s = upper_trim(s.substr(0, at));
    std::replace(s.begin(), s.end(), '-', ' ');
    return upper_trim(s);
}

bool usable(const Tray &t) { return t.ready() && !t.unknown_filament; }

// Does this tray match the filament by standardised name, name-equals-type, or type?
// (Same three passes as ElegooSlicer's matchFilamentPreset with a single candidate.)
bool tray_matches(const ProjectFilament &f, const Tray &t)
{
    const std::string tray_name = standardize_name(t.filament_name, t.brand);
    const std::string fil_name  = standardize_name(f.alias.empty() ? f.name : f.alias, f.vendor);
    const std::string tray_type = upper_trim(t.filament_type);
    const std::string fil_type  = upper_trim(f.type);
    if (!tray_name.empty() && tray_name == fil_name)
        return true;
    if (tray_type.empty())
        return false; // the reference needs the tray's type for both fallbacks
    return tray_type == fil_name || tray_name == fil_type || tray_type == fil_type;
}

const char *kMappedType  = "mappedFilamentType";
const char *kMappedName  = "mappedFilamentName";
const char *kMappedColor = "mappedFilamentColor";

} // namespace

MappingStore::MappingStore(boost::filesystem::path file) : m_file(std::move(file)) {}

std::optional<SavedMapping> MappingStore::lookup_entry(const ProjectFilament &f) const
{
    const std::string key = color_bucket_hex(f.color);
    if (key.empty() || !m_json.is_object())
        return std::nullopt;
    auto t = m_json.find(f.type);
    if (t == m_json.end() || !t->is_object())
        return std::nullopt;
    auto a = t->find(f.alias);
    if (a == t->end() || !a->is_object())
        return std::nullopt;
    auto c = a->find(key);
    if (c == a->end() || !c->is_object())
        return std::nullopt;
    auto str = [&](const char *k) {
        auto it = c->find(k);
        return (it != c->end() && it->is_string()) ? it->get<std::string>() : std::string();
    };
    SavedMapping m{str(kMappedType), str(kMappedName), str(kMappedColor)};
    if (m.type.empty() || m.name.empty() || m.color.empty())
        return std::nullopt;
    return m;
}

std::optional<std::pair<std::string, std::string>> MappingStore::lookup(const ProjectFilament &f) const
{
    if (auto m = lookup_entry(f))
        return std::make_pair(m->name, m->color);
    return std::nullopt;
}

void MappingStore::remember(const ProjectFilament &f, const Tray &t)
{
    if (t.filament_name.empty() || t.filament_color.empty())
        return;
    const ColorBucket *mapped = nearest_color_bucket(t.filament_color);
    const ColorBucket *own    = nearest_color_bucket(f.color);
    if (!mapped || !own)
        return;
    if (!m_json.is_object())
        m_json = nlohmann::json::object();
    // Same record as ElegooSlicer's saveFilamentMmsMapping: type -> alias -> standard colour.
    // A hand-edited file may hold a non-object node: replace it rather than throw.
    if (!m_json[f.type].is_object())
        m_json[f.type] = nlohmann::json::object();
    if (!m_json[f.type][f.alias].is_object())
        m_json[f.type][f.alias] = nlohmann::json::object();
    m_json[f.type][f.alias][own->hex] = {
        {kMappedType, t.filament_type},
        {kMappedName, t.filament_name},
        {kMappedColor, t.filament_color},
        {"mappedFilamentStandardColor", mapped->hex},
        {"mappedFilamentStandardColorName", mapped->name},
        {"filamentStandardColor", own->hex},
        {"filamentStandardColorName", own->name},
    };
}

void MappingStore::save() const
{
    try {
        if (m_file.has_parent_path())
            boost::filesystem::create_directories(m_file.parent_path());
        boost::nowide::ofstream out(m_file.string().c_str(), std::ios::binary | std::ios::trunc);
        if (!out)
            throw std::runtime_error("cannot open for writing");
        out << m_json.dump(4);
        if (!out)
            throw std::runtime_error("write failed");
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "CANVAS mapping file not saved (" << m_file.string() << "): " << e.what();
    }
}

void MappingStore::load()
{
    m_json = nlohmann::json::object();
    try {
        if (!boost::filesystem::exists(m_file))
            return;
        boost::nowide::ifstream in(m_file.string().c_str(), std::ios::binary);
        if (!in)
            throw std::runtime_error("cannot open for reading");
        nlohmann::json j = nlohmann::json::parse(in);
        if (!j.is_object())
            throw std::runtime_error("top level is not an object");
        m_json = std::move(j);
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(warning) << "CANVAS mapping file ignored (" << m_file.string() << "): " << e.what();
        m_json = nlohmann::json::object();
    }
}

std::vector<Mapping> auto_match(const std::vector<ProjectFilament> &filaments, const Info &info, const MappingStore &store)
{
    std::vector<Mapping> out;
    out.reserve(filaments.size());
    for (const ProjectFilament &f : filaments) {
        Mapping m;
        m.index = f.index;
        auto set_tray = [&](const Tray &t, MatchSource src) {
            m.tray   = std::make_pair(t.canvas_id, t.tray_id);
            m.source = src;
            if (upper_trim(t.filament_type) != upper_trim(f.type))
                m.warnings.push_back("type_mismatch");
        };

        // 1. Saved choice: type + name + exact colour (case-insensitive) among usable trays.
        bool done = false;
        if (auto saved = store.lookup_entry(f)) {
            for (const Unit &u : info.units) {
                for (const Tray &t : u.trays) {
                    if (usable(t) && upper_trim(t.filament_type) == upper_trim(saved->type) &&
                        upper_trim(t.filament_name) == upper_trim(saved->name) &&
                        upper_trim(t.filament_color) == upper_trim(saved->color)) {
                        set_tray(t, MatchSource::Saved);
                        done = true;
                        break;
                    }
                }
                if (done)
                    break;
            }
        }

        // 2. Same colour bucket. As in ElegooSlicer, trays are scanned in order and the
        // FIRST usable tray whose name/type matches wins (no ranking across trays), so a
        // user switching slicers gets the same result.
        const std::string bucket = color_bucket(f.color);
        if (!done && !bucket.empty()) {
            for (const Unit &u : info.units) {
                for (const Tray &t : u.trays) {
                    if (usable(t) && color_bucket(t.filament_color) == bucket && tray_matches(f, t)) {
                        set_tray(t, MatchSource::Matched);
                        done = true;
                        break;
                    }
                }
                if (done)
                    break;
            }
        }
        out.push_back(std::move(m));
    }
    return out;
}

std::vector<std::string> validate(const std::vector<Mapping> &mappings, const Info &info)
{
    std::vector<std::string> msgs;
    std::map<std::pair<int, int>, std::vector<int>> by_tray;
    for (const Mapping &m : mappings) {
        const std::string n = std::to_string(m.index + 1);
        if (!m.tray) {
            msgs.push_back("Filament " + n + " is not mapped to a tray");
            continue;
        }
        by_tray[*m.tray].push_back(m.index + 1);
        const Tray *t = info.find(m.tray->first, m.tray->second);
        if (!t)
            msgs.push_back("Filament " + n + " is mapped to a tray that is not on the printer");
        else if (!t->ready())
            msgs.push_back("Filament " + n + " is mapped to tray " + t->label() + ", which is not ready");
    }
    for (const auto &kv : by_tray) {
        const auto &ids = kv.second;
        if (ids.size() < 2)
            continue;
        const Tray *t = info.find(kv.first.first, kv.first.second);
        std::string who = "Filaments ";
        for (size_t i = 0; i < ids.size(); ++i) {
            if (i > 0)
                who += (i + 1 == ids.size()) ? " and " : ", ";
            who += std::to_string(ids[i]);
        }
        msgs.push_back(who + " are both mapped to tray " + (t ? t->label() : "#" + std::to_string(kv.first.first) + "/" + std::to_string(kv.first.second)));
    }
    return msgs;
}

namespace {

// Same type, or the same family: one type is the other plus a "-..." suffix (PLA / PLA-CF, PETG / PETG-CF).
// PETG and PET-CF are different families. Both arguments upper-cased and trimmed.
bool same_type_family(const std::string &a, const std::string &b)
{
    if (a == b)
        return true;
    auto extends = [](const std::string &longer, const std::string &base) {
        return longer.size() > base.size() + 1 && longer.compare(0, base.size(), base) == 0 &&
               (longer[base.size()] == '-' || longer[base.size()] == ' ');
    };
    return !a.empty() && !b.empty() && (extends(a, b) || extends(b, a));
}

bool is_generic(const ProjectFilament &p)
{
    const std::string vendor = upper_trim(p.vendor);
    return vendor == "GENERIC" || vendor == "ORCAFILAMENTLIBRARY" || upper_trim(p.name).find("GENERIC") != std::string::npos ||
           upper_trim(p.alias).find("GENERIC") != std::string::npos;
}

// Preset name without the " @printer" suffix (the alias when the preset has one).
std::string base_name(const ProjectFilament &p)
{
    if (!p.alias.empty())
        return p.alias;
    const size_t at = p.name.find('@');
    return at == std::string::npos ? p.name : p.name.substr(0, at);
}

} // namespace

std::optional<size_t> match_tray_preset(const Tray &tray, const std::vector<ProjectFilament> &presets)
{
    if (upper_trim(tray.filament_name).empty())
        return std::nullopt;
    const std::string tray_brand = upper_trim(tray.brand);
    const std::string tray_type  = upper_trim(tray.filament_type);
    // The tray's name with and without its brand word ("Elegoo PLA" -> "PLA" and "ELEGOO PLA").
    const std::string tray_short = standardize_name(tray.filament_name, tray.brand);
    const std::string tray_full  = standardize_name(tray.filament_name, std::string());

    auto vendor_is_brand = [&](const ProjectFilament &p) { return !tray_brand.empty() && upper_trim(p.vendor) == tray_brand; };
    // Name tiers (as ElegooSlicer): the tray's brand, then the printer vendor's presets (Elegoo), then generic presets,
    // then any other. Within a rank the printer vendor's own preset beats an Orca filament library copy, then list order.
    auto name_rank = [&](const ProjectFilament &p) {
        int r = 3;
        if (vendor_is_brand(p))
            r = 0;
        else if (upper_trim(p.vendor) == "ELEGOO")
            r = 1;
        else if (is_generic(p))
            r = 2;
        return r * 2 + (p.library ? 1 : 0);
    };
    // Type tier: the tray's brand, then generic presets, then any other.
    auto type_rank = [&](const ProjectFilament &p) {
        int r = 2;
        if (vendor_is_brand(p))
            r = 0;
        else if (is_generic(p))
            r = 1;
        return r * 2 + (p.library ? 1 : 0);
    };
    // A tray with a known type only ever resolves to a preset of that type (or, for a name match, its family).
    auto type_ok = [&](const ProjectFilament &p, bool family) {
        if (tray_type.empty())
            return true;
        const std::string pt = upper_trim(p.type);
        return family ? same_type_family(pt, tray_type) : pt == tray_type;
    };
    auto best_of = [&](auto accept, auto rank) -> std::optional<size_t> {
        std::optional<size_t> best;
        int                   best_rank = std::numeric_limits<int>::max();
        for (size_t i = 0; i < presets.size(); ++i) {
            if (!accept(presets[i]))
                continue;
            const int r = rank(presets[i]);
            if (r < best_rank) { // strict: ties keep list order
                best      = i;
                best_rank = r;
            }
        }
        return best;
    };

    // 1. Standardised names equal: vendor word dropped on both sides ("PLA" = "Elegoo PLA" of vendor Elegoo), or the
    //    whole names equal ("ELEGOO PLA" = an Orca library "Elegoo PLA"). Exact equality only, never a prefix.
    if (!tray_short.empty())
        if (auto hit = best_of(
                [&](const ProjectFilament &p) {
                    if (!type_ok(p, true))
                        return false;
                    const std::string name = base_name(p);
                    const std::string p_short = standardize_name(name, p.vendor);
                    return (!p_short.empty() && p_short == tray_short) || standardize_name(name, tray.brand) == tray_short ||
                           standardize_name(name, std::string()) == tray_full;
                },
                name_rank))
            return hit;
    if (tray_type.empty())
        return std::nullopt;
    // 2. Name equals type, either way round ("Generic PLA" for a PLA tray, a tray named "PETG"), same type only.
    if (auto hit = best_of(
            [&](const ProjectFilament &p) {
                return type_ok(p, false) &&
                       (standardize_name(base_name(p), p.vendor) == tray_type || tray_short == upper_trim(p.type));
            },
            type_rank))
        return hit;
    // 3. Same type, exactly (PETG is not PET-CF, PLA is not PLA-CF).
    return best_of([&](const ProjectFilament &p) { return upper_trim(p.type) == tray_type; }, type_rank);
}

}} // namespace Slic3r::ElegooCanvas
