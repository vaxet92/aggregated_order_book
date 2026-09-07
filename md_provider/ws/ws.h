#pragma once

// WebSocketSessionSSL: one Boost.Beast WSS connection - connect, TLS/WS
// handshake, optional subscribe frame, read loop, and a once-only,
// not-after-Stop() SetOnClosed callback the owner (Provider) uses to drive
// reconnection. One instance per redundant socket per stream.

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <functional>
#include <memory>
#include <string>
#include <iostream>
#include "root_certificates.hpp"
#include "logger/logger.h"

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

#define WS_TIMER_RATE 30

class WebSocketSessionSSL : public std::enable_shared_from_this<WebSocketSessionSSL> {
   public:
    explicit WebSocketSessionSSL(net::io_context& ioc, ssl::context& ctx,
                                 std::function<void(const std::string&)> onMessageCallback);
    ~WebSocketSessionSSL() = default;

    // If subscribeMessage is non-empty, it is sent once, right after the
    // handshake completes and before the read loop starts. Binance connects
    // directly to a per-stream URL and passes "" here; Bybit/OKX use one
    // generic endpoint and need an explicit {"op":"subscribe",...} frame.
    void Run(const char* inHost, const char* inPort, const char* inTarget, std::string subscribeMessage = "");
    void Stop();

    // Called once when this session terminates for any reason the owner did
    // not ask for: connect/handshake failure, read error, or the venue
    // closing on us. NOT called after Stop() - a deliberate shutdown is not
    // something to reconnect from.
    //
    // Fires on the io_context thread, from inside a completion handler. The
    // owner must not block in it, and must not destroy this session from it -
    // post the reconnect instead.
    //
    // Set after construction rather than passed in, so the owner can capture
    // per-session state (such as a connection index) in the lambda while
    // building sessions in a loop.
    void SetOnClosed(std::function<void()> callback) { on_closed_ = std::move(callback); }

   private:
    // Fires on_closed_ at most once, and never after Stop().
    //
    // once-only matters because the far end of this callback opens a
    // replacement socket. A session that notified twice would leave the
    // provider holding two sockets for one dead connection, and the count
    // would drift upward on every flap - eventually past the venue's
    // connection limit, which is the failure redundancy exists to avoid.
    //
    // silent after Stop() for the mirror reason. During shutdown every
    // session dies; if each asked to be reconnected the process would never
    // exit.
    void NotifyClosed();

    void OnResolve(beast::error_code ec, tcp::resolver::results_type results);
    void OnConnect(beast::error_code ec, tcp::resolver::results_type::endpoint_type ep);
    void OnSslHandshake(beast::error_code ec);
    void OnHandshake(beast::error_code ec);
    void OnSubscribeWrite(beast::error_code ec, std::size_t bytes_transferred);
    void OnRead(beast::error_code ec, std::size_t bytes_transferred);
    void OnClose(beast::error_code ec);

    tcp::resolver resolver;
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws;
    beast::flat_buffer buffer;
    std::string host;
    std::string target;
    std::string subscribe_message_;
    std::function<void(const std::string&)> onMessageCallback;
    std::function<void()> on_closed_;
    bool closed_notified_ = false;
    bool stopped;
};

using WebSocketSessionSSLPtr = std::shared_ptr<WebSocketSessionSSL>;
