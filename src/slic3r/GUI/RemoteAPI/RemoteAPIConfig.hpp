// src/slic3r/GUI/RemoteAPI/RemoteAPIConfig.hpp
#ifndef slic3r_RemoteAPIConfig_hpp_
#define slic3r_RemoteAPIConfig_hpp_

#include <string>

namespace Slic3r { namespace GUI { namespace RemoteAPI {

// Persisted in AppConfig (main-thread only: load()/save() must run on the GUI thread).
struct Config
{
    bool        enabled  { false };
    int         port     { 13130 };
    bool        bind_lan { false };
    bool        notify   { true };  // toast in-app when the API changes state
    std::string token;

    static Config      load();
    void               save() const;
    // 32 hex chars from std::random_device. Not for cryptographic secrets in
    // general, but fine for a LAN API token.
    static std::string generate_token();
};

}}} // namespace

#endif
