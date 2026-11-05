// SPDX-License-Identifier: Apache-2.0
// Compares keeping entities as Godot nodes with keeping them in a Tessera Net entity table.
// Both sides run in the same process, on the same data and with the same arithmetic.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include "tessera/net/entity_table.hpp"

using namespace godot;
namespace tn = tessera::net;

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kPasses = 20;

struct Columns {
    float px[tn::kPageSlots], py[tn::kPageSlots], pz[tn::kPageSlots];
    float vx[tn::kPageSlots], vy[tn::kPageSlots], vz[tn::kPageSlots];
    float health[tn::kPageSlots];
};

double Ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

double Median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

}  // namespace

class TesseraNodeBench : public RefCounted {
    GDCLASS(TesseraNodeBench, RefCounted)

protected:
    static void _bind_methods() {
        ClassDB::bind_method(D_METHOD("run", "count", "runs"), &TesseraNodeBench::run);
    }

public:
    // Returns the median time in milliseconds of each operation over `runs` repetitions.
    Dictionary run(int64_t count, int64_t runs) const {
        std::vector<double> node_create, node_iterate, node_destroy;
        std::vector<double> table_create, table_iterate, table_destroy;
        for (int64_t run = 0; run < runs; ++run) {
            {
                std::vector<Node3D*> nodes(static_cast<size_t>(count));
                const auto a = Clock::now();
                for (Node3D*& node : nodes) {
                    node = memnew(Node3D);
                }
                const auto b = Clock::now();
                for (int pass = 0; pass < kPasses; ++pass) {
                    for (Node3D* node : nodes) {
                        node->set_position(node->get_position() + Vector3(1.0f, 2.0f, 3.0f) * 0.033f);
                    }
                }
                const auto c = Clock::now();
                for (Node3D* node : nodes) {
                    memdelete(node);
                }
                const auto d = Clock::now();
                node_create.push_back(Ms(a, b));
                node_iterate.push_back(Ms(b, c) / kPasses);
                node_destroy.push_back(Ms(c, d));
            }
            {
                tn::EntityTable<Columns> table(tn::TableConfig{});
                std::vector<tn::EntityId> ids(static_cast<size_t>(count), tn::EntityId::Null());
                const auto a = Clock::now();
                for (tn::EntityId& id : ids) {
                    id = table.Create();
                }
                const auto b = Clock::now();
                for (int pass = 0; pass < kPasses; ++pass) {
                    for (uint32_t page = 0; page < table.PageCount(); ++page) {
                        Columns& c = table.Columns(page);
                        table.ForEachAliveInPage(page, [&](uint32_t slot) {
                            c.px[slot] += 1.0f * 0.033f;
                            c.py[slot] += 2.0f * 0.033f;
                            c.pz[slot] += 3.0f * 0.033f;
                        });
                    }
                }
                const auto c = Clock::now();
                for (const tn::EntityId id : ids) {
                    table.Destroy(id);
                }
                const auto d = Clock::now();
                table_create.push_back(Ms(a, b));
                table_iterate.push_back(Ms(b, c) / kPasses);
                table_destroy.push_back(Ms(c, d));
            }
        }
        Dictionary result;
        result["count"] = count;
        result["runs"] = runs;
        result["node_create_ms"] = Median(node_create);
        result["node_iterate_ms"] = Median(node_iterate);
        result["node_destroy_ms"] = Median(node_destroy);
        result["table_create_ms"] = Median(table_create);
        result["table_iterate_ms"] = Median(table_iterate);
        result["table_destroy_ms"] = Median(table_destroy);
        return result;
    }
};

void initialize_node_bench(ModuleInitializationLevel level) {
    if (level == MODULE_INITIALIZATION_LEVEL_SCENE) {
        GDREGISTER_CLASS(TesseraNodeBench);
    }
}

void uninitialize_node_bench(ModuleInitializationLevel) {}

extern "C" GDExtensionBool GDE_EXPORT node_bench_init(GDExtensionInterfaceGetProcAddress get_proc,
                                                      GDExtensionClassLibraryPtr library,
                                                      GDExtensionInitialization* initialization) {
    GDExtensionBinding::InitObject object(get_proc, library, initialization);
    object.register_initializer(initialize_node_bench);
    object.register_terminator(uninitialize_node_bench);
    object.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return object.init();
}
