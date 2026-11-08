// src/slic3r/GUI/RemoteAPI/RemoteAPIServer.cpp
#include "RemoteAPIServer.hpp"

#include "libslic3r/Thread.hpp"
#include <boost/log/trivial.hpp>
#include <deque>

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
using tcp       = net::ip::tcp;

namespace Slic3r { namespace GUI { namespace RemoteAPI {

// One WebSocket connection. All socket I/O runs on the single io thread.
class WsSession : public std::enable_shared_from_this<WsSession>
{
public:
    WsSession(tcp::socket socket, Server &server)
        : m_ws(std::move(socket)), m_server(server) {}

    void run(http::request<http::string_body> req)
    {
        m_ws.set_option(beast::websocket::stream_base::timeout::suggested(beast::role_type::server));
        // beast requires the upgrade request to stay valid until async_accept
        // completes, so keep it alive in a shared_ptr captured by the handler.
        auto req_held = std::make_shared<http::request<http::string_body>>(std::move(req));
        m_ws.async_accept(*req_held, [self = shared_from_this(), req_held](beast::error_code ec) {
            if (ec) return;
            self->m_server.ws_join(self);
            self->do_read();
        });
    }

    // Called on the io thread (via net::post in Server::broadcast).
    void send(const std::string &text)
    {
        m_queue.push_back(text);
        if (m_queue.size() == 1) do_write();
    }

private:
    void do_read()
    { // inbound messages are ignored; the read just detects disconnect
        m_ws.async_read(m_buffer, [self = shared_from_this()](beast::error_code ec, std::size_t) {
            if (ec) { self->m_server.ws_leave(self); return; }
            self->m_buffer.consume(self->m_buffer.size());
            self->do_read();
        });
    }
    void do_write()
    {
        m_ws.text(true);
        m_ws.async_write(net::buffer(m_queue.front()),
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec) { self->m_server.ws_leave(self); return; }
                self->m_queue.pop_front();
                if (!self->m_queue.empty()) self->do_write();
            });
    }

    beast::websocket::stream<beast::tcp_stream> m_ws;
    beast::flat_buffer                          m_buffer;
    std::deque<std::string>                     m_queue;
    Server                                     &m_server;
};

// One HTTP connection. Reads a request, answers it, closes (Connection: close).
class HttpSession : public std::enable_shared_from_this<HttpSession>
{
public:
    HttpSession(tcp::socket socket, Server &server)
        : m_stream(std::move(socket)), m_server(server)
    {}

    void run() { do_read(); }

private:
    void do_read()
    {
        m_stream.expires_after(std::chrono::seconds(15));
        m_parser.body_limit(4 * 1024 * 1024); // bound request body before buffering (unauthenticated clients)
        http::async_read(m_stream, m_buffer, m_parser,
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec) return; // closed / timeout / parse error / body too large
                self->handle();
            });
    }

    void handle()
    {
        auto &req = m_parser.get();
        // WebSocket upgrade path: auth via ?token= query param (browser WS
        // clients can't set request headers). Runs before token-header auth.
        if (beast::websocket::is_upgrade(req)) {
            std::string target = std::string(req.target());
            auto        tpos   = target.find("token=");
            std::string wtok   = (tpos == std::string::npos) ? std::string() : target.substr(tpos + 6);
            if (auto amp = wtok.find('&'); amp != std::string::npos) wtok.resize(amp);
            if (target.rfind("/api/v1/events", 0) == 0 && m_server.check_token(wtok)) {
                std::make_shared<WsSession>(m_stream.release_socket(), m_server)->run(req);
                return;
            }
            // otherwise fall through to the normal 401/404 JSON response
        }
        // Auth first, everything else second.
        auto        tok_sv = req["X-Api-Token"];
        // iterator-pair ctor: safe when the header is absent (data() may be
        // nullptr with size 0, which the (ptr,len) ctor treats as UB).
        std::string token(tok_sv.begin(), tok_sv.end());
        Response    r;
        if (!m_server.check_token(token)) {
            r = { 401, {{"error", "unauthorized"}} };
        } else if (!m_server.handler()) {
            r = { 503, {{"error", "no_handler"}} };
        } else {
            Request rq { std::string(req.method_string()),
                         std::string(req.target()),
                         req.body() };
            try {
                r = m_server.handler()(rq);
            } catch (const std::exception &e) {
                // The API must never take the slicer down.
                BOOST_LOG_TRIVIAL(error) << "remote-api handler exception: " << e.what();
                r = { 500, {{"error", "internal"}, {"detail", e.what()}} };
            }
        }
        // Build the response defensively: json::dump() can throw on malformed
        // data (e.g. non-UTF-8 bytes), and this runs on the io thread where an
        // uncaught exception would call std::terminate() and kill the slicer.
        std::shared_ptr<http::response<http::string_body>> res;
        try {
            res = std::make_shared<http::response<http::string_body>>(
                static_cast<http::status>(r.status), req.version());
            res->set(http::field::server, "orca-remote-api");
            res->keep_alive(false);
            if (r.raw_body) {
                res->set(http::field::content_type, r.raw_content_type);
                res->body() = *r.raw_body;
            } else {
                res->set(http::field::content_type, "application/json");
                res->body() = r.body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            }
            res->prepare_payload();
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(error) << "remote-api: response build exception: " << e.what();
            res = std::make_shared<http::response<http::string_body>>(
                http::status::internal_server_error, req.version());
            res->set(http::field::content_type, "application/json");
            res->set(http::field::server, "orca-remote-api");
            res->keep_alive(false);
            res->body() = "{\"error\":\"internal\"}";
            res->prepare_payload();
        }
        m_stream.expires_after(std::chrono::seconds(15)); // re-arm timeout for the write (slow-reader guard)
        http::async_write(m_stream, *res,
            [self = shared_from_this(), res](beast::error_code, std::size_t) {
                beast::error_code ec2;
                self->m_stream.socket().shutdown(tcp::socket::shutdown_send, ec2);
            });
    }

    beast::tcp_stream                          m_stream;
    beast::flat_buffer                         m_buffer;
    http::request_parser<http::string_body>    m_parser;
    Server                                     &m_server;
};

void Server::start(const Config &cfg)
{
    if (m_running) stop();
    m_cfg = cfg;
    try {
        m_ioc = std::make_unique<net::io_context>(1);
        auto address = m_cfg.bind_lan ? net::ip::address_v4::any()
                                      : net::ip::make_address_v4("127.0.0.1");
        tcp::endpoint endpoint(address, static_cast<unsigned short>(m_cfg.port));
        m_acceptor = std::make_unique<tcp::acceptor>(*m_ioc);
        m_acceptor->open(endpoint.protocol());
        // SO_REUSEADDR: rebind while a prior socket lingers in TIME_WAIT.
        // Without it, a rapid kill+relaunch silently fails to bind 13130.
        m_acceptor->set_option(net::socket_base::reuse_address(true));
        m_acceptor->bind(endpoint);
        m_acceptor->listen(net::socket_base::max_listen_connections);
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "remote-api: failed to bind port " << m_cfg.port
                                 << ": " << e.what();
        m_ioc.reset();
        m_acceptor.reset();
        return; // disabled for this run; the Preferences page shows "failed" via running()
    }
    m_running = true;
    do_accept();
    m_thread = create_thread([this] {
        set_current_thread_name("remote_api");
        this->m_ioc->run();
    });
    BOOST_LOG_TRIVIAL(info) << "remote-api: listening on "
                            << (m_cfg.bind_lan ? "0.0.0.0" : "127.0.0.1")
                            << ":" << m_cfg.port;
}

void Server::do_accept()
{
    m_acceptor->async_accept([this](beast::error_code ec, tcp::socket socket) {
        if (!m_running) return;
        if (!ec)
            std::make_shared<HttpSession>(std::move(socket), *this)->run();
        do_accept();
    });
}

void Server::stop()
{
    if (!m_running) return;
    m_running = false;
    if (m_ioc) m_ioc->stop();
    if (m_thread.joinable()) m_thread.join();
    m_acceptor.reset();
    m_ioc.reset();
    {
        std::lock_guard<std::mutex> lk(m_ws_mutex);
        m_ws_sessions.clear();
    }
    BOOST_LOG_TRIVIAL(info) << "remote-api: stopped";
}

bool Server::check_token(const std::string &presented) const
{
    // Constant-time comparison: this is the authentication path, so it must not
    // leak the token's contents through how early the comparison gives up. The
    // length check is separate because the token's length is not the secret.
    if (m_cfg.token.empty() || presented.size() != m_cfg.token.size())
        return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < m_cfg.token.size(); ++i)
        diff |= static_cast<unsigned char>(presented[i] ^ m_cfg.token[i]);
    return diff == 0;
}

// WS plumbing — populated in Task 11; broadcast is already safe to call.
void Server::ws_join(const std::shared_ptr<WsSession> &s)
{
    std::lock_guard<std::mutex> lk(m_ws_mutex);
    m_ws_sessions.insert(s);
}
void Server::ws_leave(const std::shared_ptr<WsSession> &s)
{
    std::lock_guard<std::mutex> lk(m_ws_mutex);
    m_ws_sessions.erase(s);
}
void Server::broadcast(const nlohmann::json &event)
{
    if (!m_running || !m_ioc) return;
    // dump() can throw on non-UTF-8 (project filenames, slice warnings carry raw
    // bytes). This runs on the GUI thread; an uncaught throw could take the
    // slicer down — same hazard the HTTP response path already guards against.
    std::shared_ptr<std::string> text;
    try {
        text = std::make_shared<std::string>(
            event.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "remote-api: broadcast dump failed: " << e.what();
        return;
    }
    net::post(*m_ioc, [this, text] {
        std::lock_guard<std::mutex> lk(m_ws_mutex);
        for (auto &s : m_ws_sessions) s->send(*text);
    });
}

}}} // namespace
