// SPDX-License-Identifier: Apache-2.0
#include "tessera/schema/cli.hpp"
#include "test_support.hpp"

#include <span>
#include <sstream>
#include <string>

using tessera::schema::Run;

TESSERA_TEST(run_without_arguments_prints_usage_and_returns_2) {
    std::ostringstream out;
    std::ostringstream err;
    const int code = Run(std::span<const std::string>{}, out, err);
    TESSERA_CHECK(code == 2);
    TESSERA_CHECK(err.str().find("Usage:") != std::string::npos);
    TESSERA_CHECK(err.str().find("check") != std::string::npos);
    TESSERA_CHECK(err.str().find("hash") != std::string::npos);
    TESSERA_CHECK(err.str().find("generate") != std::string::npos);
}
