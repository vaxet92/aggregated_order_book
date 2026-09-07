#include "ws.h"

static void fail(beast::error_code ec, const char* what) {
    Logger::Log(LogLevel::kError, "[WebSocket] {}: {}", what, ec.message());
}

WebSocketSessionSSL::WebSocketSessionSSL(net::io_context& ioc, ssl::context& ctx,
                                         std::function<void(const std::string&)> onMessageCallback)
    : resolver(net::make_strand(ioc)),
      ws(net::make_strand(ioc), ctx),
      onMessageCallback(std::move(onMessageCallback)),
      stopped(false) {}

void WebSocketSessionSSL::Run(const char* inHost, const char* inPort, const char* inTarget,
                              std::string subscribeMessage) {
    if (stopped) {
        return;
    }

    host = inHost;
    target = inTarget;
    subscribe_message_ = std::move(subscribeMessage);

    resolver.async_resolve(inHost, inPort,
                           beast::bind_front_handler(&WebSocketSessionSSL::OnResolve, shared_from_this()));
}

void WebSocketSessionSSL::NotifyClosed() {
    // Deliberate shutdown, or already reported - either way the owner must
    // not be asked to reconnect. See the header for why both guards matter.
    if (stopped || closed_notified_) {
        return;
    }
    closed_notified_ = true;
    if (on_closed_) {
        on_closed_();
    }
}

void WebSocketSessionSSL::OnResolve(beast::error_code ec, tcp::resolver::results_type results) {
    if (ec) {
        fail(ec, "resolve");
        return NotifyClosed();
    } else if (stopped) {
        return;
    }

    beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(WS_TIMER_RATE));

    beast::get_lowest_layer(ws).async_connect(
        results, beast::bind_front_handler(&WebSocketSessionSSL::OnConnect, shared_from_this()));
}

void WebSocketSessionSSL::OnConnect(beast::error_code ec, tcp::resolver::results_type::endpoint_type ep) {
    if (ec) {
        fail(ec, "connect");
        return NotifyClosed();
    } else if (stopped) {
        return;
    }

    beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(WS_TIMER_RATE));

    // Set SNI hostname
    if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), host.c_str())) {
        ec = beast::error_code(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category());
        fail(ec, "SSL SNI");
        return NotifyClosed();
    }

    // Update host with port for handshake
    std::string host_with_port = fmt::format("{}:{}", host, ep.port());

    ws.next_layer().async_handshake(
        ssl::stream_base::client, beast::bind_front_handler(&WebSocketSessionSSL::OnSslHandshake, shared_from_this()));
}

void WebSocketSessionSSL::OnSslHandshake(beast::error_code ec) {
    if (ec) {
        fail(ec, "ssl_handshake");
        return NotifyClosed();
    } else if (stopped) {
        return;
    }

    beast::get_lowest_layer(ws).expires_never();

    ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
    ws.set_option(websocket::stream_base::decorator([](websocket::request_type& req) {
        req.set(http::field::user_agent, std::string(BOOST_BEAST_VERSION_STRING) + " consolidated-candles");
    }));

    ws.async_handshake(host, target, beast::bind_front_handler(&WebSocketSessionSSL::OnHandshake, shared_from_this()));
}

void WebSocketSessionSSL::OnHandshake(beast::error_code ec) {
    if (ec) {
        fail(ec, "handshake");
        return NotifyClosed();
    } else if (stopped) {
        return;
    }
    Logger::Log(LogLevel::kInfo, "[WebSocket] Handshake successful with {}{}", host, target);

    if (!subscribe_message_.empty()) {
        ws.async_write(net::buffer(subscribe_message_),
                       beast::bind_front_handler(&WebSocketSessionSSL::OnSubscribeWrite, shared_from_this()));
        return;
    }

    ws.async_read(buffer, beast::bind_front_handler(&WebSocketSessionSSL::OnRead, shared_from_this()));
}

void WebSocketSessionSSL::OnSubscribeWrite(beast::error_code ec, std::size_t bytes_transferred) {
    if (ec) {
        fail(ec, "subscribe_write");
        return NotifyClosed();
    } else if (stopped) {
        return;
    }

    Logger::Log(LogLevel::kInfo, "[WebSocket] Subscribed to {}:{}", host, subscribe_message_);

    ws.async_read(buffer, beast::bind_front_handler(&WebSocketSessionSSL::OnRead, shared_from_this()));
}

void WebSocketSessionSSL::OnRead(beast::error_code ec, std::size_t bytes_transferred) {
    if (ec == websocket::error::closed) {
        Logger::Log(LogLevel::kInfo, "[WebSocket] Connection closed by {}{}", host, target);
        return NotifyClosed();
    } else if (ec) {
        fail(ec, "read");
        return NotifyClosed();
    } else if (stopped) {
        return;
    }

    if (bytes_transferred > 0 && onMessageCallback) {
        std::string message = beast::buffers_to_string(buffer.data());
        buffer.consume(bytes_transferred);

        // Call the message callback
        onMessageCallback(message);
    }

    // Continue reading
    ws.async_read(buffer, beast::bind_front_handler(&WebSocketSessionSSL::OnRead, shared_from_this()));
}

void WebSocketSessionSSL::Stop() {
    stopped = true;
}

void WebSocketSessionSSL::OnClose(beast::error_code ec) {
    if (ec) return fail(ec, "close");
    Logger::Log(LogLevel::kInfo, "[WebSocket] Closed successfully: {}{}", host, target);
}
