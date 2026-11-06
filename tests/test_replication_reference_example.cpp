// SPDX-License-Identifier: Apache-2.0
// The body of this test is the example in docs/reference/replicated-state.md. If you change one, change the
// other.
#include "tessera/net/entity_table.hpp"
#include "tessera/net/job_system.hpp"
#include "tessera/net/replication/receiver_state.hpp"
#include "tessera/net/replication/replication_engine.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <span>

using namespace tessera::net;

TESSERA_TEST(replicated_state_reference_example) {
    // 1. The columns of one entity type, as its entity table keeps them.
    struct MobColumns {
        float x[kPageSlots];
        float y[kPageSlots];
        float z[kPageSlots];
        float yaw[kPageSlots];
        int32_t health[kPageSlots];
    };

    // 2. Say which properties of type 1 are replicated, and how precisely.
    ReplicationSchema schema;
    schema.AddType(
        1, {PropertyDescriptor::Position(TESSERA_COLUMN(MobColumns, x), TESSERA_COLUMN(MobColumns, y),
                                         TESSERA_COLUMN(MobColumns, z), 0.01f,
                                         5000.0f), // 1 cm steps, up to 5 km from the origin
            PropertyDescriptor::Angle(TESSERA_COLUMN(MobColumns, yaw), 10), // 1,024 steps around the circle
            PropertyDescriptor::Integer(TESSERA_COLUMN(MobColumns, health), true, 0, 1000)});

    // 3. The engine, a table for the entities, and one connected player.
    ThreadPoolJobSystem jobs(ThreadPoolJobSystem::DefaultWorkerCount());
    ReplicationConfig config;
    config.max_connections = 8;
    config.max_visible_per_connection = 1024;
    ReplicationEngine engine(schema, config, jobs);
    EntityTable<MobColumns> table(TableConfig{0, 1});
    engine.AddConnection(0);

    // 4. Create an entity, write its columns, and flag it as changed.
    const EntityId mob = table.Create();
    const auto where = table.Find(mob);
    MobColumns& columns = table.Columns(where->page);
    columns.x[where->slot] = 120.5f;
    columns.y[where->slot] = 0.0f;
    columns.z[where->slot] = -40.25f;
    columns.yaw[where->slot] = 1.0f;
    columns.health[where->slot] = 80;
    table.MarkDirty(mob);

    // 5. Every tick, in the Network phase: quantize what changed, tell the engine what each player can see
    //    (these events come from the relevancy engine), and write a buffer for each player.
    const uint64_t tick = 1;
    engine.Refresh(table);
    const VisibilityEvent enter{VisibilityEvent::Kind::Enter, mob, 0};
    engine.ApplyVisibility(0, std::span<const VisibilityEvent>(&enter, 1));
    engine.Update(tick);
    const std::span<const uint8_t> buffer = engine.Output(0); // send this to the player

    // 6. On the player's side, a receiver turns the buffer back into state.
    ReceiverState receiver(schema, 1024, config.max_baseline_age_ticks);
    receiver.OnEnter(0, mob, tick); // the same Enter event, delivered reliably and in order
    receiver.AdvanceEvents(tick);
    const DecodeStatus status = receiver.Apply(buffer, tick);
    engine.Confirm(0, tick); // when the player confirms that the buffer arrived

    const uint32_t* quantized = receiver.Current(0); // the position, angle and health as whole numbers
    TESSERA_CHECK(status == DecodeStatus::Ok);
    TESSERA_CHECK(quantized != nullptr &&
                  quantized[0] == 512'050); // x = 120.5 m is 512,050 steps from -5,000 m
}
