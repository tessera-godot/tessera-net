// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/type_registry.hpp"
#include "test_support.hpp"

using tessera::net::TypeRegistry;

TESSERA_TEST(register_and_lookup_name) {
    TypeRegistry registry;
    TESSERA_CHECK(registry.Register(7, "zombie"));
    const std::string* name = registry.NameOf(7);
    TESSERA_CHECK(name != nullptr);
    TESSERA_CHECK(name != nullptr && *name == "zombie");
    TESSERA_CHECK(registry.Count() == 1);
}

TESSERA_TEST(duplicate_type_is_rejected) {
    TypeRegistry registry;
    TESSERA_CHECK(registry.Register(1, "player"));
    TESSERA_CHECK(!registry.Register(1, "other"));
    TESSERA_CHECK(registry.Count() == 1);
}

TESSERA_TEST(duplicate_name_is_rejected) {
    TypeRegistry registry;
    TESSERA_CHECK(registry.Register(1, "player"));
    TESSERA_CHECK(!registry.Register(2, "player"));
    TESSERA_CHECK(registry.Count() == 1);
}

TESSERA_TEST(type_above_255_is_rejected) {
    TypeRegistry registry;
    TESSERA_CHECK(registry.Register(255, "last"));
    TESSERA_CHECK(!registry.Register(256, "too_big"));
    TESSERA_CHECK(registry.Count() == 1);
}

TESSERA_TEST(unknown_type_has_no_name) {
    TypeRegistry registry;
    TESSERA_CHECK(registry.NameOf(9) == nullptr);
    TESSERA_CHECK(registry.NameOf(1000) == nullptr);
}
