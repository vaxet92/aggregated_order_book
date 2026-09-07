#pragma once

// HttpsGet: one venue-agnostic, blocking, synchronous HTTPS GET, used for
// Binance's snapshot REST call and OKX's contract-size lookup. Must never be
// called from a provider's io_context thread - see the warning below.

#include <optional>
#include <string>

namespace market_data {

// One-shot synchronous HTTPS GET. Returns the response body, or
// std::nullopt on any failure (DNS, TLS, non-200 status, malformed
// response). Never throws.
//
// Venue-agnostic. This lived in binance/ while Binance's depth snapshot was
// the only REST call in the project; OKX now needs one too (to read a swap's
// contract size), and a file named binance_rest.h included from okx_provider
// would have been actively misleading about what depends on what.
//
// Blocking BY DESIGN, but it MUST NOT be called from a Provider's
// io_context thread: that thread drives the WebSocket reads, and blocking
// it for an HTTP round-trip stalls exactly the event buffering the snapshot
// sync depends on. Call it from a separate thread and hand the result back
// with net::post(ioc, ...).
std::optional<std::string> HttpsGet(const std::string& host, const std::string& port, const std::string& target);

}  // namespace market_data
