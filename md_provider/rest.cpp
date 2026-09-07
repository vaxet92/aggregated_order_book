#include "rest.h"
#include "logger/logger.h"
#include "root_certificates.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>

namespace market_data {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = net::ip::tcp;

std::optional<std::string> HttpsGet(const std::string& host, const std::string& port, const std::string& target) {
    try {
        // A local io_context, used synchronously - deliberately NOT the
        // Provider's. This call owns its whole connection lifetime and
        // blocks the calling thread until done.
        net::io_context ioc;
        ssl::context ctx(ssl::context::tlsv12_client);
        load_root_certificates(ctx);
        ctx.set_verify_mode(ssl::verify_peer);

        tcp::resolver resolver(ioc);
        beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);

        // SNI - without this the TLS handshake fails against virtual hosts.
        if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
            Logger::Log(LogLevel::kError, "[HTTPS] SNI failed for {}", host);
            return std::nullopt;
        }

        auto const results = resolver.resolve(host, port);
        beast::get_lowest_layer(stream).connect(results);
        stream.handshake(ssl::stream_base::client);

        http::request<http::string_body> req{http::verb::get, target, 11};
        req.set(http::field::host, host);
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        http::write(stream, req);

        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(stream, buffer, res);

        // Many servers close without a clean TLS shutdown; stream_truncated
        // here is normal and not worth reporting. The error is captured and
        // deliberately ignored - the response body is already read.
        beast::error_code shutdown_ec;
        stream.shutdown(shutdown_ec);

        if (res.result() != http::status::ok) {
            Logger::Log(LogLevel::kError, "[HTTPS] GET {}{} returned status {}", host, target, res.result_int());
            return std::nullopt;
        }

        return res.body();

    } catch (const std::exception& e) {
        Logger::Log(LogLevel::kError, "[HTTPS] GET {}{} failed: {}", host, target, e.what());
        return std::nullopt;
    }
}

}  // namespace market_data
