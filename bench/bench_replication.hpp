// SPDX-License-Identifier: Apache-2.0
#pragma once

/// Runs the replicated state criteria and prints each one (see bench_report.hpp). Called from main().
void CheckReplication();

/// The time of one WriteRecord with its data in the processor cache, in nanoseconds. It does not depend on the
/// memory or the number of threads, so it shows how busy the machine is.
double WriteRecordIndicatorNs();
