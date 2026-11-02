#include <catch2/catch_all.hpp>

#include "slic3r/Utils/ElegooMqtt.hpp"

#include <stdexcept>
#include <string>
#include <vector>

using namespace Slic3r::ElegooMqtt;

TEST_CASE("encode_connect builds an MQTT 3.1.1 CONNECT", "[elegoo_mqtt]")
{
    auto b = encode_connect("1_PC_1234", "elegoo", "123456", 60);
    std::vector<uint8_t> expect = {0x10, 0x00, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04, 0xC2, 0x00, 0x3C};
    auto add = [&](const std::string &s) {
        expect.push_back(uint8_t(s.size() >> 8));
        expect.push_back(uint8_t(s.size() & 0xFF));
        expect.insert(expect.end(), s.begin(), s.end());
    };
    add("1_PC_1234");
    add("elegoo");
    add("123456");
    expect[1] = uint8_t(expect.size() - 2);
    CHECK(b == expect);
}

TEST_CASE("encode_subscribe uses QoS 1", "[elegoo_mqtt]")
{
    auto b = encode_subscribe(1, {"a/b"});
    std::vector<uint8_t> expect = {0x82, 0x08, 0x00, 0x01, 0x00, 0x03, 'a', '/', 'b', 0x01};
    expect[1] = uint8_t(expect.size() - 2);
    CHECK(b == expect);
}

TEST_CASE("encode_publish uses QoS 1 without retain", "[elegoo_mqtt]")
{
    auto b = encode_publish(2, "t", "{}");
    std::vector<uint8_t> expect = {0x32, 0x07, 0x00, 0x01, 't', 0x00, 0x02, '{', '}'};
    CHECK(b == expect);
}

TEST_CASE("fixed packets", "[elegoo_mqtt]")
{
    CHECK(encode_pingreq() == std::vector<uint8_t>{0xC0, 0x00});
    CHECK(encode_disconnect() == std::vector<uint8_t>{0xE0, 0x00});
    CHECK(encode_puback(0x0102) == std::vector<uint8_t>{0x40, 0x02, 0x01, 0x02});
}

TEST_CASE("decode CONNACK", "[elegoo_mqtt]")
{
    std::vector<uint8_t> buf = {0x20, 0x02, 0x00, 0x00};
    size_t consumed = 99;
    auto p = decode(buf, consumed);
    REQUIRE(p);
    CHECK(p->type == 2);
    CHECK(p->return_code == 0);
    CHECK(consumed == 4);
}

TEST_CASE("decode CONNACK refused keeps the return code", "[elegoo_mqtt]")
{
    std::vector<uint8_t> buf = {0x20, 0x02, 0x00, 0x05};
    size_t consumed = 0;
    auto p = decode(buf, consumed);
    REQUIRE(p);
    CHECK(p->return_code == 5);
}

TEST_CASE("decode PUBLISH QoS 1", "[elegoo_mqtt]")
{
    std::vector<uint8_t> buf = {0x32, 0x09, 0x00, 0x03, 'x', '/', 'y', 0x00, 0x05, 'h', 'i'};
    size_t consumed = 0;
    auto p = decode(buf, consumed);
    REQUIRE(p);
    CHECK(p->type == 3);
    CHECK(p->topic == "x/y");
    CHECK(p->packet_id == 5);
    CHECK(p->payload == "hi");
    CHECK(consumed == buf.size());
}

TEST_CASE("decode PUBLISH QoS 0 has no packet id", "[elegoo_mqtt]")
{
    std::vector<uint8_t> buf = {0x30, 0x07, 0x00, 0x03, 'x', '/', 'y', 'h', 'i'};
    size_t consumed = 0;
    auto p = decode(buf, consumed);
    REQUIRE(p);
    CHECK(p->topic == "x/y");
    CHECK(p->packet_id == 0);
    CHECK(p->payload == "hi");
}

TEST_CASE("decode PUBACK, SUBACK, PINGRESP", "[elegoo_mqtt]")
{
    size_t consumed = 0;
    auto pa = decode({0x40, 0x02, 0x00, 0x07}, consumed);
    REQUIRE(pa);
    CHECK(pa->type == 4);
    CHECK(pa->packet_id == 7);

    auto sa = decode({0x90, 0x03, 0x00, 0x01, 0x01}, consumed);
    REQUIRE(sa);
    CHECK(sa->type == 9);
    CHECK(sa->packet_id == 1);
    CHECK(sa->return_code == 1);
    CHECK(consumed == 5);

    auto pr = decode({0xD0, 0x00}, consumed);
    REQUIRE(pr);
    CHECK(pr->type == 13);
    CHECK(consumed == 2);
}

TEST_CASE("decode returns nullopt for partial packets", "[elegoo_mqtt]")
{
    size_t consumed = 99;
    CHECK_FALSE(decode({0x90, 0x03, 0x00}, consumed));
    CHECK(consumed == 0);

    consumed = 99;
    CHECK_FALSE(decode({0x32, 0x09, 0x00}, consumed));
    CHECK(consumed == 0);

    consumed = 99;
    CHECK_FALSE(decode({}, consumed));
    CHECK(consumed == 0);

    // continuation bit set but no next length byte
    consumed = 99;
    CHECK_FALSE(decode({0x30, 0x80}, consumed));
    CHECK(consumed == 0);
}

TEST_CASE("decode only consumes the first packet", "[elegoo_mqtt]")
{
    std::vector<uint8_t> buf = {0xD0, 0x00, 0x20, 0x02, 0x00, 0x00};
    size_t consumed = 0;
    auto p = decode(buf, consumed);
    REQUIRE(p);
    CHECK(consumed == 2);
}

TEST_CASE("remaining length uses two bytes from 128", "[elegoo_mqtt]")
{
    // 300 byte payload + 2 (topic len) + 1 (topic) + 2 (packet id) = 305 -> 0xB1 0x02
    std::string payload(300, 'p');
    auto b = encode_publish(9, "t", payload);
    REQUIRE(b.size() == 1 + 2 + 305);
    CHECK(b[1] == 0xB1);
    CHECK(b[2] == 0x02);

    size_t consumed = 0;
    auto p = decode(b, consumed);
    REQUIRE(p);
    CHECK(p->payload == payload);
    CHECK(p->packet_id == 9);
    CHECK(consumed == b.size());

    // 321 remaining bytes -> 0xC1 0x02
    std::string payload2(316, 'q');
    auto b2 = encode_publish(1, "t", payload2);
    CHECK(b2[1] == 0xC1);
    CHECK(b2[2] == 0x02);
}

TEST_CASE("decode throws on malformed packets", "[elegoo_mqtt]")
{
    size_t consumed = 99;
    // remaining length over 4 bytes
    CHECK_THROWS_AS(decode({0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0x01}, consumed), std::invalid_argument);
    // CONNACK body too short
    CHECK_THROWS_AS(decode({0x20, 0x01, 0x00}, consumed), std::invalid_argument);
    // SUBACK body too short
    CHECK_THROWS_AS(decode({0x90, 0x02, 0x00, 0x01}, consumed), std::invalid_argument);
    // PUBACK body too short
    CHECK_THROWS_AS(decode({0x40, 0x01, 0x00}, consumed), std::invalid_argument);
    // PUBLISH topic length overruns body
    CHECK_THROWS_AS(decode({0x30, 0x04, 0x00, 0x09, 'a', 'b'}, consumed), std::invalid_argument);
    // PUBLISH body under 2 bytes
    CHECK_THROWS_AS(decode({0x30, 0x01, 0x00}, consumed), std::invalid_argument);
    // PUBLISH QoS 1 with packet id missing
    CHECK_THROWS_AS(decode({0x32, 0x04, 0x00, 0x02, 'a', 'b'}, consumed), std::invalid_argument);

    try {
        decode({0x20, 0x01, 0x00}, consumed);
        FAIL("expected throw");
    } catch (const std::invalid_argument &e) {
        CHECK(std::string(e.what()).rfind("mqtt: malformed packet", 0) == 0);
    }
}
