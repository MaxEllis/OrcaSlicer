#include "ElegooCanvasSession.hpp"

#include <cctype>
#include <exception>
#include <optional>
#include <random>
#include <stdexcept>
#include <utility>

namespace Slic3r { namespace ElegooCanvas {

namespace {

using Clock = std::chrono::steady_clock;
using nlohmann::json;

constexpr int METHOD_ATTRIBUTES    = 1001;
constexpr int METHOD_START_PRINT   = 1020;
constexpr int METHOD_CANVAS_STATUS = 2005;

int random_int(int lo, int hi)
{
    static thread_local std::mt19937 gen{std::random_device{}()};
    return std::uniform_int_distribution<int>(lo, hi)(gen);
}

Session::Result fail(std::string code, std::string message)
{
    Session::Result r;
    r.ok         = false;
    r.error_code = std::move(code);
    r.message    = std::move(message);
    return r;
}

std::chrono::milliseconds remaining(Clock::time_point deadline)
{
    const auto now = Clock::now();
    return now >= deadline ? std::chrono::milliseconds(0) : std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
}

// Next message on `topic` whose payload is a JSON object accepted by `match`; nullopt when the deadline passes.
template<class Match>
std::optional<json> wait_for(ElegooMqtt::Client &client, const std::string &topic, Clock::time_point deadline, Match match)
{
    for (;;) {
        auto msg = client.wait_message(remaining(deadline));
        if (!msg)
            return std::nullopt;
        if (msg->first != topic)
            continue; // api_status pushes and anything else
        json j = json::parse(msg->second, nullptr, false);
        if (j.is_discarded() || !j.is_object() || !match(j))
            continue;
        return j;
    }
}

} // namespace

Session::Session(Endpoint endpoint, TransportFactory make_transport) : Session(std::move(endpoint), std::move(make_transport), Options{}) {}

Session::Session(Endpoint endpoint, TransportFactory make_transport, Options options)
    : m_endpoint(std::move(endpoint)), m_make_transport(std::move(make_transport)), m_options(std::move(options))
{
    m_client_id       = m_options.client_id.empty() ? "1_PC_" + std::to_string(random_int(1000, 9999)) : m_options.client_id;
    m_next_request_id = m_options.first_request_id != 0 ? m_options.first_request_id : random_int(1, 1000000);
}

Session::Result Session::get_attributes() { return call(METHOD_ATTRIBUTES, json::object()); }

Session::Result Session::get_canvas_status() { return call(METHOD_CANVAS_STATUS, json::object()); }

Session::Result Session::start_print(const std::string &filename, const nlohmann::json &config)
{
    return call(METHOD_START_PRINT, json{{"storage_media", "local"}, {"filename", filename}, {"config", config}});
}

Session::Result Session::get_canvas_info(Info &out)
{
    // One connection and one registration for both requests (spec 5.2): halves the round trips and the time a
    // switched-off printer costs.
    return run([&](const Request &request) -> Result {
        Result attrs = request(METHOD_ATTRIBUTES, json::object());
        if (!attrs.ok)
            return attrs;
        const std::string firmware = firmware_from_attributes(attrs.result);
        if (firmware_below(firmware, 1, 1))
            return fail("firmware_too_old",
                        "The printer firmware (" + firmware + ") is too old for CANVAS. Update it to 1.1 or later.");

        Result status = request(METHOD_CANVAS_STATUS, json::object());
        if (!status.ok)
            return status;
        const auto it = status.result.find("canvas_info");
        if (it == status.result.end() || !it->is_object())
            return fail("printer_error", "The printer's CANVAS status has no canvas_info.");
        try {
            out          = parse_canvas_info(*it);
            out.firmware = firmware;
        } catch (const std::exception &e) {
            return fail("printer_error", std::string("The printer's CANVAS status could not be read (") + e.what() + ").");
        }
        return status;
    });
}

Session::Result Session::call(int method, const nlohmann::json &params)
{
    return run([&](const Request &request) { return request(method, params); });
}

Session::Result Session::run(const std::function<Result(const Request &)> &body)
{
    if (m_endpoint.serial.empty()) {
        if (!m_endpoint.serial_error.empty())
            return fail("unreachable", "Could not reach the printer (" + m_endpoint.serial_error + ").");
        return fail("unreachable", "The printer's serial number is unknown. Run Test in the printer's connection settings, then try again.");
    }

    const std::string base       = "elegoo/" + m_endpoint.serial + "/";
    const std::string t_request  = base + m_client_id + "/api_request";
    const std::string t_response = base + m_client_id + "/api_response";
    const std::string t_status   = base + "api_status";
    const std::string t_register = base + m_client_id + "_req/register_response";

    std::unique_ptr<ElegooMqtt::Transport> transport;
    try {
        if (m_make_transport)
            transport = m_make_transport();
    } catch (const std::exception &e) {
        return fail("unreachable", std::string("Could not reach the printer over MQTT (") + e.what() + ").");
    }

    ElegooMqtt::Client client(std::move(transport));
    auto finish = [&client](Result r) {
        client.disconnect();
        return r;
    };

    try {
        client.connect(m_endpoint.host, m_endpoint.port, m_client_id, "elegoo", m_endpoint.access_code, m_options.connect_timeout);
    } catch (const std::exception &e) {
        return finish(fail("unreachable", std::string("Could not reach the printer over MQTT (") + e.what() + ")."));
    }

    try {
        client.subscribe({t_response, t_status, t_register}, m_options.connect_timeout);

        // Registration.
        const auto reg_deadline = Clock::now() + m_options.register_timeout;
        client.publish(base + "api_register", json{{"client_id", m_client_id}, {"request_id", m_client_id + "_req"}}.dump(),
                       m_options.register_timeout);
        auto reg = wait_for(client, t_register, reg_deadline, [this](const json &j) {
            const auto it = j.find("client_id");
            return it == j.end() || (it->is_string() && it->get<std::string>() == m_client_id);
        });
        if (!reg)
            return finish(fail("timeout", "The printer did not answer the registration request."));
        std::string reg_error;
        if (const auto e = reg->find("error"); e != reg->end()) {
            if (!e->is_string())
                return finish(fail("registration_refused", "The printer sent a registration reply that could not be read."));
            reg_error = e->get<std::string>();
        }
        if (reg_error == "too many clients")
            return finish(fail("too_many_clients", "The printer refused the connection: too many clients are connected. "
                                                   "Close the Device tab or the other Elegoo slicer, then try again."));
        if (reg_error != "ok")
            return finish(fail("registration_refused", "The printer refused the registration (" +
                                                           (reg_error.empty() ? std::string("no reason given") : reg_error) + ")."));

        // Requests, all on this connection. A throw (MQTT failure) leaves through the catch below.
        const Request request = [&](int method, const json &params) -> Result {
            const int  request_id   = m_next_request_id++;
            const auto req_deadline = Clock::now() + m_options.request_timeout;
            client.publish(t_request, json{{"id", request_id}, {"method", method}, {"params", params}}.dump(), m_options.request_timeout);
            auto reply = wait_for(client, t_response, req_deadline, [request_id](const json &j) {
                const auto it = j.find("id");
                return it != j.end() && it->is_number_integer() && it->get<long long>() == request_id;
            });
            if (!reply)
                return fail("timeout", "The printer did not answer request " + std::to_string(method) + " in time.");

            const auto res = reply->find("result");
            if (res == reply->end() || !res->is_object())
                return fail("printer_error", "The printer sent a reply without a result.");
            const auto code = res->find("error_code");
            if (code != res->end()) {
                if (!code->is_number_integer())
                    return fail("printer_error", "The printer sent an unreadable error code.");
                const long long c = code->get<long long>();
                if (c != 0) {
                    std::string msg = "The printer returned error code " + std::to_string(c);
                    const auto  m   = res->find("message");
                    if (m != res->end() && m->is_string() && !m->get<std::string>().empty())
                        msg += ": " + m->get<std::string>();
                    return fail("printer_error", msg + ".");
                }
            }

            Result ok;
            ok.ok     = true;
            ok.result = *res;
            return ok;
        };
        return finish(body(request));
    } catch (const std::exception &e) {
        const std::string what = e.what();
        const bool        is_timeout = what.find("timeout") != std::string::npos;
        return finish(fail(is_timeout ? "timeout" : "unreachable", "The MQTT connection to the printer failed (" + what + ")."));
    }
}

std::string firmware_from_attributes(const nlohmann::json &result)
{
    if (!result.is_object())
        return {};
    const auto sv = result.find("software_version");
    if (sv == result.end() || !sv->is_object())
        return {};
    const auto ota = sv->find("ota_version");
    if (ota == sv->end() || !ota->is_string())
        return {};
    return ota->get<std::string>();
}

bool firmware_below(const std::string &version, int major, int minor)
{
    size_t i = 0;
    while (i < version.size() && !std::isdigit(static_cast<unsigned char>(version[i])))
        ++i;
    auto read_number = [&](long &out) {
        const size_t start = i;
        out                = 0;
        while (i < version.size() && std::isdigit(static_cast<unsigned char>(version[i])) && i - start < 9)
            out = out * 10 + (version[i++] - '0');
        return i > start;
    };
    long maj = 0, min = 0;
    if (!read_number(maj) || i >= version.size() || version[i] != '.')
        return false;
    ++i;
    if (!read_number(min))
        return false;
    return maj < major || (maj == major && min < minor);
}

nlohmann::json start_print_config(bool time_lapse, bool bed_leveling, bool pc_plate, const std::string &slot_map_text)
{
    json config = {{"delay_video", time_lapse}, {"printer_check", bed_leveling}, {"print_layout", pc_plate ? "B" : "A"}};
    if (!slot_map_text.empty()) {
        json slot_map = json::parse(slot_map_text, nullptr, false);
        if (slot_map.is_discarded() || !slot_map.is_array())
            throw std::invalid_argument("slot_map is not a JSON array");
        if (!slot_map.empty()) {
            config["slot_map"]       = std::move(slot_map);
            config["bedlevel_force"] = false;
        }
    }
    return config;
}

}} // namespace Slic3r::ElegooCanvas
