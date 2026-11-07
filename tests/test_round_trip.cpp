// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/replication/round_trip.hpp"
#include "test_support.hpp"

#include <cstdint>

using namespace tessera::net;

TESSERA_TEST(estimator_uses_the_initial_value_before_any_sample) {
    RoundTripEstimator estimator(1, 2, 30, 6);
    TESSERA_CHECK(estimator.Samples() == 0);
    TESSERA_CHECK(estimator.ResendAfter() == 6);
}

TESSERA_TEST(estimator_first_sample_sets_the_mean) {
    RoundTripEstimator estimator(1, 2, 30, 6);
    estimator.Note(4);
    TESSERA_CHECK(estimator.Samples() == 1);
    TESSERA_CHECK(estimator.MeanTicks() == 4.0);
    TESSERA_CHECK(estimator.ResendAfter() == 5); // 4 ticks plus the margin of 1
}

TESSERA_TEST(estimator_follows_samples_with_weight_one_eighth) {
    RoundTripEstimator estimator(1, 2, 30, 6);
    estimator.Note(4);
    estimator.Note(12); // 4 + (12 - 4) / 8 = 5
    TESSERA_CHECK(estimator.MeanTicks() == 5.0);
    TESSERA_CHECK(estimator.ResendAfter() == 6);
    for (int i = 0; i < 200; ++i) {
        estimator.Note(12);
    }
    TESSERA_CHECK(estimator.MeanTicks() > 11.0 && estimator.MeanTicks() <= 12.0); // it converges
}

TESSERA_TEST(estimator_clamps_to_min_and_max) {
    RoundTripEstimator low(0, 2, 30, 6);
    low.Note(0);
    TESSERA_CHECK(low.ResendAfter() == 2); // never below the minimum
    RoundTripEstimator high(1, 2, 30, 6);
    high.Note(1'000'000); // a huge sample is cut to the maximum before it enters the mean
    TESSERA_CHECK(high.MeanTicks() == 30.0);
    TESSERA_CHECK(high.ResendAfter() == 30);
}

TESSERA_TEST(estimator_rounds_up) {
    RoundTripEstimator estimator(1, 1, 30, 1);
    estimator.Note(4);
    estimator.Note(5); // 4 + 1/8: a hair above 4 ticks
    TESSERA_CHECK(estimator.MeanTicks() > 4.0 && estimator.MeanTicks() < 4.2);
    TESSERA_CHECK(estimator.ResendAfter() == 6); // 5 rounded up, plus the margin
}

TESSERA_TEST(estimator_default_is_one_tick_everywhere) {
    RoundTripEstimator estimator;
    TESSERA_CHECK(estimator.ResendAfter() == 1);
    estimator.Note(0);
    TESSERA_CHECK(estimator.ResendAfter() == 1);
}
