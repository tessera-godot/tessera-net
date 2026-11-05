// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string_view>

namespace tessera::net {

/// Semantic version of the Tessera Net core library.
struct Version {
    int major;
    int minor;
    int patch;
};

/// Returns the version this library was built as.
Version GetVersion() noexcept;

/// Returns the version formatted as "major.minor.patch".
std::string_view GetVersionString() noexcept;

}  // namespace tessera::net
