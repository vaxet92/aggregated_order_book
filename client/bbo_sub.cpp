#include "client_common.h"

using namespace market_data;

// Fixed-subscription test binary: BBO only, no flags. client_app is the
// production client - this exists to exercise one feed in isolation.
int main() {
    ClientConfig config;
    // Set explicitly, because ClientConfig has no default market - spot and
    // futures are separate subscriptions. This is a CHOICE made here in
    // source, visible at the call site, not a fallback hidden in the config.
    // Change this one line to point the binary at futures.
    config.market = wire::SPOT;
    config.want_bbo = true;

    return RunSubscription(config, "bbo_sub", config.ToRequest(), PrintBbo);
}
