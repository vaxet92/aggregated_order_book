#include "okx_provider.h"
#include "okx_parser.h"
#include "continuity.h"
#include "logger/logger.h"
#include "md_provider/rest.h"
#include "types/venue.h"
#include <fmt/format.h>

using namespace market_data;

namespace {

// OKX uses "BTC-USDT", not "BTCUSDT" - insert a hyphen before the quote
// currency. Simple and matches every symbol this project uses (all end in
// USDT).
//
// the futures instrument is a PERPETUAL SWAP and has its own instId,
// "BTC-USDT-SWAP". It is a different instrument from "BTC-USDT" with its own
// order book, its own seqId chain and - the part that bites - its own size
// units: swap sizes are in CONTRACTS, spot sizes are in BTC. Subscribing to
// the spot instId for a futures book would silently deliver spot data into a
// futures book, which is exactly what MarketType on the key exists to prevent.
std::string ToOkxInstId(InstrumentKey instrument) {
    std::string symbol = VenueConverter::ToInstrumentString(instrument.Symbol());
    size_t usdt_pos = symbol.find("USDT");
    if (usdt_pos != std::string::npos) {
        symbol.insert(usdt_pos, "-");
    }
    if (instrument.Market() == MarketType::kFutures) {
        symbol += "-SWAP";
    }
    return symbol;
}

}  // namespace

OKXProvider::OKXProvider(const ProviderConfig& config, CallBack callback, QuoteCallBack quote_callback)
    : Provider(config, std::move(callback), std::move(quote_callback)), parser_(config.depth) {
    // OKX's path (/ws/v5/public) carries no {symbol} - the instId goes in the
    // subscribe frame. Same reasoning as Bybit: resolve anyway so the path
    // shape stays purely a config concern.
    ResolveStreamPaths(ToOkxInstId(config.instrument));
}

bool OKXProvider::OnReconnect() {
    // Spot sizes are already in the base currency - nothing to resolve, and
    // parser_ keeps its 1.0 default.
    if (config.instrument.Market() != MarketType::kFutures || contract_size_resolved_) {
        return true;
    }

    const std::string inst_id = ToOkxInstId(config.instrument);

    // this blocking GET happens HERE, on the worker thread, before
    // ioc.restart() and before a single socket exists. Doing it concurrently
    // with the stream would let the first book messages be parsed at 1.0x and
    // then silently re-scaled to 0.01x once the answer arrived - a book that
    // is 100x wrong at the top and correct lower down, with no error anywhere.
    // Blocking is normally forbidden in a provider; this is the one place it
    // is not, because there is no read loop yet to stall.
    const std::string target = fmt::format("{}?instType=SWAP&instId={}", config.rest_instruments_path, inst_id);
    const std::optional<std::string> body = HttpsGet(config.rest_host, config.rest_port, target);

    const std::optional<QtyUnits> contract_size = body ? ParseOkxContractSize(*body, inst_id) : std::nullopt;
    if (!contract_size.has_value()) {
        // refuse to connect rather than fall back to 1.0. A default of
        // 1.0 would publish OKX swap sizes 100x oversized into a consolidated
        // futures book it shares with Binance and Bybit, dominating every
        // price level and wrecking the notional bands - and it would look like
        // a deep market, not like an error. A venue that is ABSENT is safe;
        // a venue that is wrong is not.
        //
        // Returning false routes this through HandleReconnection(), so a
        // transient network failure backs off and retries while a persistent
        // one eventually stops the provider.
        Logger::Log(LogLevel::kError, "[{}] could not resolve contract size for {} - refusing to connect ({})",
                    venue_market_str_, inst_id, body ? "unexpected response" : "request failed");
        return false;
    }

    parser_.SetContractSize(*contract_size);
    contract_size_resolved_ = true;
    Logger::Log(LogLevel::kInfo, "[{}] {} contract size {} - sizes convert from contracts to base currency",
                venue_market_str_, inst_id, static_cast<double>(*contract_size) / static_cast<double>(kScaleFactor));
    return true;
}

std::string OKXProvider::CombinedSubscriptionMessage() const {
    // Both channels in one subscribe frame.
    const std::string inst_id = ToOkxInstId(config.instrument);
    return fmt::format(
        R"({{"op":"subscribe","args":[{{"channel":"books","instId":"{}"}},{{"channel":"bbo-tbt","instId":"{}"}}]}})",
        inst_id, inst_id);
}

void OKXProvider::OnCombinedMessage(const std::string& message, uint32_t conn_index) {
    // Cheap peek at `arg.channel`, decided BEFORE the real parse below -
    // both OnDepthMessage/OnBboMessage call their own parser_.Parse*Message
    // again on the same message, which is correct (PeekChannel's result is
    // only used here, immediately, never held past this point) but does mean
    // two simdjson passes per message rather than one. OKX's frames are
    // small; not measured, and not worth avoiding without a number.
    auto channel = parser_.PeekChannel(message);
    if (!channel) {
        return;  // not a channel push (e.g. subscribe ack, pong)
    }

    if (*channel == "bbo-tbt") {
        OnBboMessage(message, conn_index);
    } else {
        // "books", or anything unrecognised: OnDepthMessage's own parse
        // (gated on `action`, which bbo-tbt never carries) discards
        // anything that is not actually a books message.
        OnDepthMessage(message, conn_index);
    }
}

void OKXProvider::OnDepthMessage(const std::string& message, uint32_t conn_index) {
    auto update = parser_.ParseBooksMessage(message, config.venue_id, config.instrument);
    if (!update) {
        return;  // not a books update (e.g. subscribe ack, pong)
    }

    // OKX proves liveness by sending seqId == prevSeqId with empty
    // bids/asks after roughly 60 seconds of no book change. That id is one we
    // have already seen, so AcceptDepth below drops it - and the kIgnore
    // branch in CheckOkxContinuity that documents this keepalive sits behind
    // the filter and never runs for it. Stamping here is what preserves the
    // signal.
    //
    // 60s is a long backstop compared with Bybit's 3s on L1, so OKX depth
    // still needs connection state and cross-venue comparison to notice a
    // dead feed quickly. The keepalive bounds the worst case; it is not a
    // fast detector.
    NoteDepthActivity();

    // only the documented MAINTENANCE RESET counts - seqId jumps
    // backwards while prevSeqId still chains (prevSeqId=15, seqId=3). It is
    // not a gap (CheckOkxContinuity below treats it as kApply), but it does
    // put the id under our high-water mark, and without this flag the filter
    // would drop it and every message after it, forever, with nothing logged.
    // That is the silent freeze SeqDedup::LooksStuck() guards against.
    //
    // An ordinary snapshot (prevSeqId == -1) is deliberately NOT included.
    // Its seqId moves forward, so the `<=` rule already decides correctly, and
    // flagging it as a reset let a late-connecting socket's stale snapshot
    // drag the mark backwards - the live Bybit gap we hit.
    //
    // prev_seq > 0, not >= 0: -1 is the snapshot marker and 0 is what the
    // parser writes when the field is missing. Treating a parse failure as a
    // reset would be worse than treating it as a normal message.
    const bool venue_reset = (update->prev_seq > 0 && update->seq < static_cast<uint64_t>(update->prev_seq));

    if (!AcceptDepth(update->seq, conn_index, venue_reset)) {
        return;
    }

    switch (CheckOkxContinuity(*update, last_depth_seq_)) {
        case ContinuityAction::kIgnore:
            return;
        case ContinuityAction::kGap:
            // Chain broken: the book is WRONG, not merely stale.
            Logger::Log(LogLevel::kWarning, "[{}] depth gap: expected prevSeqId={}, got {} - resyncing",
                        venue_market_str_, last_depth_seq_, update->prev_seq);
            last_depth_seq_ = 0;
            RequestResync();
            return;
        case ContinuityAction::kReset:
        case ContinuityAction::kApply:
            break;
    }

    update->recv_ts_ns = GetCurrentTimeMs() * kTsNsMultiplier;

    Emit(std::move(*update));
}

void OKXProvider::OnBboMessage(const std::string& message, uint32_t conn_index) {
    auto update = parser_.ParseBboMessage(message, config.venue_id, config.instrument);
    if (!update) {
        return;  // not a bbo-tbt payload (e.g. subscribe ack, pong)
    }

    // Whether bbo-tbt has its own keepalive is NOT documented as far as we
    // have checked, unlike the books channel above. So this stamp records
    // real data only, and until that is verified against a live capture the
    // BBO stream's liveness must lean on connection state and cross-venue
    // comparison rather than on a timer.
    NoteBboActivity();

    // Separate filter from the depth stream: bbo-tbt and books carry
    // independent seqId sequences, so a shared high-water mark would silently
    // drop one stream behind the other.
    if (!AcceptBbo(update->seq, conn_index)) {
        return;
    }

    // bbo-tbt always carries both sides, one level each (verified against
    // live capture) - a full replacement every message, no deltas.
    if (update->bids.empty() || update->asks.empty()) {
        return;
    }

    BboQuote quote;
    quote.venue = config.venue_id;
    quote.instrument = config.instrument;
    quote.seq = update->seq;  // OKX seqId
    quote.exch_ts_ns = update->exch_ts_ns;
    quote.recv_ts_ns = GetCurrentTimeMs() * kTsNsMultiplier;
    quote.bid_price = update->bids.front().price;
    quote.bid_qty = update->bids.front().qty;
    quote.ask_price = update->asks.front().price;
    quote.ask_qty = update->asks.front().qty;
    EmitQuote(quote);
}
