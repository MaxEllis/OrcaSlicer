#ifndef slic3r_GUI_ElegooCanvasGate_hpp_
#define slic3r_GUI_ElegooCanvasGate_hpp_

#include <algorithm>
#include <string>

#ifndef ELEGOO_CANVAS_GATE_NO_CONFIG
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/ElegooLink.hpp"
#endif

namespace Slic3r {

// Profile gate for the CANVAS filament sync: an ElegooLink printer whose model is a Centauri Carbon 2 and whose
// profile does not switch multi-filament support off.
//
// `support_multi_filament` is a profile key ("1"/"0") that is not a declared config option, so it is absent from
// a loaded preset. Absent or empty counts as on (every CC2 profile carries "1"); only an explicit "0"/"false" turns it off.
inline bool elegoo_canvas_enabled(const std::string &host_type_serialized, bool is_cc2_model, std::string support_multi_filament)
{
    support_multi_filament.erase(std::remove(support_multi_filament.begin(), support_multi_filament.end(), '"'), support_multi_filament.end());
    const bool flag_off = support_multi_filament == "0" || support_multi_filament == "false";
    return host_type_serialized == "elegoolink" && is_cc2_model && !flag_off;
}

#ifndef ELEGOO_CANVAS_GATE_NO_CONFIG
inline bool elegoo_canvas_enabled(const DynamicPrintConfig &printer_cfg)
{
    const ConfigOption *host = printer_cfg.option("host_type");
    if (host == nullptr)
        return false;
    const ConfigOption *flag = printer_cfg.option("support_multi_filament");
    return elegoo_canvas_enabled(host->serialize(), ElegooLink::is_cc2_model(printer_cfg.opt_string("printer_model")),
                                 flag != nullptr ? flag->serialize() : std::string());
}
#endif

} // namespace Slic3r

#endif
