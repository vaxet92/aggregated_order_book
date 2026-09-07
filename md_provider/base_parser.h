#pragma once

// Parser: the base class every venue parser (BinanceParser, BybitParser,
// OkxParser) derives from - a reused simdjson::ondemand::parser and a
// growable input buffer, so a steady message stream does no per-message
// allocation. One instance per thread; not thread-safe.

#include <simdjson.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace market_data {

// Shared state for a venue parser: a reusable simdjson parser and input
// buffer, so a steady stream of messages does no per-message allocation,
// plus the level-reserve hint used when building each BookUpdate.
//
// NOT thread-safe: one instance serves one thread.
class Parser {
   public:
    // venue_depth: this venue's resolved book-depth tier (ProviderConfig::depth).
    explicit Parser(uint32_t venue_depth) : reserve_levels_(venue_depth) {}

   protected:
    // Copies `raw` into input_ (grown to raw.size() + SIMDJSON_PADDING) and
    // returns a view simdjson can iterate. input_ only ever grows, so once it
    // has seen the largest message a steady stream does no allocation.
    simdjson::padded_string_view Load(std::string_view raw) {
        input_.resize(raw.size() + simdjson::SIMDJSON_PADDING);
        std::memcpy(input_.data(), raw.data(), raw.size());
        return simdjson::padded_string_view(input_.data(), raw.size(), input_.size());
    }

    const std::size_t reserve_levels_;
    simdjson::ondemand::parser parser_;
    std::string input_;
};

}  // namespace market_data
