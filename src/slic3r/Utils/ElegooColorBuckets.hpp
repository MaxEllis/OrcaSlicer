#ifndef slic3r_ElegooColorBuckets_hpp_
#define slic3r_ElegooColorBuckets_hpp_

// Colour buckets for matching project filaments to CANVAS spools.
//
// The palette and the nearest-colour search (CIEDE2000 in CIE Lab, D65) are
// ported from ElegooSlicer's StandardColorMatcher
// (src/libslic3r/StandardColorMatcher.{hpp,cpp},
// https://github.com/elegooofficial/ElegooSlicer), licensed AGPL-3.0, the same
// licence as this project. The bucket hex values are also the keys of the
// remembered-mapping file, so they must stay identical to the original.

#include <cctype>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>

namespace Slic3r { namespace ElegooCanvas {

struct ColorBucket
{
    const char *name;
    const char *hex; // "#RRGGBB" upper-case
};

namespace colordetail {

inline const ColorBucket *palette(std::size_t &count)
{
    static const ColorBucket table[] = {
        {"white", "#FFFFFF"},
        {"yellow", "#FFF242"},
        {"light_yellow_green", "#DBF47A"},
        {"green", "#09CC3A"},
        {"dark_green", "#077747"},
        {"blue_green", "#0B6283"},
        {"cyan_green", "#0BE2A0"},
        {"sky_blue", "#74D9F3"},
        {"light_blue", "#48A7FA"},
        {"dark_blue", "#2850DF"},
        {"purple", "#433089"},
        {"light_purple", "#A03BF7"},
        {"pink_purple", "#F32FF8"},
        {"light_pink_purple", "#D4B1DD"},
        {"pink", "#F95D77"},
        {"red", "#F72221"},
        {"brown", "#7C4C00"},
        {"orange", "#F88D36"},
        {"beige", "#FCEBD7"},
        {"light_brown", "#D2C5A3"},
        {"dark_brown", "#AF7832"},
        {"dark_gray", "#898989"},
        {"light_gray", "#BCBCBC"},
        {"black", "#000000"},
    };
    count = sizeof(table) / sizeof(table[0]);
    return table;
}

struct Lab { double l, a, b; };

// Accepts "#RRGGBB" or "RRGGBB"; false for anything else.
inline bool parse_hex(const std::string &hex, double rgb[3])
{
    const std::size_t off = (!hex.empty() && hex[0] == '#') ? 1 : 0;
    if (hex.size() != off + 6)
        return false;
    for (int i = 0; i < 3; ++i) {
        int v = 0;
        for (int j = 0; j < 2; ++j) {
            const unsigned char c = static_cast<unsigned char>(hex[off + 2 * i + j]);
            if (!std::isxdigit(c))
                return false;
            v = v * 16 + (std::isdigit(c) ? c - '0' : std::tolower(c) - 'a' + 10);
        }
        rgb[i] = v;
    }
    return true;
}

inline Lab rgb_to_lab(const double rgb[3])
{
    double c[3];
    for (int i = 0; i < 3; ++i) {
        const double v = rgb[i] / 255.0;
        c[i] = (v > 0.04045) ? std::pow((v + 0.055) / 1.055, 2.4) : v / 12.92;
    }
    const double x = (c[0] * 0.4124564 + c[1] * 0.3575761 + c[2] * 0.1804375) * 100.0;
    const double y = (c[0] * 0.2126729 + c[1] * 0.7151522 + c[2] * 0.0721750) * 100.0;
    const double z = (c[0] * 0.0193339 + c[1] * 0.1191920 + c[2] * 0.9503041) * 100.0;
    auto pivot = [](double v) { return (v > 0.008856) ? std::cbrt(v) : (7.787 * v + 16.0 / 116.0); };
    const double fx = pivot(x / 95.047), fy = pivot(y / 100.0), fz = pivot(z / 108.883);
    return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}

inline double delta_e2000(const Lab &p, const Lab &q)
{
    const double pi = 3.14159265358979323846;
    const double c1 = std::sqrt(p.a * p.a + p.b * p.b);
    const double c2 = std::sqrt(q.a * q.a + q.b * q.b);
    const double cb7 = std::pow((c1 + c2) / 2.0, 7);
    const double g = 0.5 * (1.0 - std::sqrt(cb7 / (cb7 + std::pow(25.0, 7))));
    const double a1 = (1.0 + g) * p.a, a2 = (1.0 + g) * q.a;
    const double c1p = std::sqrt(a1 * a1 + p.b * p.b), c2p = std::sqrt(a2 * a2 + q.b * q.b);
    auto hue = [pi](double a, double b) {
        const double h = std::atan2(b, a) * 180.0 / pi;
        return h < 0.0 ? h + 360.0 : h;
    };
    const double h1 = hue(a1, p.b), h2 = hue(a2, q.b);
    const double dL = q.l - p.l, dC = c2p - c1p, hd = h2 - h1;
    double dh = 0.0;
    if (c1p * c2p != 0.0)
        dh = std::fabs(hd) <= 180.0 ? hd : (hd > 180.0 ? hd - 360.0 : hd + 360.0);
    const double dH = 2.0 * std::sqrt(c1p * c2p) * std::sin(dh * pi / 360.0);
    const double lb = (p.l + q.l) / 2.0, cb = (c1p + c2p) / 2.0;
    double hb = 0.0;
    if (c1p * c2p == 0.0)
        hb = h1 + h2;
    else
        hb = std::fabs(hd) <= 180.0 ? (h1 + h2) / 2.0 : (h1 + h2 + 360.0) / 2.0;
    if (hb >= 360.0)
        hb -= 360.0;
    const double T = 1.0 - 0.17 * std::cos((hb - 30.0) * pi / 180.0) + 0.24 * std::cos(2.0 * hb * pi / 180.0) +
                     0.32 * std::cos((3.0 * hb + 6.0) * pi / 180.0) - 0.20 * std::cos((4.0 * hb - 63.0) * pi / 180.0);
    const double dTheta = 30.0 * std::exp(-std::pow((hb - 275.0) / 25.0, 2.0));
    const double cb7p = std::pow(cb, 7);
    const double rC = 2.0 * std::sqrt(cb7p / (cb7p + std::pow(25.0, 7)));
    const double sL = 1.0 + 0.015 * std::pow(lb - 50.0, 2.0) / std::sqrt(20.0 + std::pow(lb - 50.0, 2.0));
    const double sC = 1.0 + 0.045 * cb;
    const double sH = 1.0 + 0.015 * cb * T;
    const double rT = -std::sin(2.0 * dTheta * pi / 180.0) * rC;
    return std::sqrt(std::pow(dL / sL, 2.0) + std::pow(dC / sC, 2.0) + std::pow(dH / sH, 2.0) + rT * (dC / sC) * (dH / sH));
}

} // namespace colordetail

// Nearest palette entry to `hex`; nullptr when `hex` is not a valid colour.
inline const ColorBucket *nearest_color_bucket(const std::string &hex)
{
    double rgb[3];
    if (!colordetail::parse_hex(hex, rgb))
        return nullptr;
    const colordetail::Lab in = colordetail::rgb_to_lab(rgb);
    std::size_t            n = 0;
    const ColorBucket     *table = colordetail::palette(n);
    const ColorBucket     *best = nullptr;
    double                 best_d = std::numeric_limits<double>::max();
    for (std::size_t i = 0; i < n; ++i) {
        double e[3];
        colordetail::parse_hex(table[i].hex, e);
        const double d = colordetail::delta_e2000(in, colordetail::rgb_to_lab(e));
        if (d < best_d) {
            best_d = d;
            best   = &table[i];
        }
    }
    return best;
}

// Bucket name ("red", "sky_blue", ...); "" for an unparsable colour.
inline std::string color_bucket(const std::string &hex)
{
    const ColorBucket *b = nearest_color_bucket(hex);
    return b ? b->name : std::string();
}

// Bucket hex ("#F72221"); "" for an unparsable colour. Key of the mapping file.
inline std::string color_bucket_hex(const std::string &hex)
{
    const ColorBucket *b = nearest_color_bucket(hex);
    return b ? b->hex : std::string();
}

}} // namespace Slic3r::ElegooCanvas

#endif
