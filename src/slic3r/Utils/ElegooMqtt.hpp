#ifndef slic3r_ElegooMqtt_hpp_
#define slic3r_ElegooMqtt_hpp_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <utility>
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

// Byte transport under the client. The real one is a TCP socket; tests use an in-memory fake.
// All failures are reported as std::runtime_error with a message starting "mqtt:".
class Transport
{
public:
    virtual ~Transport() = default;
    virtual void   send(const std::vector<uint8_t> &data) = 0;
    // Appends whatever arrives within `timeout` to `into` and returns the byte count; 0 means nothing arrived.
    // Throws if the peer closed the connection.
    virtual size_t recv_some(std::vector<uint8_t> &into, std::chrono::milliseconds timeout) = 0;
    virtual void   close() = 0;
};

// TCP transport (Boost.Asio, synchronous with deadlines). Throws "mqtt: ..." on resolve/connect failure or timeout.
std::unique_ptr<Transport> make_tcp_transport(const std::string &host, uint16_t port, std::chrono::milliseconds timeout);

// Minimal MQTT 3.1.1 client for short-lived sessions (QoS 1, no timer-driven keepalive).
// Every error throws std::runtime_error whose message starts "mqtt:" and closes the transport.
class Client
{
public:
    // With no transport, connect() opens a TCP one to host:port.
    explicit Client(std::unique_ptr<Transport> transport = nullptr);
    ~Client();
    Client(const Client &)            = delete;
    Client &operator=(const Client &) = delete;

    void connect(const std::string &host, uint16_t port, const std::string &client_id, const std::string &user,
                 const std::string &pass, std::chrono::milliseconds timeout);
    void subscribe(const std::vector<std::string> &topics, std::chrono::milliseconds timeout);
    void publish(const std::string &topic, const std::string &payload, std::chrono::milliseconds timeout);
    // Next PUBLISH as (topic, payload), PUBACKed first; nullopt on timeout. PINGRESP is ignored.
    std::optional<std::pair<std::string, std::string>> wait_message(std::chrono::milliseconds timeout);
    // Sends DISCONNECT and closes; safe to call twice.
    void disconnect();

private:
    using Clock = std::chrono::steady_clock;
    std::optional<Packet> read_packet(Clock::time_point deadline);
    Packet                expect(uint8_t type, int packet_id, Clock::time_point deadline, const char *what);
    void                  send_or_close(const std::vector<uint8_t> &data);
    [[noreturn]] void     fail(const std::string &msg);
    void                  close_transport();
    uint16_t              next_id();

    std::unique_ptr<Transport>                         m_transport;
    std::vector<uint8_t>                               m_buf;
    std::deque<std::pair<std::string, std::string>>    m_pending;
    uint16_t                                           m_last_id = 0;
};

} // namespace Slic3r::ElegooMqtt

#endif
