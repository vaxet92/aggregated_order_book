#include "client_common.h"

using namespace market_data;

// Fixed-subscription test binary: price bands only, server defaults, no
// flags. client_app is the production client - this exists to exercise one
// feed in isolation.
int main() {
    ClientConfig config;
    // Explicit, not a default - see bbo_sub.cpp.
    config.market = wire::SPOT;
    config.want_price_bands = true;  // empty bps_bands = server defaults

    return RunSubscription(config, "price_band_sub", config.ToRequest(), PrintPriceBands);
}
