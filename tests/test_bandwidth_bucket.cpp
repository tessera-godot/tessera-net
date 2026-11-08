// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/replication/bandwidth_bucket.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

using namespace tessera::net;

TESSERA_TEST(bucket_rate_per_tick) {
    // 100 kbps is 12,500 bytes a second: 416 bytes and a bit at 30 ticks a second.
    BandwidthBucket bucket(100, 100, 30);
    TESSERA_CHECK(!bucket.Unlimited());
    TESSERA_CHECK(bucket.BytesPerTickMilli() == 416'666); // 100 * 125,000 / 30
    BandwidthBucket sixty(100, 100, 60);
    TESSERA_CHECK(sixty.BytesPerTickMilli() == 208'333);
}

TESSERA_TEST(bucket_capacity_is_the_peak_window) {
    // (200 - 100) kbps for 10 seconds: 100 * 125 bytes a second * 10 = 125,000 bytes.
    BandwidthBucket bucket(100, 200, 30);
    TESSERA_CHECK(bucket.CapacityBytes() == 125'000);
    BandwidthBucket flat(100, 100, 30);
    TESSERA_CHECK(flat.CapacityBytes() == 0);
}

TESSERA_TEST(bucket_starts_full) {
    BandwidthBucket bucket(100, 200, 30);
    TESSERA_CHECK(bucket.AvailableBytes() == 125'000);
    BandwidthBucket flat(100, 100, 30); // no savings, but a tick's worth is always available
    TESSERA_CHECK(flat.AvailableBytes() == 416);
}

TESSERA_TEST(bucket_refill_stops_at_capacity) {
    BandwidthBucket bucket(100, 200, 30);
    for (int i = 0; i < 100; ++i) {
        bucket.Refill();
    }
    TESSERA_CHECK(bucket.AvailableBytes() == 125'000); // full stays full
    bucket.Charge(10'000);
    TESSERA_CHECK(bucket.AvailableBytes() == 115'000);
    for (int i = 0; i < 1000; ++i) {
        bucket.Refill();
    }
    TESSERA_CHECK(bucket.AvailableBytes() == 125'000);
}

TESSERA_TEST(bucket_charge_can_go_into_debt_and_recovers) {
    BandwidthBucket bucket(100, 100, 30); // a tick's worth: 416.666 bytes
    bucket.Charge(1000);                  // 584 bytes more than it had
    TESSERA_CHECK(bucket.AvailableBytes() == 0);
    bucket.Refill(); // +416.666: still in debt
    TESSERA_CHECK(bucket.AvailableBytes() == 0);
    bucket.Refill(); // +416.666 more: 249.998 bytes are positive again
    TESSERA_CHECK(bucket.AvailableBytes() == 249);
    bucket.Refill();
    TESSERA_CHECK(bucket.AvailableBytes() == 416); // back at the cap of one tick
}

TESSERA_TEST(bucket_never_exceeds_rate_times_window_plus_capacity) {
    struct Case {
        uint32_t rate, peak, tick_rate;
    };
    const Case cases[] = {{100, 200, 30}, {100, 100, 30}, {37, 90, 60}, {333, 400, 7}, {1, 1, 1000}};
    for (const Case& c : cases) {
        BandwidthBucket bucket(c.rate, c.peak, c.tick_rate);
        std::vector<uint64_t> spent;
        for (uint32_t tick = 0; tick < 20 * c.tick_rate; ++tick) {
            bucket.Refill();
            const uint64_t take = static_cast<uint64_t>(bucket.AvailableBytes()); // spend all it allows
            bucket.Charge(static_cast<uint32_t>(take));
            spent.push_back(take);
        }
        const double per_tick = c.rate * 125.0 / c.tick_rate;
        for (const uint32_t window : {1u, c.tick_rate, 10 * c.tick_rate}) {
            uint64_t sum = 0;
            uint64_t most = 0;
            for (size_t i = 0; i < spent.size(); ++i) {
                sum += spent[i];
                if (i >= window) {
                    sum -= spent[i - window];
                }
                most = std::max(most, sum);
            }
            const double bound =
                per_tick * window + static_cast<double>(bucket.CapacityBytes()) + per_tick + 1.0;
            TESSERA_CHECK(static_cast<double>(most) <= bound);
        }
    }
}

TESSERA_TEST(bucket_default_is_unlimited) {
    BandwidthBucket bucket;
    TESSERA_CHECK(bucket.Unlimited());
    TESSERA_CHECK(bucket.AvailableBytes() == std::numeric_limits<int64_t>::max());
    bucket.Charge(1'000'000);
    bucket.Refill();
    TESSERA_CHECK(bucket.AvailableBytes() == std::numeric_limits<int64_t>::max());
}

TESSERA_TEST(bucket_rate_zero_is_unlimited) {
    BandwidthBucket bucket(0, 0, 30);
    TESSERA_CHECK(bucket.Unlimited());
}
