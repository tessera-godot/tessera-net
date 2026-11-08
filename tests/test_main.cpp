// SPDX-License-Identifier: Apache-2.0
#include "test_support.hpp"

#include <cstdio>

int main() {
    for (const auto& test : tessera_test::Registry()) {
        std::printf("[ RUN ] %s\n", test.name);
        std::fflush(stdout); // so that a crash names the test that caused it
        test.body();
    }
    const int failures = tessera_test::FailureCount();
    std::printf("%zu tests, %d failures\n", tessera_test::Registry().size(), failures);
    return failures == 0 ? 0 : 1;
}
