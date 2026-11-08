#ifndef slic3r_tests_ElegooFakeTransport_hpp_
#define slic3r_tests_ElegooFakeTransport_hpp_

#include "slic3r/Utils/ElegooMqtt.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <thread>

// In-memory Transport for tests: the test feeds inbound chunks, the client's writes are logged.
// Shared state is held by pointer so the test can keep inspecting it after the Client takes ownership.
struct FakeTransportState
{
    std::deque<std::vector<uint8_t>> inbound;
    std::vector<uint8_t>             outbound;
    bool                             closed = false;

    void feed(const std::vector<uint8_t> &chunk) { inbound.push_back(chunk); }
};

class FakeTransport : public Slic3r::ElegooMqtt::Transport
{
public:
    explicit FakeTransport(FakeTransportState *s) : m_s(s) {}

    void send(const std::vector<uint8_t> &data) override
    {
        m_s->outbound.insert(m_s->outbound.end(), data.begin(), data.end());
    }

    size_t recv_some(std::vector<uint8_t> &into, std::chrono::milliseconds timeout) override
    {
        if (m_s->inbound.empty()) {
            // Simulate "nothing arrived" without sleeping the whole timeout.
            std::this_thread::sleep_for(std::min(timeout, std::chrono::milliseconds(2)));
            return 0;
        }
        auto chunk = std::move(m_s->inbound.front());
        m_s->inbound.pop_front();
        into.insert(into.end(), chunk.begin(), chunk.end());
        return chunk.size();
    }

    void close() override { m_s->closed = true; }

private:
    FakeTransportState *m_s;
};

#endif
