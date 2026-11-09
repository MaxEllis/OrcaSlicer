#pragma once
// The range rule of OrcaSlicer's own settings fields, for values the Remote API is asked to write. Pure C++ (no
// libslic3r): the controller turns a ConfigOption into McpRangeElem values and asks for the first one out of range.
//
// The rule is ConfigOptionDef::is_value_valid (Config.cpp), which Field::get_value_by_opt_type (the sidebar) and
// PrintConfig's validate() use:
//   - a nil value (NaN, or the nullable ints' nil) is valid only for a nullable option;
//   - with min == 0 any negative value is out of range (checked before the tolerance below);
//   - a value within 10^-4 of min or max is in range; else min <= value <= max.
// Plus the sidebar's literal limit for "mm or %" fields (Field.cpp, coFloatOrPercent): a value given in mm (not a
// percentage) above max_literal is out of range too (an outer wall line width of 40 mm, say).
// Only elements that change are judged: writing a value back that is already there never fails, as in the GUI.
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <vector>

namespace Slic3r { namespace GUI { namespace RemoteAPI {

struct McpRangeLimits
{
    double min           = -FLT_MAX;
    double max           = FLT_MAX;
    bool   nullable      = false;
    bool   literal_limit = false; // the option's unit text has "mm " (a "mm or %" field): max_literal applies in mm
    double max_literal   = 1;
};

struct McpRangeElem
{
    double value   = 0;
    bool   percent = false; // a float-or-percent value given as a percentage
    bool   nil     = false; // the nullable option's nil value
    bool   changed = true;  // differs from the value there now (only these are judged)
};

struct McpRangeHit
{
    bool   found   = false;
    size_t index   = 0;     // element index (0 for a scalar)
    double value   = 0;
    bool   percent = false;
    double min     = -FLT_MAX; // the bounds that refused it (FLT_MAX / -FLT_MAX: none on that side)
    double max     = FLT_MAX;
};

// ConfigOptionDef::is_value_valid with max_precision 4.
inline bool mcp_value_in_range(const McpRangeLimits &l, double v)
{
    if (std::isnan(v)) return l.nullable;
    if (l.min == 0. && v < 0) return false;
    const double ep = 1e-4;
    if (std::fabs(v - l.min) < ep || std::fabs(v - l.max) < ep) return true;
    return l.min <= v && v <= l.max;
}

// The first changed element out of range, with the bounds to name; found = false when all are in range.
inline McpRangeHit mcp_first_out_of_range(const McpRangeLimits &l, const std::vector<McpRangeElem> &elems)
{
    McpRangeHit hit;
    for (size_t i = 0; i < elems.size(); ++i) {
        const McpRangeElem &e = elems[i];
        if (!e.changed) continue;
        if (e.nil) {
            if (l.nullable) continue;
        } else if (!std::isfinite(e.value)) {
            // inf or NaN given as a value: never a valid setting (NaN of a nullable option is its nil, set above)
        } else if (mcp_value_in_range(l, e.value)) {
            if (!(l.literal_limit && !e.percent && e.value > l.max_literal)) continue;
            hit.found = true; hit.index = i; hit.value = e.value; hit.percent = false;
            hit.min = l.min; hit.max = l.max_literal;
            return hit;
        }
        hit.found   = true;
        hit.index   = i;
        hit.value   = e.value;
        hit.percent = e.percent;
        hit.min     = l.min;
        hit.max     = l.max;
        return hit;
    }
    return hit;
}

// The bound is a real limit (not the "no limit" default of a ConfigOptionDef).
inline bool mcp_range_bounded(double bound) { return std::fabs(bound) < FLT_MAX * 0.5; }

}}} // namespace Slic3r::GUI::RemoteAPI
