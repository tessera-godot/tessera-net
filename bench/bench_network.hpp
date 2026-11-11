// SPDX-License-Identifier: Apache-2.0
#pragma once

/// Runs the network criteria and prints each one (see bench_report.hpp). Called from main().
void CheckNetwork();

/// Measures how the time of the network phase grows with the entities that move in a tick, and fits a line to
/// it. Only runs when asked for (`network-curve`); `reverse` plays the points from the busiest to the quietest.
void CheckNetworkCurve(bool reverse);
