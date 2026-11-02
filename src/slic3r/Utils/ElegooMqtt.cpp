#include "ElegooMqtt.hpp"

namespace Slic3r::ElegooMqtt {

namespace {

void put_u16(std::vector<uint8_t> &out, uint16_t v)
{
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v & 0xFF));
}

void put_string(std::vector<uint8_t> &out, const std::string &s)
{
    put_u16(out, uint16_t(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

std::vector<uint8_t> frame(uint8_t first_byte, const std::vector<uint8_t> &body)
{
    std::vector<uint8_t> out;
    out.push_back(first_byte);
    size_t len = body.size();
    do {
        uint8_t digit = uint8_t(len % 128);
        len /= 128;
        if (len > 0)
            digit |= 0x80;
        out.push_back(digit);
    } while (len > 0);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

} // namespace

std::vector<uint8_t> encode_connect(const std::string &client_id, const std::string &user, const std::string &pass, uint16_t keepalive)
{
    std::vector<uint8_t> body;
    put_string(body, "MQTT");
    body.push_back(0x04); // protocol level 3.1.1
    body.push_back(0xC2); // username + password + clean session
    put_u16(body, keepalive);
    put_string(body, client_id);
    put_string(body, user);
    put_string(body, pass);
    return frame(0x10, body);
}

std::vector<uint8_t> encode_subscribe(uint16_t packet_id, const std::vector<std::string> &topics)
{
    std::vector<uint8_t> body;
    put_u16(body, packet_id);
    for (const std::string &t : topics) {
        put_string(body, t);
        body.push_back(0x01); // QoS 1
    }
    return frame(0x82, body);
}

std::vector<uint8_t> encode_publish(uint16_t packet_id, const std::string &topic, const std::string &payload)
{
    std::vector<uint8_t> body;
    put_string(body, topic);
    put_u16(body, packet_id);
    body.insert(body.end(), payload.begin(), payload.end());
    return frame(0x32, body); // PUBLISH, QoS 1, no retain, no dup
}

std::vector<uint8_t> encode_puback(uint16_t packet_id)
{
    std::vector<uint8_t> body;
    put_u16(body, packet_id);
    return frame(0x40, body);
}

std::vector<uint8_t> encode_pingreq() { return {0xC0, 0x00}; }

std::vector<uint8_t> encode_disconnect() { return {0xE0, 0x00}; }

std::optional<Packet> decode(const std::vector<uint8_t> &buf, size_t &consumed)
{
    consumed = 0;
    if (buf.empty())
        return std::nullopt;

    // Remaining length: 1 to 4 bytes, 7 bits each.
    size_t remaining = 0;
    size_t multiplier = 1;
    size_t pos = 1;
    for (int i = 0;; ++i) {
        if (i == 4)
            return std::nullopt; // malformed
        if (pos >= buf.size())
            return std::nullopt;
        uint8_t digit = buf[pos++];
        remaining += size_t(digit & 0x7F) * multiplier;
        multiplier *= 128;
        if ((digit & 0x80) == 0)
            break;
    }
    if (buf.size() - pos < remaining)
        return std::nullopt;

    const uint8_t *body = buf.data() + pos;

    Packet p;
    p.type = uint8_t(buf[0] >> 4);
    const uint8_t flags = buf[0] & 0x0F;

    auto u16_at = [&](size_t off) { return uint16_t((body[off] << 8) | body[off + 1]); };

    switch (p.type) {
    case 2: // CONNACK: session-present, return code
        if (remaining >= 2)
            p.return_code = body[1];
        break;
    case 3: { // PUBLISH
        if (remaining < 2)
            break;
        const size_t tlen = u16_at(0);
        size_t off = 2;
        if (off + tlen > remaining)
            break;
        p.topic.assign(reinterpret_cast<const char *>(body + off), tlen);
        off += tlen;
        if (((flags >> 1) & 0x03) > 0) {
            if (off + 2 > remaining)
                break;
            p.packet_id = u16_at(off);
            off += 2;
        }
        p.payload.assign(reinterpret_cast<const char *>(body + off), remaining - off);
        break;
    }
    case 4: // PUBACK
    case 9: // SUBACK: packet id, then one return code per topic (first is kept)
        if (remaining >= 2)
            p.packet_id = u16_at(0);
        if (p.type == 9 && remaining >= 3)
            p.return_code = body[2];
        break;
    default:
        break;
    }

    consumed = pos + remaining;
    return p;
}

} // namespace Slic3r::ElegooMqtt
