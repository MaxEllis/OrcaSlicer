// src/slic3r/GUI/RemoteAPI/RemoteAPIServer.hpp
#ifndef slic3r_RemoteAPIServer_hpp_
#define slic3r_RemoteAPIServer_hpp_

#include "RemoteAPIConfig.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/thread.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>

namespace Slic3r { namespace GUI { namespace RemoteAPI {

struct Request
{
    std::string method;
    std::string target; // path incl. query, e.g. "/api/v1/config?keys=layer_height"
    std::string body;
};

struct Response
{
    int                        status { 200 };
    nlohmann::json             body;
    // If set, sent verbatim with raw_content_type instead of JSON-dumping `body`
    // (used by GET /gcode to return the raw G-code file). Defaults keep every
    // other handler's JSON behavior byte-for-byte unchanged.
    std::optional<std::string> raw_body;
    std::string                raw_content_type { "application/octet-stream" };
};

class WsSession; // Task 11

class Server
{
public:
    using Handler = std::function<Response(const Request &)>;

    Server()  = default;
    ~Server() { stop(); }

    void set_handler(Handler h) { m_handler = std::move(h); }
    void start(const Config &cfg);
    void stop();
    bool running() const { return m_running; }
    int  port() const { return m_cfg.port; }

    // Thread-safe: called from the GUI thread, writes go out on the io thread.
    void broadcast(const nlohmann::json &event);

    // Internal (used by sessions):
    bool           check_token(const std::string &presented) const;
    const Handler &handler() const { return m_handler; }
    void ws_join(const std::shared_ptr<WsSession> &s);
    void ws_leave(const std::shared_ptr<WsSession> &s);
    boost::asio::io_context &ioc() { return *m_ioc; }

private:
    void do_accept();

    Config                                     m_cfg;
    Handler                                    m_handler;
    std::atomic<bool>                          m_running { false };
    std::unique_ptr<boost::asio::io_context>   m_ioc;
    std::unique_ptr<boost::asio::ip::tcp::acceptor> m_acceptor;
    boost::thread                              m_thread;
    std::mutex                                 m_ws_mutex;
    std::set<std::shared_ptr<WsSession>>       m_ws_sessions;
};

}}} // namespace

#endif
