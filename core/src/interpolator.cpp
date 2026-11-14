// SPDX-License-Identifier: Apache-2.0
#include "tessera/net/interpolator.hpp"

#include "tessera/net/replication/property_codecs.hpp"

#include <cmath>
#include <cstring>
#include <new>
#include <utility>

namespace tessera::net {

namespace {

constexpr size_t kTypeCount = 256;
constexpr double kTwoPi = 6.283185307179586476925286766559;

void Describe(std::string* error, const char* text) {
    if (error != nullptr) {
        *error = text;
    }
}

} // namespace

Interpolator::Interpolator(const ReplicationSchema& schema) : schema_(schema) {
}

bool Interpolator::Configure(const InterpolationConfig& config, std::string* error) {
    if (config.delay_ticks > kMaxDelayTicks) {
        Describe(error, "the interpolation delay must be from 0 to 30 ticks");
        return false;
    }
    if (!std::isfinite(config.snap_distance) || config.snap_distance <= 0.0) {
        Describe(error, "the snap distance must be a number above 0");
        return false;
    }
    if (config.max_gap_ticks < 1 || config.max_gap_ticks > 255) {
        Describe(error, "the gap limit must be from 1 to 255 ticks");
        return false;
    }
    std::vector<Pool> pools(kTypeCount);
    pools_ = std::move(pools);
    std::vector<Entity>().swap(entities_);
    config_ = config;
    stats_ = InterpolationStats{};
    ResetClock();
    enabled_ = true;
    return true;
}

void Interpolator::Clear() noexcept {
    enabled_ = false;
    config_ = InterpolationConfig{};
    stats_ = InterpolationStats{};
    ResetClock();
    std::vector<Entity>().swap(entities_);
    std::vector<Pool>().swap(pools_);
}

void Interpolator::Reset(uint32_t max_visible) {
    if (!enabled_) {
        return;
    }
    stats_ = InterpolationStats{};
    ResetClock();
    for (Pool& pool : pools_) {
        pool = Pool{};
    }
    if (max_visible == 0) {
        std::vector<Entity>().swap(entities_);
    } else {
        entities_.assign(max_visible, Entity{});
    }
}

Interpolator::View Interpolator::Locate(const Entity& entity) const noexcept {
    const Pool& pool = pools_[entity.type->type];
    const Block& block = pool.blocks[entity.chunk / kChunksPerBlock];
    const size_t within = entity.chunk % kChunksPerBlock;
    const size_t capacity = Capacity();
    return View{block.ticks.get() + within * capacity,
                block.words.get() + within * capacity * entity.type->words,
                block.links.get() + within * capacity};
}

bool Interpolator::GrowPool(Pool& pool, uint32_t words) {
    const size_t states = static_cast<size_t>(Capacity()) * kChunksPerBlock;
    try {
        Block block;
        block.ticks = std::make_unique<uint64_t[]>(states);
        block.words = std::make_unique<uint32_t[]>(states * words);
        block.links = std::make_unique<uint8_t[]>(states);
        const size_t first = pool.blocks.size() * kChunksPerBlock;
        pool.blocks.reserve(pool.blocks.size() + 1);
        // Everything that can be free at once has room in the list, so giving a chunk back never allocates.
        pool.free.reserve(first + kChunksPerBlock);
        pool.blocks.push_back(std::move(block));
        for (size_t i = kChunksPerBlock; i > 0; --i) {
            pool.free.push_back(static_cast<uint32_t>(first + i - 1));
        }
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

void Interpolator::Release(Entity& entity) noexcept {
    if (entity.chunk != kNoChunk) {
        pools_[entity.type->type].free.push_back(entity.chunk); // within the room GrowPool made
    }
    entity = Entity{};
}

bool Interpolator::OnEnter(uint32_t compact_id, uint32_t type) {
    if (!enabled_ || compact_id >= entities_.size()) {
        return false;
    }
    const TypeSchema* schema = schema_.Find(type);
    if (schema == nullptr) {
        return false;
    }
    Entity& entity = entities_[compact_id];
    Release(entity);
    Pool& pool = pools_[type];
    if (pool.free.empty() && !GrowPool(pool, schema->words)) {
        return false;
    }
    entity.type = schema;
    entity.chunk = pool.free.back();
    entity.count = 0;
    pool.free.pop_back();
    return true;
}

void Interpolator::OnLeave(uint32_t compact_id) noexcept {
    if (enabled_ && compact_id < entities_.size()) {
        Release(entities_[compact_id]);
    }
}

// Whether two neighbouring states are blended: they are not too far apart in ticks, nor in space (the
// distance counts every position of the type together). A type with nothing to blend has nothing to hold.
bool Interpolator::Link(const TypeSchema& type, uint64_t before_tick, const uint32_t* before, uint64_t tick,
                        const uint32_t* words, bool* gap_failed) const noexcept {
    *gap_failed = false;
    bool blends = false;
    double squared = 0.0;
    const bool in_gap = tick - before_tick <= config_.max_gap_ticks;
    for (const PropertyDescriptor& property : type.properties) {
        if (property.kind == PropertyKind::Angle) {
            blends = true;
        } else if (property.kind == PropertyKind::Position) {
            blends = true;
            if (in_gap) {
                double a[3];
                double b[3];
                DequantizePosition(property, before + property.word_offset, a);
                DequantizePosition(property, words + property.word_offset, b);
                for (int axis = 0; axis < 3; ++axis) {
                    squared += (b[axis] - a[axis]) * (b[axis] - a[axis]);
                }
            }
        }
    }
    if (!blends) {
        return true;
    }
    if (!in_gap) {
        *gap_failed = true;
        return false;
    }
    return squared <= config_.snap_distance * config_.snap_distance;
}

void Interpolator::OnRecord(uint32_t compact_id, uint64_t tick, const uint32_t* words) {
    if (!enabled_) {
        return;
    }
    if (compact_id >= entities_.size() || entities_[compact_id].chunk == kNoChunk || words == nullptr) {
        ++stats_.states_ignored;
        return;
    }
    Entity& entity = entities_[compact_id];
    const View view = Locate(entity);
    const uint32_t capacity = Capacity();
    const size_t width = entity.type->words;
    uint32_t count = entity.count;
    uint32_t at = count;
    while (at > 0 && view.ticks[at - 1] > tick) {
        --at;
    }
    if (at > 0 && view.ticks[at - 1] == tick) {
        ++stats_.states_ignored; // a state of that tick is already kept
        return;
    }
    if (count == capacity) {
        if (at == 0) {
            ++stats_.states_ignored; // older than everything kept
            return;
        }
        // The oldest leaves.
        std::memmove(view.ticks, view.ticks + 1, (count - 1) * sizeof(uint64_t));
        std::memmove(view.words, view.words + width, (count - 1) * width * sizeof(uint32_t));
        std::memmove(view.links, view.links + 1, count - 1);
        --count;
        --at;
    }
    const uint32_t tail = count - at;
    std::memmove(view.ticks + at + 1, view.ticks + at, tail * sizeof(uint64_t));
    std::memmove(view.words + (at + 1) * width, view.words + at * width, tail * width * sizeof(uint32_t));
    std::memmove(view.links + at + 1, view.links + at, tail);
    view.ticks[at] = tick;
    std::memcpy(view.words + at * width, words, width * sizeof(uint32_t));
    view.links[at] = 0;
    ++count;
    entity.count = count;
    ++stats_.states;

    if (at > 0) {
        bool gap_failed = false;
        const bool linked = Link(*entity.type, view.ticks[at - 1], view.words + (at - 1) * width, tick,
                                 view.words + at * width, &gap_failed);
        view.links[at] = linked ? 1 : 0;
        if (!linked && at + 1 == count) { // only a state that arrives in order is counted
            ++(gap_failed ? stats_.gap_holds : stats_.snaps);
        }
    }
    if (at + 1 < count) {
        bool ignored = false;
        view.links[at + 1] = Link(*entity.type, tick, view.words + at * width, view.ticks[at + 1],
                                  view.words + (at + 1) * width, &ignored)
                                 ? 1
                                 : 0;
    }
}

// Where the shown time wants to be: the delay behind the newest tick, and never before the start.
double Interpolator::Target() const noexcept {
    return latest_ > config_.delay_ticks ? static_cast<double>(latest_ - config_.delay_ticks) : 0.0;
}

void Interpolator::ResetClock() noexcept {
    running_ = false;
    display_ = 0.0;
    latest_ = 0;
}

void Interpolator::OnBuffer(uint64_t tick) noexcept {
    if (!enabled_) {
        return;
    }
    if (!running_) {
        running_ = true;
        latest_ = tick;
        display_ = Target();
        return;
    }
    if (tick <= latest_) {
        return; // older than what the clock has seen, or a repeat
    }
    latest_ = tick;
    const double target = Target();
    if (display_ - target < -kResyncTicks) {
        // Too far behind to catch up by running a little faster (the network stopped and came back, or a
        // much newer tick arrived): go to the target instead of replaying what was missed.
        display_ = target;
        ++stats_.resyncs;
    }
}

void Interpolator::Advance(double ticks) noexcept {
    if (!enabled_ || !running_ || !std::isfinite(ticks) || !(ticks > 0.0)) {
        return;
    }
    const double error = display_ - Target();
    double rate = 1.0;
    if (error > kBandTicks) {
        rate = 1.0 - kRateAdjust; // ahead: give back the slack that a late network ate
    } else if (error < -kBandTicks) {
        rate = 1.0 + kRateAdjust; // behind: catch up
    }
    display_ += ticks * rate;
    const double newest = static_cast<double>(latest_);
    if (display_ > newest) {
        display_ = newest; // what has not arrived cannot be shown
        ++stats_.stalls;
    }
}

// The index of the first state with a tick after the shown time (`count` if there is none).
uint32_t Interpolator::After(const View& view, uint32_t count) const noexcept {
    uint32_t index = count;
    while (index > 0 && static_cast<double>(view.ticks[index - 1]) > display_) {
        --index;
    }
    return index;
}

const uint32_t* Interpolator::HeldWords(uint32_t compact_id) const noexcept {
    if (!running_ || StateCount(compact_id) == 0) {
        return nullptr;
    }
    const Entity& entity = entities_[compact_id];
    const View view = Locate(entity);
    const uint32_t after = After(view, entity.count);
    return view.words + static_cast<size_t>(after > 0 ? after - 1 : 0) * entity.type->words;
}

bool Interpolator::Read(uint32_t compact_id, uint32_t property_index, std::span<double> out) const noexcept {
    if (!running_ || StateCount(compact_id) == 0) {
        return false;
    }
    const Entity& entity = entities_[compact_id];
    if (property_index >= entity.type->properties.size()) {
        return false;
    }
    const PropertyDescriptor& property = entity.type->properties[property_index];
    if (property.kind == PropertyKind::Position ? out.size() < 3
                                                : (property.kind != PropertyKind::Angle || out.empty())) {
        return false;
    }
    const View view = Locate(entity);
    const size_t width = entity.type->words;
    const uint32_t after = After(view, entity.count);
    // The state to show, and the one to blend it with (the same one when nothing is blended).
    uint32_t from = after > 0 ? after - 1 : 0;
    uint32_t to = from;
    double fraction = 0.0;
    if (after > 0 && after < entity.count && view.links[after] != 0) {
        to = after;
        fraction = (display_ - static_cast<double>(view.ticks[from])) /
                   static_cast<double>(view.ticks[to] - view.ticks[from]);
    }
    const uint32_t* a = view.words + from * width + property.word_offset;
    const uint32_t* b = view.words + to * width + property.word_offset;
    if (property.kind == PropertyKind::Position) {
        double start[3];
        double end[3];
        DequantizePosition(property, a, start);
        DequantizePosition(property, b, end);
        for (int axis = 0; axis < 3; ++axis) {
            out[axis] = start[axis] + (end[axis] - start[axis]) * fraction;
        }
        return true;
    }
    const double start = DequantizeAngle(property, a);
    double turn = DequantizeAngle(property, b) - start;
    turn -= kTwoPi * std::round(turn / kTwoPi); // the short way round: into (-pi, pi]
    double angle = std::fmod(start + turn * fraction, kTwoPi);
    if (angle < 0.0) {
        angle += kTwoPi;
    }
    out[0] = angle < kTwoPi ? angle : 0.0;
    return true;
}

size_t Interpolator::StateCount(uint32_t compact_id) const noexcept {
    if (compact_id >= entities_.size() || entities_[compact_id].chunk == kNoChunk) {
        return 0;
    }
    return entities_[compact_id].count;
}

uint64_t Interpolator::StateTick(uint32_t compact_id, uint32_t index) const noexcept {
    if (index >= StateCount(compact_id)) {
        return 0;
    }
    return Locate(entities_[compact_id]).ticks[index];
}

const uint32_t* Interpolator::StateWords(uint32_t compact_id, uint32_t index) const noexcept {
    if (index >= StateCount(compact_id)) {
        return nullptr;
    }
    const Entity& entity = entities_[compact_id];
    return Locate(entity).words + static_cast<size_t>(index) * entity.type->words;
}

bool Interpolator::Linked(uint32_t compact_id, uint32_t index) const noexcept {
    if (index == 0 || index >= StateCount(compact_id)) {
        return false;
    }
    return Locate(entities_[compact_id]).links[index] != 0;
}

size_t Interpolator::MemoryBytes() const noexcept {
    if (!enabled_) {
        return 0;
    }
    size_t bytes = entities_.capacity() * sizeof(Entity) + pools_.capacity() * sizeof(Pool);
    for (size_t type = 0; type < pools_.size(); ++type) {
        const Pool& pool = pools_[type];
        bytes += pool.blocks.capacity() * sizeof(Block) + pool.free.capacity() * sizeof(uint32_t);
        if (!pool.blocks.empty()) {
            const TypeSchema* schema = schema_.Find(static_cast<uint32_t>(type));
            const size_t words = schema != nullptr ? schema->words : 0;
            const size_t per_state = sizeof(uint64_t) + words * sizeof(uint32_t) + 1;
            bytes += pool.blocks.size() * kChunksPerBlock * Capacity() * per_state;
        }
    }
    return bytes;
}

} // namespace tessera::net
