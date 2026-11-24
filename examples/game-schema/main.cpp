// SPDX-License-Identifier: Apache-2.0
// What a game server does with the code that tessera_schema makes from schema.json: describes the types,
// makes a table and creates an entity in it.
#include "game_schema.hpp"

#include <cstdio>
#include <string>

int main() {
    tessera::net::ReplicationSchema schema;
    std::string error;
    if (!game::AddGeneratedTypes(schema, &error)) {
        std::fprintf(stderr, "the schema was refused: %s\n", error.c_str());
        return 1;
    }

    // The table of the monsters, for shard 1. Every id it makes carries the shard and the type.
    const auto monsters = game::MakeMonsterTable(1);
    const tessera::net::EntityId id = monsters->Create();
    const auto where = monsters->Find(id);
    if (!where) {
        std::fprintf(stderr, "the monster was not found in its table\n");
        return 1;
    }
    auto& columns = monsters->Columns(where->page);
    columns.health[where->slot] = 100;
    monsters->MarkDirty(id);

    // The hash of the schema made here is the one in the generated header (and in the GDScript the client
    // uses).
    std::printf("one monster, schema hash 0x%016llx\n", static_cast<unsigned long long>(schema.Hash()));
    return schema.Hash() == game::kSchemaHash ? 0 : 2;
}
