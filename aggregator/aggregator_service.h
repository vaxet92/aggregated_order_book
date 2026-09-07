#pragma once

// AggregatorServiceImpl: the gRPC service. One Subscribe stream per client,
// fanned out from Core's BboCallback/BookCallback through one ConflatedChannel
// per session (conflated_channel.h). PublishBook is where per-subscriber band
// selection happens - each session's own thresholds walked against the one
// shared consolidated book Core just published, so Core itself never learns
// what any client asked for.

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "aggregator.grpc.pb.h"
#include "conflated_channel.h"
#include "md_core/consolidated_bbo.h"
#include "md_core/consolidated_book.h"
#include "types/venue.h"
#include "wire_translation.h"

namespace market_data {

class AggregatorServiceImpl final : public wire::Aggregator::Service {
   public:
    grpc::Status Subscribe(grpc::ServerContext* context, const wire::SubscribeRequest* request,
                           grpc::ServerWriter<wire::Update>* writer) override;

    // Called by whoever owns Core (md_core_main.cpp) via Core's BboCallback
    // whenever a new consolidated BBO is available. Fans out to every session that
    // asked for BBO. May be called from any provider's thread - must be safe
    // to call concurrently.
    void PublishBbo(InstrumentKey instrument, const consolidated::BBO& bbo);

    // Called via Core's BookCallback with a fresh, immutable merged book on
    // every depth update. This is where per-subscriber band selection
    // happens: each session's own thresholds are walked against this
    // one shared snapshot, so Core never learns what any client asked for.
    // a const REFERENCE, and it may NOT be retained. Core publishes its
    // live consolidated book directly and resumes mutating it as soon as this
    // returns - which is what removed an 8.2 us snapshot copy from every
    // update. Everything below reads the levels and turns them into protobuf
    // before returning; nothing stores the book.
    void PublishBook(InstrumentKey instrument, const consolidated::Book& book);

    // Slot -> wire venue, for resolving the attribution md_core publishes by
    // slot. Set by md_core_main.cpp.
    //
    // in the deployed split, venues register on the ingress thread AFTER
    // gRPC is already serving, so this IS rebuilt while live -
    // md_core_main.cpp's refresh_wire_table calls SetVenueWireTable at the top
    // of PublishBbo/PublishBook when the venue count has changed. That is safe
    // without a lock only because both callbacks fire on the one consolidator
    // thread; see the comment there.
    void SetVenueWireTable(const VenueWireTable& venues) { venue_wire_table_ = venues; }

   private:
    // Defaults to VENUE_UNSPECIFIED everywhere, so a forgotten
    // SetVenueWireTable shows up as unattributed levels rather than as
    // levels attributed to the wrong exchange.
    VenueWireTable venue_wire_table_{};

    using Channel = ConflatedChannel<wire::Update>;

    struct Subscription {
        std::shared_ptr<Channel> channel;

        // What this session subscribed to - symbol AND market. They are two
        // separate subscriptions, so a spot subscriber must never receive a
        // futures update.
        //
        // one packed uint32 compare in the publish loops filters BOTH
        // halves at once, which is why InstrumentKey is packed. Before this
        // field existed the fanout filtered by NOTHING: every session received
        // every instrument, and FillHeader dropped the market from the header,
        // so a client could not even tell which book it had been sent.
        InstrumentKey instrument;

        bool wants_bbo = false;
        bool wants_volume_bands = false;
        bool wants_price_bands = false;

        // Sorted ascending - FillToNotionalBands/FillToBpsBands require it
        // for their single forward walk to be correct. Sorted server-side at
        // subscribe time; never trusted from the wire.
        std::vector<uint64_t> notional_bands;
        std::vector<uint32_t> bps_bands;

        // Per-session, NOT global: seq means "position in THIS client's
        // stream", so a gap means this client's own channel conflated
        // something. A counter shared across sessions would make
        // every client report gaps caused by traffic sent to other clients.
        // Mutated only under sessions_mutex_, so it needs no atomic.
        uint64_t next_seq{};
    };

    uint64_t RegisterSession(Subscription subscription);
    void UnregisterSession(uint64_t session_id);

    std::mutex sessions_mutex_;
    std::unordered_map<uint64_t, Subscription> sessions_;
    uint64_t next_session_id_{1};
};

}  // namespace market_data
