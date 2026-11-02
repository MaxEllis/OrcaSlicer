#ifndef slic3r_ElegooMqtt_hpp_
#define slic3r_ElegooMqtt_hpp_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Pure MQTT 3.1.1 packet encoder/decoder (QoS 1 only). No I/O, no logging.
namespace Slic3r::ElegooMqtt {

struct Packet
{
    uint8_t     type        = 0; // MQTT control packet type: 2 CONNACK, 3 PUBLISH, 4 PUBACK, 9 SUBACK, 13 PINGRESP
    uint16_t    packet_id   = 0; // 0 when the packet carries none (QoS 0 PUBLISH)
    std::string topic;           // PUBLISH only
    std::string payload;         // PUBLISH only
    uint8_t     return_code = 0; // CONNACK return code, or the first SUBACK return code
};

std::vector<uint8_t> encode_connect(const std::string &client_id, const std::string &user, const std::string &pass, uint16_t keepalive);
std::vector<uint8_t> encode_subscribe(uint16_t packet_id, const std::vector<std::string> &topics);
std::vector<uint8_t> encode_publish(uint16_t packet_id, const std::string &topic, const std::string &payload);
std::vector<uint8_t> encode_puback(uint16_t packet_id);
std::vector<uint8_t> encode_pingreq();
std::vector<uint8_t> encode_disconnect();

// Decodes the first packet in buf.
// - Returns nullopt (consumed = 0) ONLY when buf holds less than one whole packet (need more bytes).
// - Throws std::invalid_argument ("mqtt: malformed packet: ...") for a corrupt stream: remaining
//   length over 4 bytes, a CONNACK/SUBACK/PUBACK body shorter than its fixed part, or a PUBLISH whose
//   topic or packet id overruns its body.
std::optional<Packet> decode(const std::vector<uint8_t> &buf, size_t &consumed);

} // namespace Slic3r::ElegooMqtt

#endif
