// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdio>
#include <functional>
#include <utility>
#include <vector>

namespace tessera_test {

struct TestCase {
    const char* name;
    std::function<void()> body;
};

/// Registry filled by the TESSERA_TEST macro before main() runs.
inline std::vector<TestCase>& Registry() {
    static std::vector<TestCase> registry;
    return registry;
}

inline int& FailureCount() {
    static int failures = 0;
    return failures;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> body) {
        Registry().push_back({name, std::move(body)});
    }
};

}  // namespace tessera_test

#define TESSERA_TEST(test_name)                                                        \
    static void test_name();                                                           \
    static const tessera_test::Registrar registrar_##test_name{#test_name, test_name}; \
    static void test_name()

#define TESSERA_CHECK(condition)                                                 \
    do {                                                                         \
        if (!(condition)) {                                                      \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++tessera_test::FailureCount();                                      \
        }                                                                        \
    } while (false)
