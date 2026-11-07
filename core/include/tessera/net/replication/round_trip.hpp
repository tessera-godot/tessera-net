// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstdint>

namespace tessera::net {

/// How long a connection takes to confirm a buffer, in ticks, as a moving average, and from it how long to
/// wait before sending an unconfirmed entity again. Waiting less than a round trip only repeats data that is
/// already on its way back.
///
/// The average is kept in sixteenths of a tick. Each new sample moves it an eighth of the way. A sample is
/// cut to the largest wait before it is used, so one absurd value (a confirmation that sat in a queue) cannot
/// drag the average far.
class RoundTripEstimator {
public:
    /// One tick everywhere: an entity that is not confirmed is sent again on the next tick.
    RoundTripEstimator() = default;

    /// Wait `round trip + margin_ticks`, never less than `min_ticks` or more than `max_ticks`;
    /// `initial_ticks` until the first sample.
    RoundTripEstimator(uint32_t margin_ticks, uint32_t min_ticks, uint32_t max_ticks, uint32_t initial_ticks)
        : margin_(margin_ticks), min_(min_ticks), max_(max_ticks), initial_(initial_ticks) {}

    /// A confirmation came `sample_ticks` ticks after the buffer it confirms was written.
    void Note(uint32_t sample_ticks) noexcept {
        const int64_t sample = static_cast<int64_t>(std::min(sample_ticks, max_)) * 16;
        mean16_ = samples_ == 0 ? sample : mean16_ + (sample - mean16_) / 8;
        ++samples_;
    }

    /// Ticks to wait before sending an entity again.
    uint32_t ResendAfter() const noexcept {
        if (samples_ == 0) {
            return initial_;
        }
        const uint32_t rounded_up = static_cast<uint32_t>((mean16_ + 15) / 16);
        return std::clamp(rounded_up + margin_, min_, max_);
    }

    uint32_t Samples() const noexcept {
        return samples_;
    }

    /// The average round trip, in ticks.
    double MeanTicks() const noexcept {
        return static_cast<double>(mean16_) / 16.0;
    }

private:
    uint32_t margin_ = 0;
    uint32_t min_ = 1;
    uint32_t max_ = 30;
    uint32_t initial_ = 1;
    int64_t mean16_ = 0;
    uint32_t samples_ = 0;
};

} // namespace tessera::net
