// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace tessera::net {

/// How many bytes a connection may be sent: a bucket of tokens that fills at a steady rate up to a limit, and
/// that every send empties. It lets a quiet connection save up for a burst (a player joining a busy area)
/// and keeps the average, over any stretch of time, at the rate.
///
/// Over any window of `W` seconds the bytes taken are at most `rate * W + capacity` (plus one tick of rate,
/// because the bucket always holds at least a tick's worth). Tokens are counted in thousandths of a byte so
/// that a rate that does not divide the tick loses nothing. The bucket may go into debt: a send larger than
/// what was left is allowed, and the debt is paid back before the next one.
class BandwidthBucket {
public:
    /// No limit: everything is allowed.
    BandwidthBucket() = default;

    /// `rate_kbps` is the average in kilobits (1,000 bits) a second; 0 means no limit. `peak_kbps` is what
    /// may be averaged over 10 seconds, at least the rate: the difference is the capacity saved for bursts.
    BandwidthBucket(uint32_t rate_kbps, uint32_t peak_kbps, uint32_t tick_rate) {
        if (rate_kbps == 0 || tick_rate == 0) {
            return;
        }
        unlimited_ = false;
        rate_milli_ = static_cast<uint64_t>(rate_kbps) * 125'000 / tick_rate;
        const uint64_t peak = std::max(peak_kbps, rate_kbps);
        capacity_milli_ = (peak - rate_kbps) * 1'250'000;
        limit_milli_ = std::max(capacity_milli_, rate_milli_);
        tokens_ = static_cast<int64_t>(limit_milli_); // a connection starts with a full bucket
    }

    bool Unlimited() const noexcept {
        return unlimited_;
    }

    /// Once per tick, before the tick's buffer is written: adds a tick of rate, up to the limit.
    void Refill() noexcept {
        if (unlimited_) {
            return;
        }
        tokens_ = std::min<int64_t>(tokens_ + static_cast<int64_t>(rate_milli_),
                                    static_cast<int64_t>(limit_milli_));
    }

    /// Whole bytes that may be sent now: 0 while in debt, the largest `int64_t` without a limit.
    int64_t AvailableBytes() const noexcept {
        if (unlimited_) {
            return std::numeric_limits<int64_t>::max();
        }
        return tokens_ > 0 ? tokens_ / 1000 : 0;
    }

    /// Takes `bytes` out of the bucket. It may go below zero.
    void Charge(uint32_t bytes) noexcept {
        if (!unlimited_) {
            tokens_ -= static_cast<int64_t>(bytes) * 1000;
        }
    }

    /// Bytes saved up beyond one tick of rate: what a burst may use.
    uint64_t CapacityBytes() const noexcept {
        return capacity_milli_ / 1000;
    }

    /// Thousandths of a byte added by each `Refill`.
    uint64_t BytesPerTickMilli() const noexcept {
        return rate_milli_;
    }

private:
    bool unlimited_ = true;
    uint64_t rate_milli_ = 0;
    uint64_t capacity_milli_ = 0;
    uint64_t limit_milli_ = 0;
    int64_t tokens_ = 0;
};

} // namespace tessera::net
