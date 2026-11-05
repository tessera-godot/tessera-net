// SPDX-License-Identifier: Apache-2.0
#include <cstdio>

#include "test_support.hpp"

int main() {
    for (const auto& test : tessera_test::Registry()) {
        std::printf("[ RUN ] %s\n", test.name);
        test.body();
    }
    const int failures = tessera_test::FailureCount();
    std::printf("%zu tests, %d failures\n", tessera_test::Registry().size(), failures);
    return failures == 0 ? 0 : 1;
}
