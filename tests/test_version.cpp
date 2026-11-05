// SPDX-License-Identifier: Apache-2.0
#include <string>

#include "tessera/net/version.hpp"
#include "test_support.hpp"

TESSERA_TEST(version_string_matches_numeric_version) {
    const tessera::net::Version version = tessera::net::GetVersion();
    const std::string expected = std::to_string(version.major) + "." + std::to_string(version.minor) + "." +
                                 std::to_string(version.patch);
    TESSERA_CHECK(std::string(tessera::net::GetVersionString()) == expected);
}
