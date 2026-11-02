#include "ElegooMqtt.hpp"

#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <stdexcept>

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

[[noreturn]] static void malformed(const char *what)
{
    throw std::invalid_argument(std::string("mqtt: malformed packet: ") + what);
}

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
            malformed("remaining length over 4 bytes");
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
        if (remaining < 2)
            malformed("CONNACK too short");
        p.return_code = body[1];
        break;
    case 3: { // PUBLISH
        if (remaining < 2)
            malformed("PUBLISH too short");
        const size_t tlen = u16_at(0);
        size_t off = 2;
        if (off + tlen > remaining)
            malformed("PUBLISH topic overruns body");
        p.topic.assign(reinterpret_cast<const char *>(body + off), tlen);
        off += tlen;
        if (((flags >> 1) & 0x03) > 0) {
            if (off + 2 > remaining)
                malformed("PUBLISH packet id missing");
            p.packet_id = u16_at(off);
            off += 2;
        }
        p.payload.assign(reinterpret_cast<const char *>(body + off), remaining - off);
        break;
    }
    case 4: // PUBACK
    case 9: // SUBACK: packet id, then one return code per topic (first is kept)
        if (remaining < (p.type == 9 ? 3u : 2u))
            malformed(p.type == 9 ? "SUBACK too short" : "PUBACK too short");
        p.packet_id = u16_at(0);
        if (p.type == 9)
            p.return_code = body[2];
        break;
    default:
        break;
    }

    consumed = pos + remaining;
    return p;
}

namespace {

namespace asio = boost::asio;
using asio::ip::tcp;

class TcpTransport : public Transport
{
public:
    TcpTransport(const std::string &host, uint16_t port, std::chrono::milliseconds timeout)
        : m_timeout(timeout), m_socket(m_io)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        auto remaining = [&]() {
            return std::max(std::chrono::milliseconds(0),
                            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()));
        };

        std::vector<tcp::endpoint> endpoints;
        boost::system::error_code  ec;
        const auto                 addr = asio::ip::make_address(host, ec);
        if (!ec) {
            endpoints.push_back(tcp::endpoint(addr, port)); // an IP literal needs no resolver
        } else {
            endpoints = resolve_with_deadline(host, port, remaining());
        }

        bool done = false;
        ec        = {};
        asio::async_connect(m_socket, endpoints, [&](const boost::system::error_code &e, const tcp::endpoint &) {
            ec   = e;
            done = true;
        });
        run_until(remaining());
        if (!done) {
            boost::system::error_code ignored;
            m_socket.close(ignored);
            m_io.restart();
            m_io.run();
            throw std::runtime_error("mqtt: timeout connecting to " + host);
        }
        if (ec)
            throw std::runtime_error("mqtt: cannot connect to " + host + ": " + ec.message());
    }

    void send(const std::vector<uint8_t> &data) override
    {
        boost::system::error_code ec;
        bool                      done = false;
        asio::async_write(m_socket, asio::buffer(data), [&](const boost::system::error_code &e, size_t) {
            ec   = e;
            done = true;
        });
        run_until(m_timeout);
        if (!done) {
            m_socket.close(ec);
            m_io.restart();
            m_io.run();
            throw std::runtime_error("mqtt: timeout sending");
        }
        if (ec)
            throw std::runtime_error("mqtt: send failed: " + ec.message());
    }

    size_t recv_some(std::vector<uint8_t> &into, std::chrono::milliseconds timeout) override
    {
        uint8_t                   chunk[4096];
        boost::system::error_code ec;
        size_t                    n    = 0;
        bool                      done = false;
        m_socket.async_read_some(asio::buffer(chunk), [&](const boost::system::error_code &e, size_t bytes) {
            ec   = e;
            n    = bytes;
            done = true;
        });
        run_until(timeout);
        if (!done) {
            // Nothing arrived: cancel the read (its handler runs with operation_aborted and n == 0).
            boost::system::error_code ignored;
            m_socket.cancel(ignored);
            m_io.restart();
            m_io.run();
            if (n == 0)
                return 0;
        } else if (ec) {
            throw std::runtime_error(ec == asio::error::eof ? std::string("mqtt: connection closed by peer") :
                                                              "mqtt: receive failed: " + ec.message());
        }
        into.insert(into.end(), chunk, chunk + n);
        return n;
    }

    void close() override
    {
        boost::system::error_code ec;
        m_socket.shutdown(tcp::socket::shutdown_both, ec);
        m_socket.close(ec);
    }

private:
    // getaddrinfo cannot be interrupted, so resolve on a detached thread that owns its state; on timeout the
    // thread is left to finish by itself and nothing here waits for it.
    static std::vector<tcp::endpoint> resolve_with_deadline(const std::string &host, uint16_t port, std::chrono::milliseconds timeout)
    {
        struct Shared
        {
            std::mutex                 m;
            std::condition_variable    cv;
            bool                       done = false;
            std::string                error;
            std::vector<tcp::endpoint> endpoints;
        };
        auto shared = std::make_shared<Shared>();
        std::thread([shared, host, port]() {
            std::string                error;
            std::vector<tcp::endpoint> eps;
            try {
                asio::io_context          io;
                tcp::resolver             resolver(io);
                boost::system::error_code ec;
                auto                      results = resolver.resolve(host, std::to_string(port), ec);
                if (ec)
                    error = ec.message();
                else
                    for (const auto &entry : results)
                        eps.push_back(entry.endpoint());
            } catch (const std::exception &e) {
                error = e.what();
            }
            std::lock_guard<std::mutex> lock(shared->m);
            shared->error     = std::move(error);
            shared->endpoints = std::move(eps);
            shared->done      = true;
            shared->cv.notify_all();
        }).detach();

        std::unique_lock<std::mutex> lock(shared->m);
        if (!shared->cv.wait_for(lock, timeout, [&] { return shared->done; }))
            throw std::runtime_error("mqtt: timeout resolving " + host);
        if (!shared->error.empty() || shared->endpoints.empty())
            throw std::runtime_error("mqtt: cannot resolve " + host + (shared->error.empty() ? "" : ": " + shared->error));
        return std::move(shared->endpoints);
    }

    void run_until(std::chrono::milliseconds timeout)
    {
        m_io.restart();
        m_io.run_for(timeout);
    }

    std::chrono::milliseconds m_timeout;
    asio::io_context          m_io;
    tcp::socket               m_socket;
};

} // namespace

std::unique_ptr<Transport> make_tcp_transport(const std::string &host, uint16_t port, std::chrono::milliseconds timeout)
{
    return std::make_unique<TcpTransport>(host, port, timeout);
}

Client::Client(std::unique_ptr<Transport> transport) : m_transport(std::move(transport)) {}

Client::~Client()
{
    close_transport();
}

void Client::close_transport()
{
    if (m_transport) {
        try {
            m_transport->close();
        } catch (...) {
        }
    }
}

void Client::fail(const std::string &msg)
{
    close_transport();
    throw std::runtime_error(msg);
}

uint16_t Client::next_id()
{
    m_last_id = uint16_t(m_last_id == 0xFFFF ? 1 : m_last_id + 1);
    return m_last_id;
}

void Client::send_or_close(const std::vector<uint8_t> &data)
{
    try {
        m_transport->send(data);
    } catch (...) {
        close_transport();
        throw;
    }
}

std::optional<Packet> Client::read_packet(Clock::time_point deadline)
{
    for (;;) {
        size_t consumed = 0;
        try {
            auto pkt = decode(m_buf, consumed);
            if (pkt) {
                m_buf.erase(m_buf.begin(), m_buf.begin() + std::ptrdiff_t(consumed));
                return pkt;
            }
        } catch (const std::invalid_argument &e) {
            fail(e.what()); // already "mqtt: malformed packet ..."
        }
        // Always try at least one read, even with no time left, so data already queued is never missed.
        const auto now       = Clock::now();
        const auto remaining = now >= deadline ? std::chrono::milliseconds(0) :
                                                 std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        size_t     got       = 0;
        try {
            got = m_transport->recv_some(m_buf, remaining);
        } catch (...) {
            close_transport();
            throw;
        }
        if (got == 0 && Clock::now() >= deadline)
            return std::nullopt;
    }
}

Packet Client::expect(uint8_t type, int packet_id, Clock::time_point deadline, const char *what)
{
    for (;;) {
        auto pkt = read_packet(deadline);
        if (!pkt)
            fail(std::string("mqtt: timeout waiting for ") + what);
        if (pkt->type == 3) { // a message slipped in before the ack: answer it and keep it for wait_message
            if (pkt->packet_id != 0)
                send_or_close(encode_puback(pkt->packet_id));
            m_pending.emplace_back(std::move(pkt->topic), std::move(pkt->payload));
            continue;
        }
        if (pkt->type == type && (packet_id < 0 || pkt->packet_id == packet_id))
            return *pkt;
        // PINGRESP and anything unrelated is ignored.
    }
}

void Client::connect(const std::string &host, uint16_t port, const std::string &client_id, const std::string &user,
                     const std::string &pass, std::chrono::milliseconds timeout)
{
    const auto deadline = Clock::now() + timeout;
    m_buf.clear();
    m_pending.clear();
    if (!m_transport)
        m_transport = make_tcp_transport(host, port, timeout); // throws "mqtt: ..." itself
    send_or_close(encode_connect(client_id, user, pass, 60));
    const Packet ack = expect(2, -1, deadline, "CONNACK");
    if (ack.return_code != 0)
        fail("mqtt: connection refused (" + std::to_string(int(ack.return_code)) + ")");
}

void Client::subscribe(const std::vector<std::string> &topics, std::chrono::milliseconds timeout)
{
    const auto     deadline = Clock::now() + timeout;
    const uint16_t id       = next_id();
    send_or_close(encode_subscribe(id, topics));
    const Packet ack = expect(9, id, deadline, "SUBACK");
    if (ack.return_code == 0x80)
        fail("mqtt: subscribe refused for " + (topics.empty() ? std::string() : topics.front()));
}

void Client::publish(const std::string &topic, const std::string &payload, std::chrono::milliseconds timeout)
{
    const auto     deadline = Clock::now() + timeout;
    const uint16_t id       = next_id();
    send_or_close(encode_publish(id, topic, payload));
    expect(4, id, deadline, "PUBACK");
}

std::optional<std::pair<std::string, std::string>> Client::wait_message(std::chrono::milliseconds timeout)
{
    if (!m_pending.empty()) {
        auto m = std::move(m_pending.front());
        m_pending.pop_front();
        return m;
    }
    const auto deadline = Clock::now() + timeout;
    for (;;) {
        auto pkt = read_packet(deadline);
        if (!pkt)
            return std::nullopt;
        if (pkt->type != 3)
            continue; // PINGRESP and other packets are ignored
        if (pkt->packet_id != 0)
            send_or_close(encode_puback(pkt->packet_id));
        return std::make_pair(std::move(pkt->topic), std::move(pkt->payload));
    }
}

void Client::disconnect()
{
    if (!m_transport)
        return;
    try {
        m_transport->send(encode_disconnect());
    } catch (...) {
    }
    close_transport();
    m_transport.reset();
}

} // namespace Slic3r::ElegooMqtt
