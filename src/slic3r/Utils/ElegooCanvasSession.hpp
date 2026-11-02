#ifndef slic3r_ElegooCanvasSession_hpp_
#define slic3r_ElegooCanvasSession_hpp_

// Short-lived MQTT sessions with an Elegoo Centauri Carbon 2 (CC2): read printer attributes and
// CANVAS status, start a print. Standalone: depends on ElegooMqtt, ElegooCanvas, nlohmann and std only.
// Each call connects, subscribes, registers, sends one request, waits for its reply and disconnects.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "ElegooCanvas.hpp"
#include "ElegooMqtt.hpp"

namespace Slic3r { namespace ElegooCanvas {

struct Endpoint
{
    std::string host;
    uint16_t    port = 1883;
    std::string access_code;
    std::string serial;
};

class Session
{
public:
    using TransportFactory = std::function<std::unique_ptr<ElegooMqtt::Transport>()>;

    struct Options
    {
        std::string               client_id;            // empty: "1_PC_<1000-9999>", random
        int                       first_request_id = 0; // 0: random; later requests count up from it
        std::chrono::milliseconds connect_timeout{5000};
        std::chrono::milliseconds register_timeout{3000};
        std::chrono::milliseconds request_timeout{10000};
    };

    struct Result
    {
        bool           ok = false;
        // "unreachable", "registration_refused", "too_many_clients", "timeout", "printer_error", "firmware_too_old"
        std::string    error_code;
        std::string    message;
        nlohmann::json result; // the reply's "result" object when ok
    };

    // With no factory, each call opens a TCP connection to host:port.
    explicit Session(Endpoint endpoint, TransportFactory make_transport = {});
    Session(Endpoint endpoint, TransportFactory make_transport, Options options);

    Result get_attributes();    // method 1001
    Result get_canvas_status(); // method 2005, result carries "canvas_info"
    // method 1020, params {storage_media:"local", filename, config}
    Result start_print(const std::string &filename, const nlohmann::json &config);

    // Attributes, then CANVAS status: fills `out` (firmware included). Fails with "firmware_too_old"
    // when the firmware version parses below 1.1; an unknown version does not block.
    Result get_canvas_info(Info &out);

    const std::string &client_id() const { return m_client_id; }

private:
    Result call(int method, const nlohmann::json &params);

    Endpoint         m_endpoint;
    TransportFactory m_make_transport;
    Options          m_options;
    std::string      m_client_id;
    int              m_next_request_id = 1;
};

// Firmware version from a 1001 result: result["software_version"]["ota_version"] (as elegoo-link reads it);
// "" when either is missing or not a string.
std::string firmware_from_attributes(const nlohmann::json &result);

// True when `version` ("1.0.9", "V1.0.9.3") parses and is below major.minor. Unparsable or empty: false.
bool firmware_below(const std::string &version, int major, int minor);

// The 1020 `config` object from the send dialog's options. `slot_map_text` is the JSON array text from
// extended_info["slot_map"]; empty text or an empty array leaves slot_map and bedlevel_force out
// (single-filament job). Throws std::invalid_argument when the text is not a JSON array.
nlohmann::json start_print_config(bool time_lapse, bool bed_leveling, bool pc_plate, const std::string &slot_map_text);

}} // namespace Slic3r::ElegooCanvas

#endif
