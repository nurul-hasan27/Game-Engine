#include "engine/ecs/EntityManager.hpp"
#include "engine/input/ActionState.hpp"
#include "engine/ecs/Query.hpp"
#include "engine/ecs/System.hpp"
#include "engine/ecs/SystemManager.hpp"
#include "engine/input/Input.hpp"
#include "engine/math/Vec2.hpp"

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{

// ---------------------------------------------------------------------------
// Test-only components.
//
// These are deliberately plain data with no behaviour of any kind, which is the
// point: behaviour lives in the systems below. They are not part of the
// engine's gameplay design.
// ---------------------------------------------------------------------------

struct Position
{
    engine::Vec2 value;
};

struct Velocity
{
    engine::Vec2 value;
};

struct Health
{
    int value;
};

struct Label
{
    std::string value;
};

// ---------------------------------------------------------------------------
// Test-only systems.
// ---------------------------------------------------------------------------

/// The system from the design example: every entity holding a Position and a
/// Velocity gets its position advanced by its velocity.
class MovementSystem final : public engine::ecs::System
{
public:
    void update(engine::ecs::EntityManager& entities, const engine::input::ActionState& actions, const float deltaSeconds) override
    {
        (void)actions;
        (void)deltaSeconds; // this system advances by a fixed step, not by time
        for (auto&& [entity, position, velocity] : entities.query<Position, Velocity>())
        {
            (void)entity;
            position.value += velocity.value;
        }
    }

    [[nodiscard]] const char* name() const override { return "MovementSystem"; }
};

/// Reduces health, proving a second system can act on the same entity.
class HealthDecaySystem final : public engine::ecs::System
{
public:
    explicit HealthDecaySystem(int amount) : m_amount{amount} {}

    void update(engine::ecs::EntityManager& entities, const engine::input::ActionState& actions, const float deltaSeconds) override
    {
        (void)actions;
        (void)deltaSeconds; // deliberate: proves a system may ignore time
        for (auto&& [entity, health] : entities.query<Health>())
        {
            (void)entity;
            health.value -= m_amount;
        }
    }

    [[nodiscard]] const char* name() const override { return "HealthDecaySystem"; }

private:
    int m_amount;
};

/// Records the order systems actually run in, so ordering is testable rather
/// than assumed.
class RecordingSystem final : public engine::ecs::System
{
public:
    RecordingSystem(std::vector<std::string>& log, std::string label) : m_log{&log}, m_label{std::move(label)} {}

    void update(engine::ecs::EntityManager& entities, const engine::input::ActionState& actions, const float deltaSeconds) override
    {
        (void)entities;
        (void)actions;
        (void)deltaSeconds;
        m_log->push_back(m_label);
    }

    [[nodiscard]] const char* name() const override { return m_label.c_str(); }

private:
    std::vector<std::string>* m_log;
    std::string m_label;
};

/// Counts how many entities it visited, proving a query visits each once.
class CountingSystem final : public engine::ecs::System
{
public:
    explicit CountingSystem(std::size_t& count) : m_count{&count} {}

    void update(engine::ecs::EntityManager& entities, const engine::input::ActionState& actions, const float deltaSeconds) override
    {
        (void)actions;
        (void)deltaSeconds;
        for ([[maybe_unused]] auto&& entry : entities.query<Position, Velocity>())
        {
            ++(*m_count);
        }
    }

    [[nodiscard]] const char* name() const override { return "CountingSystem"; }

private:
    std::size_t* m_count;
};

/// Records the delta it was handed each frame, proving that timing reaches
/// systems and that every one of them sees the same value.
class DeltaRecordingSystem final : public engine::ecs::System
{
public:
    explicit DeltaRecordingSystem(std::vector<float>& deltas) : m_deltas{&deltas} {}

    void update(engine::ecs::EntityManager& entities, const engine::input::ActionState& actions, const float deltaSeconds) override
    {
        (void)entities;
        (void)actions;
        m_deltas->push_back(deltaSeconds);
    }

    [[nodiscard]] const char* name() const override { return "DeltaRecordingSystem"; }

private:
    std::vector<float>* m_deltas;
};

// ---------------------------------------------------------------------------
// Minimal harness, matching the style already used by the other test files.
// ---------------------------------------------------------------------------

int g_failureCount = 0;

void check(const bool condition, const char* const expression, const char* const file, const int line)
{
    if (!condition)
    {
        ++g_failureCount;
        std::cerr << "    " << file << ':' << line << ": CHECK(" << expression << ") failed\n";
    }
}

// Variadic so that an expression containing a comma, such as
// query<Position, Velocity>(), is treated as one argument. The preprocessor
// does not treat angle brackets as grouping.
#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)
#define CHECK_FALSE(...) check(!(__VA_ARGS__), "!" #__VA_ARGS__, __FILE__, __LINE__)

using engine::Vec2;
using engine::ecs::Entity;
using engine::ecs::EntityManager;
using engine::ecs::Query;
using engine::ecs::SystemManager;

/// The frame delta these tests pretend to be running at. Explicit values only:
/// nothing here sleeps or reads a clock, so the results are identical on every
/// machine. 0.016 s is roughly one 60 FPS frame.
constexpr float kTestDeltaSeconds = 0.016f;

template <typename View>
std::size_t countIn(const View& view)
{
    std::size_t count = 0;
    for ([[maybe_unused]] const auto& entry : view)
    {
        ++count;
    }
    return count;
}

// ---------------------------------------------------------------------------
// Compile-time guarantees, checked once.
// ---------------------------------------------------------------------------

// A query is a borrowed pointer and nothing else: it cannot be holding a
// collection of its own.
static_assert(sizeof(Query<false, Position>) == sizeof(void*),
              "a query must store only a borrowed pointer to the manager");
static_assert(std::is_trivially_copyable_v<Query<false, Position>>,
              "a query is a view, so copying it must be trivial");

// Duplicate component types are rejected by the query's own guard.
static_assert(engine::ecs::HasDuplicateTypes<int, int>::value, "duplicates must be detected");
static_assert(!engine::ecs::HasDuplicateTypes<int, float>::value, "distinct types must not be flagged");

// The two query flavours must differ in what they hand out.
using MutableQuery = Query<false, Position>;
using ReadOnlyQuery = Query<true, Position>;
static_assert(std::is_same_v<MutableQuery::Reference, std::tuple<Entity&, Position&>>,
              "a mutable query must yield a mutable entity and mutable components");
static_assert(std::is_same_v<ReadOnlyQuery::Reference, std::tuple<const Entity&, const Position&>>,
              "a const query must yield a const entity and const components");

// A const manager must not be able to hand out a mutable query.
static_assert(std::is_same_v<decltype(std::declval<const EntityManager&>().query<Position>()), ReadOnlyQuery>,
              "a const EntityManager must only produce read-only queries");
static_assert(std::is_same_v<decltype(std::declval<EntityManager&>().query<Position>()), MutableQuery>,
              "a mutable EntityManager must produce mutable queries");

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void testSingleComponentQuery()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::ActionState actions;
    Entity& first = manager.addEntity("first");
    Entity& second = manager.addEntity("second");
    Entity& without = manager.addEntity("third");
    without.addComponent<Label>(Label{"no position"});

    first.addComponent<Position>(Position{Vec2{1.0f, 2.0f}});
    second.addComponent<Position>(Position{Vec2{3.0f, 4.0f}});

    std::size_t seen = 0;
    for (auto&& [entity, position] : manager.query<Position>())
    {
        CHECK(entity.isAlive());
        position.value = Vec2(position.value.x * 10.0f, position.value.y * 10.0f);
        ++seen;
    }

    CHECK(seen == 2);
    CHECK(first.getComponent<Position>().value == Vec2(10.0f, 20.0f));
    CHECK(second.getComponent<Position>().value == Vec2(30.0f, 40.0f));
}

void testMultiComponentQuery()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::ActionState actions;
    Entity& entity = manager.addEntity("first");

    entity.addComponent<Position>(Position{Vec2{10.0f, 20.0f}});
    entity.addComponent<Velocity>(Velocity{Vec2{2.0f, -1.0f}});

    std::size_t seen = 0;
    for (auto&& [owner, position, velocity] : manager.query<Position, Velocity>())
    {
        CHECK(owner.id() == entity.id());
        position.value += velocity.value;
        ++seen;
    }

    CHECK(seen == 1);
    CHECK(entity.getComponent<Position>().value == Vec2(12.0f, 19.0f));
}

void testThreeComponentQuery()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::ActionState actions;
    Entity& entity = manager.addEntity("first");

    entity.addComponent<Position>(Position{Vec2{1.0f, 1.0f}});
    entity.addComponent<Velocity>(Velocity{Vec2{0.5f, 0.5f}});
    entity.addComponent<Health>(Health{10});

    std::size_t seen = 0;
    for (auto&& [owner, position, velocity, health] : manager.query<Position, Velocity, Health>())
    {
        (void)owner;
        position.value += velocity.value;
        health.value -= 1;
        ++seen;
    }

    CHECK(seen == 1);
    CHECK(entity.getComponent<Position>().value == Vec2(1.5f, 1.5f));
    CHECK(entity.getComponent<Health>().value == 9);
}

void testQueryExcludesMissingComponents()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::ActionState actions;
    Entity& onlyPosition = manager.addEntity("onlyPosition");
    Entity& onlyVelocity = manager.addEntity("onlyVelocity");
    Entity& both = manager.addEntity("both");
    Entity& neither = manager.addEntity("neither");

    onlyPosition.addComponent<Position>(Position{Vec2{1.0f, 0.0f}});
    onlyVelocity.addComponent<Velocity>(Velocity{Vec2{1.0f, 0.0f}});
    both.addComponent<Position>(Position{Vec2{2.0f, 0.0f}});
    both.addComponent<Velocity>(Velocity{Vec2{2.0f, 0.0f}});
    neither.addComponent<Label>(Label{"nothing useful"});

    // Only "both" has both components.
    CHECK(manager.query<Position>().size() == 2);
    CHECK(manager.query<Velocity>().size() == 2);
    CHECK(manager.query<Position, Velocity>().size() == 1);

    for (auto&& [entity, position, velocity] : manager.query<Position, Velocity>())
    {
        CHECK(entity.id() == both.id());
        CHECK(position.value == Vec2(2.0f, 0.0f));
        CHECK(velocity.value == Vec2(2.0f, 0.0f));
    }

    CHECK(manager.query<Position, Health>().empty());
    CHECK(manager.query<Health>().empty());
}

void testQueryExcludesDeadEntities()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::ActionState actions;
    Entity& alive = manager.addEntity("alive");
    Entity& doomed = manager.addEntity("doomed");

    alive.addComponent<Position>(Position{Vec2{1.0f, 1.0f}});
    alive.addComponent<Velocity>(Velocity{Vec2{1.0f, 1.0f}});
    doomed.addComponent<Position>(Position{Vec2{9.0f, 9.0f}});
    doomed.addComponent<Velocity>(Velocity{Vec2{9.0f, 9.0f}});

    CHECK(manager.query<Position, Velocity>().size() == 2);

    manager.destroyEntity(doomed);

    // Excluded as soon as it is flagged, without waiting for cleanup.
    CHECK(manager.query<Position, Velocity>().size() == 1);
    CHECK(manager.query<Position>().size() == 1);

    for (auto&& [entity, position, velocity] : manager.query<Position, Velocity>())
    {
        CHECK(entity.id() == alive.id());
        position.value += velocity.value;
    }

    // The dead entity's data was left untouched because it was never visited.
    CHECK(doomed.getComponent<Position>().value == Vec2(9.0f, 9.0f));
}

void testQueryEdgeCases()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::ActionState actions;

    // Zero entities at all.
    CHECK(manager.query<Position>().empty());
    CHECK(manager.query<Position>().size() == 0);
    CHECK(countIn(manager.query<Position>()) == 0);

    // Exactly one matching entity.
    Entity& single = manager.addEntity("single");
    single.addComponent<Position>(Position{Vec2{5.0f, 5.0f}});
    CHECK(manager.query<Position>().size() == 1);

    // Many matching entities.
    for (int i = 0; i < 32; ++i)
    {
        manager.addEntity("bulk").addComponent<Position>(Position{Vec2{1.0f, 1.0f}});
    }
    CHECK(manager.query<Position>().size() == 33);

    // No matching entities, because nothing carries Health.
    CHECK(manager.query<Health>().empty());
    CHECK(manager.query<Position, Health>().empty());
}

void testQueryVisitsEachEntityOnce()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::ActionState actions;
    for (int i = 0; i < 5; ++i)
    {
        Entity& entity = manager.addEntity("entity");
        entity.addComponent<Position>(Position{Vec2{0.0f, 0.0f}});
        entity.addComponent<Velocity>(Velocity{Vec2{1.0f, 1.0f}});
    }

    std::vector<engine::ecs::EntityId> seen;
    for (auto&& [entity, position, velocity] : manager.query<Position, Velocity>())
    {
        (void)position;
        (void)velocity;
        seen.push_back(entity.id());
    }

    CHECK(seen.size() == 5);
    for (std::size_t i = 1; i < seen.size(); ++i)
    {
        CHECK(seen[i] != seen[i - 1]);
    }
}

void testQueryDoesNotCopyComponents()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::ActionState actions;
    Entity& entity = manager.addEntity("entity");
    entity.addComponent<Position>(Position{Vec2{1.0f, 2.0f}});

    // The query must hand back the very same object the entity owns, not a copy.
    Position* const stored = entity.tryGetComponent<Position>();
    CHECK(stored != nullptr);

    bool sameObject = false;
    for (auto&& [owner, position] : manager.query<Position>())
    {
        (void)owner;
        sameObject = (&position == stored);
        position.value = Vec2(7.0f, 8.0f);
    }

    CHECK(sameObject);
    CHECK(entity.getComponent<Position>().value == Vec2(7.0f, 8.0f));
    CHECK(entity.componentCount() == 1);
}

void testConstQueryDoesNotAllowMutation()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::ActionState actions;
    Entity& entity = manager.addEntity("entity");
    entity.addComponent<Position>(Position{Vec2{1.0f, 2.0f}});
    entity.addComponent<Velocity>(Velocity{Vec2{5.0f, 5.0f}});

    const EntityManager& constManager = manager;

    std::size_t seen = 0;
    for (auto&& [owner, position, velocity] : constManager.query<Position, Velocity>())
    {
        (void)owner;
        (void)velocity;
        // Compile-time proof that the entity and the components arrived
        // read-only.
        static_assert(std::is_const_v<std::remove_reference_t<decltype(owner)>>,
                      "a const query must yield a const entity");
        static_assert(std::is_const_v<std::remove_reference_t<decltype(position)>>,
                      "a const query must yield const components");
        // Reading is fine.
        CHECK(position.value == Vec2(1.0f, 2.0f));
        ++seen;
    }

    CHECK(seen == 1);
    CHECK(entity.getComponent<Position>().value == Vec2(1.0f, 2.0f));
}

void testMovementSystem()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::ActionState actions;
    Entity& entity = manager.addEntity("entity");

    // The exact worked example from the design: (10, 20) plus (2, -1).
    entity.addComponent<Position>(Position{Vec2{10.0f, 20.0f}});
    entity.addComponent<Velocity>(Velocity{Vec2{2.0f, -1.0f}});

    SystemManager systems;
    systems.add<MovementSystem>();

    systems.update(manager, actions, kTestDeltaSeconds);

    CHECK(entity.getComponent<Position>().value == Vec2(12.0f, 19.0f));
    CHECK(entity.getComponent<Velocity>().value == Vec2(2.0f, -1.0f));

    // Running again keeps moving: the system is stateless behaviour, not a
    // one-shot transform.
    systems.update(manager, actions, kTestDeltaSeconds);
    CHECK(entity.getComponent<Position>().value == Vec2(14.0f, 18.0f));
}

void testMovementSystemAcrossManyEntities()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::ActionState actions;
    Entity& first = manager.addEntity("first");
    Entity& second = manager.addEntity("second");
    Entity& staticOnly = manager.addEntity("static");

    first.addComponent<Position>(Position{Vec2{0.0f, 0.0f}});
    first.addComponent<Velocity>(Velocity{Vec2{1.0f, 2.0f}});
    second.addComponent<Position>(Position{Vec2{10.0f, 10.0f}});
    second.addComponent<Velocity>(Velocity{Vec2{-1.0f, 0.0f}});
    staticOnly.addComponent<Position>(Position{Vec2{99.0f, 99.0f}});

    SystemManager systems;
    systems.add<MovementSystem>();
    systems.update(manager, actions, kTestDeltaSeconds);

    CHECK(first.getComponent<Position>().value == Vec2(1.0f, 2.0f));
    CHECK(second.getComponent<Position>().value == Vec2(9.0f, 10.0f));
    // An entity with no Velocity is not moved.
    CHECK(staticOnly.getComponent<Position>().value == Vec2(99.0f, 99.0f));
}

void testMultipleSystemsOnSameEntity()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::ActionState actions;
    Entity& entity = manager.addEntity("entity");
    entity.addComponent<Position>(Position{Vec2{0.0f, 0.0f}});
    entity.addComponent<Velocity>(Velocity{Vec2{3.0f, 4.0f}});
    entity.addComponent<Health>(Health{10});

    SystemManager systems;
    systems.add<MovementSystem>();
    systems.add<HealthDecaySystem>(2);

    systems.update(manager, actions, kTestDeltaSeconds);

    // Both systems acted on the same entity, independently.
    CHECK(entity.getComponent<Position>().value == Vec2(3.0f, 4.0f));
    CHECK(entity.getComponent<Health>().value == 8);

    systems.update(manager, actions, kTestDeltaSeconds);
    CHECK(entity.getComponent<Position>().value == Vec2(6.0f, 8.0f));
    CHECK(entity.getComponent<Health>().value == 6);
}

void testSystemExecutionOrderIsDeterministic()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::ActionState actions;
    std::vector<std::string> log;

    SystemManager systems;
    systems.add<RecordingSystem>(log, "InputSystem");
    systems.add<RecordingSystem>(log, "MovementSystem");
    systems.add<RecordingSystem>(log, "PhysicsSystem");
    systems.add<RecordingSystem>(log, "RenderSystem");

    CHECK(systems.systemCount() == 4);

    systems.update(manager, actions, kTestDeltaSeconds);

    CHECK(log.size() == 4);
    if (log.size() == 4)
    {
        CHECK(log[0] == "InputSystem");
        CHECK(log[1] == "MovementSystem");
        CHECK(log[2] == "PhysicsSystem");
        CHECK(log[3] == "RenderSystem");
    }

    // Repeating gives exactly the same order.
    log.clear();
    systems.update(manager, actions, kTestDeltaSeconds);
    CHECK(log.size() == 4);
    if (log.size() == 4)
    {
        CHECK(log[0] == "InputSystem");
        CHECK(log[3] == "RenderSystem");
    }

    // Registration order is preserved, not sorted by name.
    CHECK(std::string{systems.systemAt(0).name()} == "InputSystem");
    CHECK(std::string{systems.systemAt(3).name()} == "RenderSystem");
}

void testSystemQueryIsRepeatable()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::ActionState actions;
    for (int i = 0; i < 4; ++i)
    {
        Entity& entity = manager.addEntity("entity");
        entity.addComponent<Position>(Position{Vec2{0.0f, 0.0f}});
        entity.addComponent<Velocity>(Velocity{Vec2{1.0f, 1.0f}});
    }

    std::size_t visited = 0;
    SystemManager systems;
    systems.add<CountingSystem>(visited);
    systems.update(manager, actions, kTestDeltaSeconds);

    CHECK(visited == 4);

    // Running again is repeatable: a fresh query each frame, same result.
    systems.update(manager, actions, kTestDeltaSeconds);
    CHECK(visited == 8);
}

void testSystemsDoNotOwnEntities()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::ActionState actions;
    Entity& entity = manager.addEntity("entity");
    entity.addComponent<Position>(Position{Vec2{0.0f, 0.0f}});
    entity.addComponent<Velocity>(Velocity{Vec2{1.0f, 1.0f}});

    {
        SystemManager systems;
        systems.add<MovementSystem>();
        systems.update(manager, actions, kTestDeltaSeconds);
        CHECK(entity.getComponent<Position>().value == Vec2(1.0f, 1.0f));
    }

    // The SystemManager and its systems are gone; the entity and its components
    // are untouched, because the EntityManager owns them.
    CHECK(entity.isAlive());
    CHECK(entity.componentCount() == 2);
    CHECK(entity.getComponent<Position>().value == Vec2(1.0f, 1.0f));
    CHECK(manager.aliveEntityCount() == 1);

    // And the world still works without any systems at all.
    SystemManager later;
    later.add<MovementSystem>();
    later.update(manager, actions, kTestDeltaSeconds);
    CHECK(entity.getComponent<Position>().value == Vec2(2.0f, 2.0f));
}

void testAddSystemReferenceStaysValid()
{
    std::vector<std::string> log;
    SystemManager systems;

    MovementSystem& first = systems.add<MovementSystem>();
    RecordingSystem& second = systems.add<RecordingSystem>(log, "later");

    // Adding further systems must not move the ones already registered.
    systems.add<HealthDecaySystem>(1);

    CHECK(std::string{first.name()} == "MovementSystem");
    CHECK(std::string{second.name()} == "later");
    CHECK(systems.systemCount() == 3);
}

void testQueryAndSystemsWithNoEntities()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::ActionState actions;
    SystemManager systems;
    systems.add<MovementSystem>();
    systems.add<HealthDecaySystem>(5);

    // Running against an empty world must be harmless.
    systems.update(manager, actions, kTestDeltaSeconds);
    CHECK(manager.aliveEntityCount() == 0);
    CHECK(manager.query<Position, Velocity>().empty());
}

void testQueryIsRecomputedEachTime()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::ActionState actions;
    CHECK(manager.query<Position>().empty());

    // A query is a view, not a snapshot, so it reflects later changes.
    Entity& entity = manager.addEntity("entity");
    entity.addComponent<Position>(Position{Vec2{1.0f, 1.0f}});
    CHECK(manager.query<Position>().size() == 1);

    entity.removeComponent<Position>();
    CHECK(manager.query<Position>().empty());

    entity.addComponent<Position>(Position{Vec2{2.0f, 2.0f}});
    CHECK(manager.query<Position>().size() == 1);
    CHECK(entity.getComponent<Position>().value == Vec2(2.0f, 2.0f));
}

void testQueryCombinedWithTagsAndCleanup()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::ActionState actions;
    Entity& player = manager.addEntity("player");
    Entity& enemy = manager.addEntity("enemy");
    Entity& doomed = manager.addEntity("enemy");

    for (Entity* entity : {&player, &enemy, &doomed})
    {
        entity->addComponent<Position>(Position{Vec2{0.0f, 0.0f}});
        entity->addComponent<Velocity>(Velocity{Vec2{1.0f, 0.0f}});
    }

    manager.destroyEntity(doomed);
    manager.update();

    // Tag views and component queries agree once cleanup has run.
    CHECK(countIn(manager.getEntities("enemy")) == 1);
    CHECK(manager.query<Position, Velocity>().size() == 2);

    SystemManager systems;
    systems.add<MovementSystem>();
    systems.update(manager, actions, kTestDeltaSeconds);

    CHECK(player.getComponent<Position>().value == Vec2(1.0f, 0.0f));
    CHECK(enemy.getComponent<Position>().value == Vec2(1.0f, 0.0f));
}

void testSystemsReceiveDeltaSeconds()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::ActionState actions;
    std::vector<float> deltas;

    SystemManager systems;
    systems.add<DeltaRecordingSystem>(deltas);

    systems.update(manager, actions, 0.016f);

    // The system was handed exactly what the caller passed, unaltered.
    CHECK(deltas.size() == 1);
    if (deltas.size() == 1)
    {
        CHECK(deltas[0] == 0.016f);
    }

    deltas.clear();
    systems.update(manager, actions, 0.25f);
    CHECK(deltas.size() == 1);
    if (deltas.size() == 1)
    {
        CHECK(deltas[0] == 0.25f);
    }
}

void testAllSystemsReceiveTheSameDelta()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::ActionState actions;
    std::vector<float> first;
    std::vector<float> second;
    std::vector<float> third;

    SystemManager systems;
    systems.add<DeltaRecordingSystem>(first);
    systems.add<DeltaRecordingSystem>(second);
    systems.add<DeltaRecordingSystem>(third);

    systems.update(manager, actions, 0.033f);

    CHECK(first.size() == 1);
    CHECK(second.size() == 1);
    CHECK(third.size() == 1);

    if (first.size() == 1 && second.size() == 1 && third.size() == 1)
    {
        // One frame, one delta, every system, unchanged.
        CHECK(first[0] == 0.033f);
        CHECK(second[0] == first[0]);
        CHECK(third[0] == first[0]);
    }
}

void testDeltaReachesSystemsInRegistrationOrder()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::ActionState actions;
    std::vector<float> deltas;
    std::vector<std::string> order;

    SystemManager systems;
    systems.add<RecordingSystem>(order, "first");
    systems.add<DeltaRecordingSystem>(deltas);
    systems.add<RecordingSystem>(order, "third");

    systems.update(manager, actions, 0.05f);

    // Adding the delta recorder between two loggers must not disturb the order.
    CHECK(order.size() == 2);
    if (order.size() == 2)
    {
        CHECK(order[0] == "first");
        CHECK(order[1] == "third");
    }
    CHECK(deltas.size() == 1);
    if (deltas.size() == 1)
    {
        CHECK(deltas[0] == 0.05f);
    }
}

} // namespace

int main()
{
    const std::pair<const char*, void (*)()> testCases[] = {
        {"single component query", &testSingleComponentQuery},
        {"multi component query", &testMultiComponentQuery},
        {"three component query", &testThreeComponentQuery},
        {"query excludes missing components", &testQueryExcludesMissingComponents},
        {"query excludes dead entities", &testQueryExcludesDeadEntities},
        {"query edge cases", &testQueryEdgeCases},
        {"query visits each entity once", &testQueryVisitsEachEntityOnce},
        {"query does not copy components", &testQueryDoesNotCopyComponents},
        {"const query does not allow mutation", &testConstQueryDoesNotAllowMutation},
        {"movement system", &testMovementSystem},
        {"movement system across many entities", &testMovementSystemAcrossManyEntities},
        {"multiple systems on same entity", &testMultipleSystemsOnSameEntity},
        {"system execution order is deterministic", &testSystemExecutionOrderIsDeterministic},
        {"system query is repeatable", &testSystemQueryIsRepeatable},
        {"systems do not own entities", &testSystemsDoNotOwnEntities},
        {"add() reference stays valid", &testAddSystemReferenceStaysValid},
        {"systems with no entities", &testQueryAndSystemsWithNoEntities},
        {"query is recomputed each time", &testQueryIsRecomputedEachTime},
        {"query combined with tags and cleanup", &testQueryCombinedWithTagsAndCleanup},
        {"systems receive deltaSeconds", &testSystemsReceiveDeltaSeconds},
        {"all systems receive the same delta", &testAllSystemsReceiveTheSameDelta},
        {"delta reaches systems in registration order", &testDeltaReachesSystemsInRegistrationOrder},
    };

    int failedGroups = 0;

    for (const auto& [name, testCase] : testCases)
    {
        const int failuresBefore = g_failureCount;
        testCase();

        const bool passed = g_failureCount == failuresBefore;
        if (!passed)
        {
            ++failedGroups;
        }

        std::cout << (passed ? "  PASS  " : "  FAIL  ") << name << '\n';
    }

    const auto groupCount = sizeof(testCases) / sizeof(testCases[0]);

    if (g_failureCount != 0)
    {
        std::cerr << g_failureCount << " check(s) failed across " << failedGroups << " of " << groupCount
                  << " test groups\n";
        return EXIT_FAILURE;
    }

    std::cout << groupCount << " Phase 4 test groups passed\n";
    return EXIT_SUCCESS;
}
