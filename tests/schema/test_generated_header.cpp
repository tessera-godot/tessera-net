// SPDX-License-Identifier: Apache-2.0
// The header that tessera_schema writes from fixtures/all_kinds.json, included and used. It is made while the
// tests are built (see tests/schema/CMakeLists.txt), so that what is tested is what the tool writes now.
#include "all_kinds.hpp"
#include "hand_written_example.hpp"
#include "risky_names.hpp"
#include "schema_test_support.hpp"
#include "tessera/net/entity_table.hpp"
#include "tessera/schema/columns.hpp"
#include "tessera/schema/schema_check.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

using namespace tessera::schema;
using schema_test::ReadTestFile;

// The header of the risky names is generated into a namespace with a part called tessera, which the
// generated code has to survive.
namespace rk = ::risky::tessera;

// Defined in test_generated_header_other_unit.cpp, which includes the same header.
bool OtherUnitAddsTheTypes(tessera::net::ReplicationSchema& schema);
uint64_t OtherUnitHash();

namespace {

SchemaFile ReadAllKinds() {
    const ReadResult read = ReadSchema(ReadTestFile("fixtures/all_kinds.json"));
    TESSERA_CHECK(read.file.has_value());
    return read.file ? *read.file : SchemaFile{};
}

} // namespace

TESSERA_TEST(generated_header_compiles_and_registers_the_types) {
    const SchemaFile file = ReadAllKinds();
    const CheckResult check = Check(file);
    TESSERA_CHECK(check.schema != nullptr);
    tessera::net::ReplicationSchema schema;
    std::string error;
    TESSERA_CHECK(all_kinds::AddGeneratedTypes(schema, &error));
    TESSERA_CHECK(error.empty());
    TESSERA_CHECK(all_kinds::kSchemaHash == check.hash);
    TESSERA_CHECK(schema.Hash() == all_kinds::kSchemaHash);
    TESSERA_CHECK(schema.Find(all_kinds::kAllKindsType) != nullptr &&
                  schema.Find(all_kinds::kLastType) != nullptr);
    // Where each column is, and how big its elements are, are what the checked schema said it would be.
    const tessera::net::TypeSchema* type = schema.Find(all_kinds::kAllKindsType);
    if (type == nullptr || type->properties.size() != 16 || file.types.empty()) {
        TESSERA_CHECK(false);
        return;
    }
    using C = all_kinds::AllKindsColumns;
    const struct {
        size_t property;
        size_t slot;
        size_t offset;
    } columns[] = {
        {0, 0, offsetof(C, position_x)}, {0, 1, offsetof(C, position_y)}, {0, 2, offsetof(C, position_z)},
        {1, 0, offsetof(C, yaw)},        {2, 0, offsetof(C, u8)},         {3, 0, offsetof(C, u16)},
        {4, 0, offsetof(C, u32)},        {5, 0, offsetof(C, u64)},        {6, 0, offsetof(C, s8)},
        {7, 0, offsetof(C, s16)},        {8, 0, offsetof(C, s32)},        {9, 0, offsetof(C, s64)},
        {10, 0, offsetof(C, extreme)},   {11, 0, offsetof(C, alive)},     {12, 0, offsetof(C, mask8)},
        {13, 0, offsetof(C, mask40)},    {14, 0, offsetof(C, team)},      {15, 0, offsetof(C, species)},
    };
    for (const auto& column : columns) {
        const tessera::net::ColumnRef& ref = type->properties[column.property].source[column.slot];
        const PropertyDecl& decl = file.types[0].properties[column.property];
        TESSERA_CHECK(ref.offset == column.offset);
        TESSERA_CHECK(ref.element_bytes == ColumnFor(decl).bytes);
    }
}

TESSERA_TEST(generated_add_type_reports_an_error_when_the_type_exists) {
    tessera::net::ReplicationSchema schema;
    std::string error;
    TESSERA_CHECK(all_kinds::AddAllKindsType(schema, &error));
    TESSERA_CHECK(!all_kinds::AddAllKindsType(schema, &error));
    TESSERA_CHECK(error.find("type 1 already has a schema") != std::string::npos);
    // The group of them stops at the first that fails, and says why.
    error.clear();
    TESSERA_CHECK(!all_kinds::AddGeneratedTypes(schema, &error));
    TESSERA_CHECK(!error.empty());
    // Without a place for the message the functions still work.
    tessera::net::ReplicationSchema other;
    TESSERA_CHECK(all_kinds::AddGeneratedTypes(other));
    TESSERA_CHECK(!all_kinds::AddGeneratedTypes(other));
}

TESSERA_TEST(generated_per_type_functions_add_only_their_type) {
    tessera::net::ReplicationSchema schema;
    TESSERA_CHECK(all_kinds::AddLastType(schema));
    TESSERA_CHECK(schema.Find(all_kinds::kLastType) != nullptr &&
                  schema.Find(all_kinds::kAllKindsType) == nullptr);
    // The same schema, written by hand.
    struct HandColumns {
        uint8_t ready[tessera::net::kPageSlots];
    };
    tessera::net::ReplicationSchema by_hand;
    by_hand.AddType(255, {tessera::net::PropertyDescriptor::Bits(TESSERA_COLUMN(HandColumns, ready), 1)});
    TESSERA_CHECK(schema.Hash() == by_hand.Hash());
    // The fingerprint of the whole file is another number: a schema of only some of the types does not match
    // it.
    TESSERA_CHECK(schema.Hash() != all_kinds::kSchemaHash);
}

TESSERA_TEST(generated_tables_carry_the_shard_and_the_type) {
    const std::unique_ptr<tessera::net::EntityTable<all_kinds::LastColumns>> table =
        all_kinds::MakeLastTable(7);
    TESSERA_CHECK(table != nullptr);
    if (!table) {
        return;
    }
    TESSERA_CHECK(table->Config().shard == 7 && table->Config().type == all_kinds::kLastType);
    const tessera::net::EntityId id = table->Create();
    TESSERA_CHECK(!id.IsNull() && id.shard() == 7 && id.type() == 255);
    const auto other = all_kinds::MakeAllKindsTable(4095);
    TESSERA_CHECK(other->Config().shard == 4095 && other->Config().type == 1);
    bool threw = false;
    try {
        all_kinds::MakeLastTable(4096);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    TESSERA_CHECK(threw);
}

TESSERA_TEST(generated_header_can_be_included_in_two_translation_units) {
    tessera::net::ReplicationSchema schema;
    TESSERA_CHECK(OtherUnitAddsTheTypes(schema));
    TESSERA_CHECK(OtherUnitHash() == all_kinds::kSchemaHash);
    TESSERA_CHECK(schema.Hash() == all_kinds::kSchemaHash);
}

TESSERA_TEST(generated_header_compiles_with_the_names_of_standard_things) {
    // Properties called size_t, std, tessera, net, byte, string, move and so on: they are columns of a
    // struct, and what is generated has to keep working with them (this file compiling is the test, and a few
    // uses make sure they are there).
    rk::RiskyColumns columns{};
    columns.size_t[0] = 1;
    columns.std[1] = 1;
    columns.tessera[2] = 1;
    columns.net[3] = 1;
    columns.string[4] = 1;
    columns.move[5] = 1;
    columns.position_x[6] = 1.0f;
    TESSERA_CHECK(columns.size_t[0] == 1 && columns.std[1] == 1 && columns.string[4] == 1 &&
                  columns.position_x[6] == 1.0f);
    tessera::net::ReplicationSchema schema;
    std::string error;
    TESSERA_CHECK(rk::AddGeneratedTypes(schema, &error));
    TESSERA_CHECK(error.empty());
    TESSERA_CHECK(rk::MakeRiskyTable(1)->Config().type == 3);
}
