#ifndef slic3r_ElegooLink_hpp_
#define slic3r_ElegooLink_hpp_

#include <boost/optional/optional.hpp>
#include <cstddef>
#include <boost/filesystem/path.hpp>
#include <string>
#include <wx/string.h>
#include <boost/optional.hpp>
#include <boost/asio/ip/address.hpp>

#include "PrintHost.hpp"
#include "OctoPrint.hpp"

class WebSocketClient;
namespace boost { template <class T> class optional; }
#include "WebSocketClient.hpp"
#include "ElegooCanvasSession.hpp"
namespace Slic3r {

class DynamicPrintConfig;
class Http;

class ElegooLink : public OctoPrint
{
public:
    ElegooLink(DynamicPrintConfig *config);
    ~ElegooLink() override = default;
    static std::string get_print_host_webui(DynamicPrintConfig *config);
    const char* get_name() const override;
    virtual bool test(wxString &curl_msg) const override;
    wxString get_test_ok_msg() const override;
    wxString get_test_failed_msg(wxString& msg) const override;
    bool upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const override;
    std::string get_sn() const override;
    bool has_auto_discovery() const override { return false; }
    bool can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override;

    // Centauri Carbon 2 (MQTT, CANVAS). is_cc2_model() is the same check the upload path uses.
    static bool is_cc2_model(const std::string &printer_model);
    bool is_cc2() const;
    // host from print_host, access code from printhost_apikey (default 123456), serial from the dev_sn cache
    // (fetched over HTTP when not cached yet). A non-empty `host_override` (a URL or bare host) replaces the MQTT
    // host only, so the start command reaches the address the upload used (Windows .local resolved by Bonjour).
    ElegooCanvas::Endpoint canvas_endpoint(const std::string &host_override = {}) const;
    // Attributes then CANVAS status; "firmware_too_old" below 1.1.
    ElegooCanvas::Session::Result get_canvas_info(ElegooCanvas::Info &out) const;
    // MQTT 1020 for an uploaded file. A refusal is reported through info_fn (tag "Uploaded"), never error_fn,
    // because the upload itself succeeded; returns whether the printer accepted the start. `host_override` as in
    // canvas_endpoint().
    bool start_print_cc2(const std::string &filename, const PrintHostUpload &upload_data, ErrorFn error_fn, InfoFn info_fn,
                         const std::string &host_override = {}) const;
protected:
#ifdef WIN32
    virtual bool upload_inner_with_resolved_ip(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn, const boost::asio::ip::address& resolved_addr) const override;
#endif
    virtual bool validate_version_text(const boost::optional<std::string> &version_text) const override;
    virtual bool upload_inner_with_host(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const override;

#ifdef WIN32
    virtual bool test_with_resolved_ip(wxString& curl_msg) const override;
    bool elegoo_test_with_resolved_ip(wxString& curl_msg) const;
#endif
private:
    bool elegoo_test(wxString& curl_msg) const;
    bool elegoo_cc2_test(wxString& curl_msg) const;
    bool print(WebSocketClient&  client,
               std::string       timeLapse,
               std::string       heatedBedLeveling,
               std::string       bedType,
               const std::string filename, ErrorFn error_fn) const;
    bool checkResult(WebSocketClient&  client,
               ErrorFn           error_fn) const;

    bool loopUpload(std::string url, PrintHostUpload upload_data,
                    ProgressFn        prorgess_fn,
                    ErrorFn           error_fn,
                    InfoFn            info_fn) const;
    bool loopUploadCC2(std::string url,
                       const std::string& host_header,
                       PrintHostUpload    upload_data,
                       ProgressFn         prorgess_fn,
                       ErrorFn            error_fn,
                       InfoFn             info_fn) const;

    bool uploadPart(Http &http,
                    std::string       md5,
                    std::string       uuid,
                    std::string       path,
                    std::string       filename,
                    size_t            filesize,
                    size_t            offset,
                    size_t            length,
                    ProgressFn        prorgess_fn,
                    ErrorFn           error_fn,
                    InfoFn            info_fn) const;
    bool uploadPartCC2(Http&                           http,
                       const std::string&             host_header,
                       const std::string&             token,
                       const std::string&             md5,
                       const boost::filesystem::path& path,
                       const std::string&             filename,
                       size_t                         filesize,
                       size_t                         offset,
                       size_t                         length,
                       ProgressFn                     prorgess_fn,
                       ErrorFn                        error_fn) const;

    std::string cc2_token() const;
    std::string make_cc2_info_url() const;
    std::string make_cc2_upload_url() const;
#ifdef WIN32
    bool elegoo_cc2_test_with_resolved_ip(wxString& curl_msg) const;
#endif

    std::string m_printerModel;
};
}

#endif
