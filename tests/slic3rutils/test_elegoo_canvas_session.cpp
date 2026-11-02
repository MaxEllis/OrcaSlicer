#include <catch2/catch_all.hpp>

#include "slic3r/Utils/ElegooCanvasSession.hpp"
#include "elegoo_fake_transport.hpp"

#include <memory>
#include <string>
#include <vector>

using namespace Slic3r::ElegooCanvas;
using namespace Slic3r::ElegooMqtt;
using nlohmann::json;
using namespace std::chrono_literals;

namespace {

const char *CID = "1_PC_4242";

struct Rig
{
    // One state per connection the session opens (get_canvas_info opens two).
    std::vector<std::unique_ptr<FakeTransportState>> states;
    size_t                                           opened = 0;

    FakeTransportState &next_state()
    {
        states.push_back(std::make_unique<FakeTransportState>());
        return *states.back();
    }

    Session make(int first_id = 7)
    {
        Session::Options o;
        o.client_id        = CID;
        o.first_request_id = first_id;
        o.connect_timeout  = 100ms;
        o.register_timeout = 100ms;
        o.request_timeout  = 100ms;
        return Session(Endpoint{"printer", 1883, "123456", "SN1"},
                       [this]() -> std::unique_ptr<Transport> {
                           REQUIRE(opened < states.size());
                           return std::make_unique<FakeTransport>(states[opened++].get());
                       },
                       o);
    }
};

std::string register_topic() { return std::string("elegoo/SN1/") + CID + "_req/register_response"; }
std::string response_topic() { return std::string("elegoo/SN1/") + CID + "/api_response"; }

// CONNACK, SUBACK (packet 1, three topics), PUBACK for the register publish (packet 2).
void script_handshake(FakeTransportState &s)
{
    s.feed({0x20, 0x02, 0x00, 0x00});
    s.feed({0x90, 0x05, 0x00, 0x01, 0x01, 0x01, 0x01});
    s.feed({0x40, 0x02, 0x00, 0x02});
}

void script_register(FakeTransportState &s, const json &reply)
{
    s.feed(encode_publish(100, register_topic(), reply.dump()));
}

void script_registered(FakeTransportState &s)
{
    script_handshake(s);
    script_register(s, json{{"client_id", CID}, {"error", "ok"}});
    s.feed({0x40, 0x02, 0x00, 0x03}); // PUBACK for the request (packet 3)
}

json reply(int id, int method, json result) { return json{{"id", id}, {"method", method}, {"result", std::move(result)}}; }

struct Sent
{
    std::string topic;
    std::string payload;
};

// Client PUBLISH packets in the order they were sent.
std::vector<Sent> publishes(const FakeTransportState &s)
{
    std::vector<Sent> out;
    std::vector<uint8_t> buf = s.outbound;
    for (;;) {
        size_t consumed = 0;
        auto   p        = decode(buf, consumed);
        if (!p)
            break;
        if (p->type == 3)
            out.push_back({p->topic, p->payload});
        buf.erase(buf.begin(), buf.begin() + std::ptrdiff_t(consumed));
    }
    return out;
}

json canvas_info_sample()
{
    return json{{"active_canvas_id", 0},
                {"active_tray_id", 1},
                {"auto_refill", false},
                {"canvas_list",
                 json::array({json{{"canvas_id", 0},
                                   {"connected", 1},
                                   {"tray_list", json::array({json{{"tray_id", 0},
                                                                   {"brand", "ELEGOO"},
                                                                   {"filament_type", "PLA"},
                                                                   {"filament_name", "PLA"},
                                                                   {"filament_color", "#FF0000"},
                                                                   {"status", 2}}})}}})}};
}

} // namespace

TEST_CASE("Session happy path: register, then 2005 on the request topic", "[elegoo_canvas_session]")
{
    Rig  rig;
    auto &s = rig.next_state();
    script_registered(s);
    s.feed(encode_publish(101, response_topic(), reply(7, 2005, json{{"error_code", 0}, {"canvas_info", canvas_info_sample()}}).dump()));

    Session session = rig.make();
    auto    r       = session.get_canvas_status();
    REQUIRE(r.ok);
    CHECK(r.error_code.empty());
    CHECK(r.result.contains("canvas_info"));

    auto pubs = publishes(s);
    REQUIRE(pubs.size() == 2);
    CHECK(pubs[0].topic == "elegoo/SN1/api_register");
    CHECK(json::parse(pubs[0].payload) == json{{"client_id", CID}, {"request_id", std::string(CID) + "_req"}});
    CHECK(pubs[1].topic == std::string("elegoo/SN1/") + CID + "/api_request");
    CHECK(json::parse(pubs[1].payload) == json{{"id", 7}, {"method", 2005}, {"params", json::object()}});

    // Subscribed to the three topics, logged in as elegoo/<access code>, and disconnected at the end.
    std::vector<uint8_t> expect_prefix = encode_connect(CID, "elegoo", "123456", 60);
    auto sub = encode_subscribe(1, {response_topic(), "elegoo/SN1/api_status", register_topic()});
    expect_prefix.insert(expect_prefix.end(), sub.begin(), sub.end());
    REQUIRE(s.outbound.size() > expect_prefix.size());
    CHECK(std::vector<uint8_t>(s.outbound.begin(), s.outbound.begin() + std::ptrdiff_t(expect_prefix.size())) == expect_prefix);
    auto disc = encode_disconnect();
    CHECK(std::vector<uint8_t>(s.outbound.end() - std::ptrdiff_t(disc.size()), s.outbound.end()) == disc);
    CHECK(s.closed);
}

TEST_CASE("Session: too many clients names the Device tab", "[elegoo_canvas_session]")
{
    Rig  rig;
    auto &s = rig.next_state();
    script_handshake(s);
    script_register(s, json{{"client_id", CID}, {"error", "too many clients"}});

    auto r = rig.make().get_attributes();
    CHECK_FALSE(r.ok);
    CHECK(r.error_code == "too_many_clients");
    CHECK(r.message.find("Device tab") != std::string::npos);
    CHECK(publishes(s).size() == 1); // no request after a refused registration
    CHECK(s.closed);
}

TEST_CASE("Session: other registration errors are registration_refused", "[elegoo_canvas_session]")
{
    Rig  rig;
    auto &s = rig.next_state();
    script_handshake(s);
    script_register(s, json{{"client_id", CID}, {"error", "denied"}});

    auto r = rig.make().get_attributes();
    CHECK(r.error_code == "registration_refused");
    CHECK(r.message.find("denied") != std::string::npos);
}

TEST_CASE("Session: a register reply for another client is ignored", "[elegoo_canvas_session]")
{
    Rig  rig;
    auto &s = rig.next_state();
    script_handshake(s);
    script_register(s, json{{"client_id", "1_PC_1111"}, {"error", "too many clients"}});
    script_register(s, json{{"client_id", CID}, {"error", "ok"}});
    s.feed({0x40, 0x02, 0x00, 0x03});
    s.feed(encode_publish(101, response_topic(), reply(7, 1001, json{{"error_code", 0}}).dump()));

    CHECK(rig.make().get_attributes().ok);
}

TEST_CASE("Session: no register reply is a timeout", "[elegoo_canvas_session]")
{
    Rig  rig;
    auto &s = rig.next_state();
    script_handshake(s);

    auto r = rig.make().get_attributes();
    CHECK(r.error_code == "timeout");
}

TEST_CASE("Session skips a reply with another id, then returns ours", "[elegoo_canvas_session]")
{
    Rig  rig;
    auto &s = rig.next_state();
    script_registered(s);
    s.feed(encode_publish(101, "elegoo/SN1/api_status", json{{"method", 6000}, {"result", json::object()}}.dump()));
    s.feed(encode_publish(102, response_topic(), reply(6, 2005, json{{"error_code", 5}}).dump()));
    s.feed(encode_publish(103, response_topic(), "not json"));
    s.feed(encode_publish(104, response_topic(), reply(7, 2005, json{{"error_code", 0}, {"canvas_info", {{"marker", 1}}}}).dump()));

    auto r = rig.make().get_canvas_status();
    REQUIRE(r.ok);
    CHECK(r.result["canvas_info"]["marker"] == 1);
}

TEST_CASE("Session: no reply within the timeout", "[elegoo_canvas_session]")
{
    Rig  rig;
    auto &s = rig.next_state();
    script_registered(s);

    auto r = rig.make().get_canvas_status();
    CHECK_FALSE(r.ok);
    CHECK(r.error_code == "timeout");
    CHECK(s.closed);
}

TEST_CASE("Session: error_code in the result is printer_error with the code", "[elegoo_canvas_session]")
{
    Rig  rig;
    auto &s = rig.next_state();
    script_registered(s);
    s.feed(encode_publish(101, response_topic(), reply(7, 2005, json{{"error_code", 1}}).dump()));

    auto r = rig.make().get_canvas_status();
    CHECK_FALSE(r.ok);
    CHECK(r.error_code == "printer_error");
    CHECK(r.message.find("1") != std::string::npos);
    CHECK(r.message.find("error code 1") != std::string::npos);
}

TEST_CASE("Session: connect failure is unreachable", "[elegoo_canvas_session]")
{
    SECTION("no CONNACK")
    {
        Rig rig;
        rig.next_state();
        CHECK(rig.make().get_attributes().error_code == "unreachable");
    }
    SECTION("transport factory throws")
    {
        Session::Options o;
        o.client_id = CID;
        Session s(Endpoint{"printer", 1883, "123456", "SN1"},
                  []() -> std::unique_ptr<Transport> { throw std::runtime_error("mqtt: connect failed"); }, o);
        auto r = s.get_attributes();
        CHECK(r.error_code == "unreachable");
        CHECK(r.message.find("connect failed") != std::string::npos);
    }
    SECTION("unknown serial")
    {
        Session s(Endpoint{"printer", 1883, "123456", ""},
                  []() -> std::unique_ptr<Transport> { FAIL("must not connect"); return nullptr; });
        CHECK(s.get_attributes().error_code == "unreachable");
    }
}

TEST_CASE("Session::start_print publishes 1020 with filename and config", "[elegoo_canvas_session]")
{
    Rig  rig;
    auto &s = rig.next_state();
    script_registered(s);
    s.feed(encode_publish(101, response_topic(), reply(7, 1020, json{{"error_code", 0}}).dump()));

    json cfg = {{"delay_video", true}, {"slot_map", json::array({json{{"t", 0}, {"canvas_id", 0}, {"tray_id", 2}}})}};
    auto r   = rig.make().start_print("x.gcode", cfg);
    REQUIRE(r.ok);

    auto pubs = publishes(s);
    REQUIRE(pubs.size() == 2);
    json req = json::parse(pubs[1].payload);
    CHECK(req["id"] == 7);
    CHECK(req["method"] == 1020);
    CHECK(req["params"]["storage_media"] == "local");
    CHECK(req["params"]["filename"] == "x.gcode");
    CHECK(req["params"]["config"] == cfg);
}

TEST_CASE("Session uses a fresh request id per call and a random 1_PC_ client id", "[elegoo_canvas_session]")
{
    Session::Options o;
    Session          s(Endpoint{"h", 1883, "a", "SN"}, {}, o);
    const auto      &cid = s.client_id();
    REQUIRE(cid.size() == 9);
    CHECK(cid.rfind("1_PC_", 0) == 0);
    const int n = std::stoi(cid.substr(5));
    CHECK(n >= 1000);
    CHECK(n <= 9999);

    Rig  rig;
    auto &a = rig.next_state();
    script_registered(a);
    a.feed(encode_publish(101, response_topic(), reply(7, 1001, json{{"error_code", 0}}).dump()));
    auto &b = rig.next_state();
    script_registered(b);
    b.feed(encode_publish(101, response_topic(), reply(8, 1001, json{{"error_code", 0}}).dump()));
    Session session = rig.make(7);
    CHECK(session.get_attributes().ok);
    CHECK(session.get_attributes().ok);
    CHECK(json::parse(publishes(b)[1].payload)["id"] == 8);
}

TEST_CASE("firmware_from_attributes reads software_version.ota_version", "[elegoo_canvas_session]")
{
    CHECK(firmware_from_attributes(json{{"software_version", {{"ota_version", "1.1.0.5"}}}}) == "1.1.0.5");
    CHECK(firmware_from_attributes(json{{"firmware_version", "1.2"}}) == "");
    CHECK(firmware_from_attributes(json{{"software_version", json::object()}}) == "");
    CHECK(firmware_from_attributes(json{{"software_version", "1.2"}}) == "");
    CHECK(firmware_from_attributes(json{{"software_version", {{"ota_version", 11}}}}) == "");
    CHECK(firmware_from_attributes(json()) == "");
}

TEST_CASE("firmware_below", "[elegoo_canvas_session]")
{
    CHECK(firmware_below("1.0.9", 1, 1));
    CHECK(firmware_below("V1.0.9.12", 1, 1));
    CHECK(firmware_below("0.9", 1, 1));
    CHECK_FALSE(firmware_below("1.1.0", 1, 1));
    CHECK_FALSE(firmware_below("1.10", 1, 1));
    CHECK_FALSE(firmware_below("2.0", 1, 1));
    CHECK_FALSE(firmware_below("", 1, 1));
    CHECK_FALSE(firmware_below("unknown", 1, 1));
    CHECK_FALSE(firmware_below("1", 1, 1)); // no minor: not parsable as major.minor
}

TEST_CASE("get_canvas_info: attributes then status, firmware filled", "[elegoo_canvas_session]")
{
    Rig  rig;
    auto &a = rig.next_state();
    script_registered(a);
    a.feed(encode_publish(101, response_topic(),
                          reply(7, 1001, json{{"error_code", 0}, {"software_version", {{"ota_version", "1.1.2"}}}}).dump()));
    auto &b = rig.next_state();
    script_registered(b);
    b.feed(encode_publish(101, response_topic(), reply(8, 2005, json{{"error_code", 0}, {"canvas_info", canvas_info_sample()}}).dump()));

    Info info;
    auto r = rig.make().get_canvas_info(info);
    REQUIRE(r.ok);
    CHECK(info.firmware == "1.1.2");
    REQUIRE(info.units.size() == 1);
    CHECK(info.units[0].trays.size() == 1);
    CHECK(json::parse(publishes(a)[1].payload)["method"] == 1001);
    CHECK(json::parse(publishes(b)[1].payload)["method"] == 2005);
}

TEST_CASE("get_canvas_info: old firmware is firmware_too_old; missing version does not block", "[elegoo_canvas_session]")
{
    SECTION("1.0.9")
    {
        Rig  rig;
        auto &a = rig.next_state();
        script_registered(a);
        a.feed(encode_publish(101, response_topic(),
                              reply(7, 1001, json{{"error_code", 0}, {"software_version", {{"ota_version", "1.0.9"}}}}).dump()));
        Info info;
        auto r = rig.make().get_canvas_info(info);
        CHECK(r.error_code == "firmware_too_old");
        CHECK(r.message.find("1.0.9") != std::string::npos);
        CHECK(rig.opened == 1); // no status request
    }
    SECTION("no software_version")
    {
        Rig  rig;
        auto &a = rig.next_state();
        script_registered(a);
        a.feed(encode_publish(101, response_topic(), reply(7, 1001, json{{"error_code", 0}}).dump()));
        auto &b = rig.next_state();
        script_registered(b);
        b.feed(encode_publish(101, response_topic(), reply(8, 2005, json{{"error_code", 0}, {"canvas_info", canvas_info_sample()}}).dump()));
        Info info;
        auto r = rig.make().get_canvas_info(info);
        REQUIRE(r.ok);
        CHECK(info.firmware.empty());
    }
    SECTION("status without canvas_info")
    {
        Rig  rig;
        auto &a = rig.next_state();
        script_registered(a);
        a.feed(encode_publish(101, response_topic(), reply(7, 1001, json{{"error_code", 0}}).dump()));
        auto &b = rig.next_state();
        script_registered(b);
        b.feed(encode_publish(101, response_topic(), reply(8, 2005, json{{"error_code", 0}}).dump()));
        Info info;
        CHECK(rig.make().get_canvas_info(info).error_code == "printer_error");
    }
}

TEST_CASE("start_print_config maps the send options", "[elegoo_canvas_session]")
{
    SECTION("single filament: no slot_map, no bedlevel_force")
    {
        json c = start_print_config(true, false, false, "");
        CHECK(c == json{{"delay_video", true}, {"printer_check", false}, {"print_layout", "A"}});
        CHECK(start_print_config(false, true, true, "[]") == json{{"delay_video", false}, {"printer_check", true}, {"print_layout", "B"}});
    }
    SECTION("slot map present")
    {
        json c = start_print_config(false, false, false, R"([{"t":0,"canvas_id":0,"tray_id":3}])");
        CHECK(c["slot_map"] == json::array({json{{"t", 0}, {"canvas_id", 0}, {"tray_id", 3}}}));
        CHECK(c["bedlevel_force"] == false);
    }
    SECTION("bad slot map text throws")
    {
        CHECK_THROWS_AS(start_print_config(false, false, false, "{}"), std::invalid_argument);
        CHECK_THROWS_AS(start_print_config(false, false, false, "[oops"), std::invalid_argument);
    }
}
