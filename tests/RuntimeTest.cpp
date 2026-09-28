#include "engine/Time.hpp"
#include "engine/components/Transform.hpp"
#include "engine/ecs/EntityManager.hpp"
#include "engine/input/ActionState.hpp"
#include "engine/ecs/EntityView.hpp"
#include "engine/ecs/Query.hpp"
#include "engine/ecs/System.hpp"
#include "engine/ecs/SystemManager.hpp"
#include "engine/input/Input.hpp"
#include "engine/math/Vec2.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{

// ---------------------------------------------------------------------------
// The engine's first real component, plus test-only systems that exercise it.
// ---------------------------------------------------------------------------

using engine::Time;
using engine::Vec2;
using engine::components::Transform;
using engine::ecs::Entity;
using engine::ecs::EntityManager;
using engine::ecs::SystemManager;

/// The smallest concrete demonstration that the whole stack works:
/// Time -> SystemManager -> System -> Query -> Transform -> Vec2.
///
/// It is a demonstration, not a movement feature: no acceleration, no gravity,
/// no collision, no bounds. A real movement system belongs to a later phase.
class TransformMovementSystem final : public engine::ecs::System
{
public:
    void update(engine::ecs::EntityManager& entities, const engine::input::ActionState& actions, const float deltaSeconds) override
    {
        (void)actions; // time-driven, not actions-driven
        for (auto&& [entity, transform] : entities.query<Transform>())
        {
            (void)entity;
            transform.position += transform.velocity * deltaSeconds;
        }
    }

    [[nodiscard]] const char* name() const override { return "TransformMovementSystem"; }
};

/// A second system, to prove several systems can act on one entity and that
/// order is still deterministic once timing is involved.
class TransformSpinSystem final : public engine::ecs::System
{
public:
    void update(engine::ecs::EntityManager& entities, const engine::input::ActionState& actions, const float deltaSeconds) override
    {
        (void)actions; // time-driven, not actions-driven
        for (auto&& [entity, transform] : entities.query<Transform>())
        {
            (void)entity;
            transform.angle += deltaSeconds;
        }
    }

    [[nodiscard]] const char* name() const override { return "TransformSpinSystem"; }
};

/// Records the order systems ran in.
class OrderRecorderSystem final : public engine::ecs::System
{
public:
    explicit OrderRecorderSystem(std::vector<std::string>& log, std::string label)
        : m_log{&log}, m_label{std::move(label)}
    {
    }

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

/// An unrelated test-only component, so a query can be shown to exclude
/// entities that hold something else instead.
struct MarkerComponent
{
    int value = 0;
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

void checkNear(const float actual, const float expected, const char* const expression, const char* const file,
               const int line)
{
    // Tolerance for results that pass through floating point arithmetic.
    constexpr float kTolerance = 1e-5F;

    if (std::fabs(actual - expected) > kTolerance)
    {
        ++g_failureCount;
        std::cerr << "    " << file << ':' << line << ": CHECK_NEAR(" << expression << ") failed"
                  << "\n      actual = " << actual << ", expected = " << expected << '\n';
    }
}

// Variadic so an expression containing a comma, such as query<A, B>(), is
// treated as one argument.
#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)
#define CHECK_FALSE(...) check(!(__VA_ARGS__), "!" #__VA_ARGS__, __FILE__, __LINE__)
#define CHECK_NEAR(actual, expected) checkNear((actual), (expected), #actual " ~= " #expected, __FILE__, __LINE__)

/// An exact frame duration, in the clock's own units, so nothing is truncated.
auto millis(const float milliseconds)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<float>{milliseconds / 1000.0F});
}

// ---------------------------------------------------------------------------
// Compile-time guarantees: Transform is data, and nothing else.
// ---------------------------------------------------------------------------

// No user-provided constructors, so brace initialisation sets every field.
static_assert(std::is_aggregate_v<Transform>, "Transform must remain an aggregate");

// Plain data: trivially copyable and standard layout, so it can be stored,
// copied and moved by the ECS with no surprises.
static_assert(std::is_trivially_copyable_v<Transform>, "Transform must be trivially copyable");
static_assert(std::is_standard_layout_v<Transform>, "Transform must be standard layout");
static_assert(std::is_trivially_destructible_v<Transform>, "Transform must be trivially destructible");

// Still a plain data component: adding methods would break this.
static_assert(sizeof(Transform) == sizeof(Vec2) * 3 + sizeof(float), "Transform must hold exactly the four fields");

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

void testTimeStartsAtRest()
{
    const Time time;

    CHECK(time.deltaSeconds() == 0.0f);
    CHECK(time.elapsedSeconds() == 0.0f);
    CHECK(time.frameCount() == 0);
}

void testFirstTickReportsZero()
{
    Time time;

    // The first frame has no previous frame to measure against, so it must not
    // report the time spent constructing the world.
    time.advance(millis(500.0F));

    CHECK(time.deltaSeconds() == 0.0f);
    CHECK(time.frameCount() == 1);
}

void testDeltaIsInSeconds()
{
    Time time;
    time.advance(millis(500.0F)); // first tick: zero by definition
    time.advance(millis(16.0F));

    // 16 ms must be reported as 0.016 seconds, not 16.
    CHECK_NEAR(time.deltaSeconds(), 0.016F);
    CHECK_NEAR(time.deltaSeconds(), 16.0F / 1000.0F);
}

void testDeltaMatchesRealElapsed()
{
    Time time;
    time.advance(millis(1.0F)); // first tick
    time.advance(millis(20.0F));
    CHECK_NEAR(time.deltaSeconds(), 0.02F);

    time.advance(millis(5.0F));
    CHECK_NEAR(time.deltaSeconds(), 0.005F);

    time.advance(millis(0.0F));
    CHECK_NEAR(time.deltaSeconds(), 0.0F);
}

void testElapsedAccumulatesRealTime()
{
    Time time;
    time.advance(millis(1.0F));
    time.advance(millis(10.0F));
    time.advance(millis(20.0F));

    // Real time keeps counting up across every frame.
    CHECK_NEAR(time.elapsedSeconds(), 0.031F);
    CHECK(time.frameCount() == 3);

    // The delta is only ever the latest frame, not the running total.
    CHECK_NEAR(time.deltaSeconds(), 0.02F);
}

void testDeltaIsClamped()
{
    Time time;
    time.advance(millis(1.0F)); // first tick

    // A frame that took two seconds: a debugger pause, a breakpoint, a
    // minimised window. It must not be handed to a system in full.
    time.advance(millis(2000.0F));

    CHECK_NEAR(time.deltaSeconds(), Time::kMaxDeltaSeconds);
    CHECK(time.deltaSeconds() == 0.1F);
    CHECK(time.deltaSeconds() < 1.0F);

    // Elapsed time is still truthful, so diagnostics do not silently lose time.
    CHECK_NEAR(time.elapsedSeconds(), 2.001F);
}

void testClampBoundary()
{
    Time time;
    time.advance(millis(1.0F)); // first tick

    // Exactly at the limit is not clamped away.
    time.advance(millis(100.0F));
    CHECK_NEAR(time.deltaSeconds(), 0.1F);

    // A hair over the limit is.
    time.advance(millis(100.5F));
    CHECK_NEAR(time.deltaSeconds(), Time::kMaxDeltaSeconds);
}

void testTimeReset()
{
    Time time;
    time.advance(millis(1.0F));
    time.advance(millis(50.0F));
    CHECK(time.frameCount() == 2);

    time.reset();

    CHECK(time.deltaSeconds() == 0.0f);
    CHECK(time.elapsedSeconds() == 0.0F);
    CHECK(time.frameCount() == 0);

    // After a reset the next tick is a "first" tick again, so it reports zero
    // rather than the time since the reset.
    time.advance(millis(40.0F));
    CHECK(time.deltaSeconds() == 0.0f);

    time.advance(millis(10.0F));
    CHECK_NEAR(time.deltaSeconds(), 0.01F);
}

void testRealClockProducesSaneDeltas()
{
    // The only test that touches the real clock, and it only asserts that tick()
    // is wired to std::chrono at all. No sleeping, and a generous tolerance, so
    // this cannot be flaky.
    Time time;

    time.tick();
    CHECK(time.frameCount() == 1);
    CHECK(time.deltaSeconds() == 0.0f);

    // Busy-wait for a few milliseconds so the second tick has something to
    // measure. A spin rather than a sleep keeps this fast and predictable.
    const Time::Clock::time_point deadline = Time::Clock::now() + std::chrono::milliseconds{5};
    while (Time::Clock::now() < deadline)
    {
    }

    time.tick();

    CHECK(time.frameCount() == 2);
    // At least a hair of time passed, and certainly not a runaway value.
    CHECK(time.deltaSeconds() > 0.0f);
    CHECK(time.deltaSeconds() <= Time::kMaxDeltaSeconds);
    CHECK(time.elapsedSeconds() >= time.deltaSeconds());
}

// ---------------------------------------------------------------------------
// Transform
// ---------------------------------------------------------------------------

void testTransformDefaults()
{
    const Transform transform;

    CHECK(transform.position == Vec2(0.0F, 0.0F));
    CHECK(transform.velocity == Vec2(0.0F, 0.0F));
    CHECK(transform.scale == Vec2(1.0F, 1.0F));
    CHECK(transform.angle == 0.0F);
}

void testTransformAggregateConstruction()
{
    const Transform transform{Vec2{10.0F, 20.0F}, Vec2{2.0F, -1.0F}, Vec2{3.0F, 4.0F}, 1.5F};

    CHECK(transform.position == Vec2(10.0F, 20.0F));
    CHECK(transform.velocity == Vec2(2.0F, -1.0F));
    CHECK(transform.scale == Vec2(3.0F, 4.0F));
    CHECK_NEAR(transform.angle, 1.5F);
}

void testTransformFieldsAreWritable()
{
    Transform transform;

    transform.position = Vec2(5.0F, 6.0F);
    transform.velocity = Vec2(-1.0F, -2.0F);
    transform.scale = Vec2(2.0F, 2.0F);
    transform.angle = 0.5F;

    CHECK(transform.position == Vec2(5.0F, 6.0F));
    CHECK(transform.velocity == Vec2(-1.0F, -2.0F));
    CHECK(transform.scale == Vec2(2.0F, 2.0F));
    CHECK_NEAR(transform.angle, 0.5F);
}

void testTransformUsesEngineVec2()
{
    Transform transform;
    transform.velocity = Vec2(1.0F, 0.0F).rotated(0.5F);
    transform.angle = 0.5F;

    // The fields really are Vec2, so Vec2's own operations apply to them
    // directly rather than to two loose floats each.
    CHECK_NEAR(transform.velocity.length(), 1.0F);
    CHECK_NEAR(transform.velocity.x, std::cos(0.5F));
    CHECK_NEAR(transform.velocity.y, std::sin(0.5F));
    CHECK_NEAR(transform.velocity.dot(Vec2(1.0F, 0.0F)), std::cos(0.5F));

    // A scale is not a direction: (1, 1) means "no scaling on either axis", so
    // its length is sqrt(2) and not 1.
    CHECK(transform.scale == Vec2(1.0F, 1.0F));
    CHECK(transform.position == Vec2(0.0F, 0.0F));
}

void testTransformIsStoredOnEntities()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::ActionState actions;
    Entity& entity = manager.addEntity("entity");

    Transform& transform = entity.addComponent<Transform>();

    CHECK(entity.hasComponent<Transform>());
    CHECK(entity.componentCount() == 1);
    CHECK(&entity.getComponent<Transform>() == &transform);

    // Defaults survive a round trip through the ECS.
    CHECK(entity.getComponent<Transform>().position == Vec2(0.0F, 0.0F));
    CHECK(entity.getComponent<Transform>().scale == Vec2(1.0F, 1.0F));

    // And can be written through the retrieved reference.
    entity.getComponent<Transform>().velocity = Vec2(3.0F, 4.0F);
    CHECK(entity.getComponent<Transform>().velocity == Vec2(3.0F, 4.0F));
}

void testTransformsAreIndependentPerEntity()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::ActionState actions;
    Entity& first = manager.addEntity("first");
    Entity& second = manager.addEntity("second");

    first.addComponent<Transform>(Transform{Vec2{1.0F, 1.0F}, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
    second.addComponent<Transform>();

    CHECK(first.getComponent<Transform>().position == Vec2(1.0F, 1.0F));
    CHECK(second.getComponent<Transform>().position == Vec2(0.0F, 0.0F));

    first.getComponent<Transform>().position = Vec2(99.0F, 99.0F);
    CHECK(second.getComponent<Transform>().position == Vec2(0.0F, 0.0F));

    // Removing one entity's Transform leaves the other alone.
    first.removeComponent<Transform>();
    CHECK_FALSE(first.hasComponent<Transform>());
    CHECK(second.hasComponent<Transform>());
    CHECK(second.getComponent<Transform>().scale == Vec2(1.0F, 1.0F));
}

void testTransformQuery()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::ActionState actions;
    Entity& moving = manager.addEntity("moving");
    Entity& still = manager.addEntity("still");
    Entity& other = manager.addEntity("other");
    Entity& doomed = manager.addEntity("doomed");

    moving.addComponent<Transform>(Transform{Vec2{0.0F, 0.0F}, Vec2{1.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
    still.addComponent<Transform>();
    doomed.addComponent<Transform>(Transform{Vec2{0.0F, 0.0F}, Vec2{5.0F, 5.0F}, Vec2{1.0F, 1.0F}, 0.0F});
    other.addComponent<MarkerComponent>(); // holds something, but not a Transform

    CHECK(manager.query<Transform>().size() == 3);

    manager.destroyEntity(doomed);
    CHECK(manager.query<Transform>().size() == 2);

    std::size_t seen = 0;
    for (auto&& [entity, transform] : manager.query<Transform>())
    {
        CHECK(entity.isAlive());
        CHECK(entity.id() != doomed.id());
        (void)transform;
        ++seen;
    }
    CHECK(seen == 2);
}

// ---------------------------------------------------------------------------
// The whole stack: Time -> SystemManager -> System -> Query -> Transform
// ---------------------------------------------------------------------------

void testMovementSystemUsesDelta()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::ActionState actions;
    Entity& entity = manager.addEntity("entity");
    entity.addComponent<Transform>(Transform{Vec2{0.0F, 0.0F}, Vec2{100.0F, 50.0F}, Vec2{1.0F, 1.0F}, 0.0F});

    SystemManager systems;
    systems.add<TransformMovementSystem>();

    // One second of movement at 100,50 units per second.
    systems.update(manager, actions, 1.0F);
    CHECK(entity.getComponent<Transform>().position == Vec2(100.0F, 50.0F));

    // Half a second moves half as far: the system is genuinely time-scaled.
    systems.update(manager, actions, 0.5F);
    CHECK_NEAR(entity.getComponent<Transform>().position.x, 150.0F);
    CHECK_NEAR(entity.getComponent<Transform>().position.y, 75.0F);
}

void testMovementSystemMatchesVec2Semantics()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::ActionState actions;
    Entity& entity = manager.addEntity("entity");
    // 45 degrees, unit speed: one second of travel ends on the unit circle.
    entity.addComponent<Transform>(Transform{Vec2{0.0F, 0.0F}, Vec2{1.0F, 0.0F}.rotated(0.25F), Vec2{1.0F, 1.0F}, 0.0F});

    SystemManager systems;
    systems.add<TransformMovementSystem>();
    systems.update(manager, actions, 1.0F);

    const Transform& transform = entity.getComponent<Transform>();
    CHECK_NEAR(transform.position.length(), 1.0F);
    CHECK_NEAR(transform.position.x, std::cos(0.25F));
    CHECK_NEAR(transform.position.y, std::sin(0.25F));
}

void testClampedDeltaProtectsMovement()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::ActionState actions;
    Entity& entity = manager.addEntity("entity");
    entity.addComponent<Transform>(Transform{Vec2{0.0F, 0.0F}, Vec2{100.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});

    SystemManager systems;
    systems.add<TransformMovementSystem>();

    // A two second stall is clamped to 0.1 s, so the entity moves 10 units
    // instead of 200 and nothing is flung off screen.
    systems.update(manager, actions, Time::kMaxDeltaSeconds);

    CHECK(entity.getComponent<Transform>().position == Vec2(10.0F, 0.0F));
}

void testRealTimeDrivesMovement()
{
    // The full chain, with the clock driving it rather than a literal delta.
    EntityManager manager;
    [[maybe_unused]] engine::input::ActionState actions;
    Entity& entity = manager.addEntity("entity");
    entity.addComponent<Transform>(Transform{Vec2{0.0F, 0.0F}, Vec2{60.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});

    SystemManager systems;
    systems.add<TransformMovementSystem>();

    Time time;
    time.tick(); // first frame: zero delta, nothing moves

    CHECK(entity.getComponent<Transform>().position == Vec2(0.0F, 0.0F));

    // Advance the clock by a known amount rather than sleeping, so this is
    // deterministic: one 20 ms frame.
    time.advance(millis(1.0F));
    time.advance(millis(20.0F));
    CHECK_NEAR(time.deltaSeconds(), 0.02F);

    systems.update(manager, actions, time.deltaSeconds());

    // 60 units per second for 0.02 s is 1.2 units.
    CHECK_NEAR(entity.getComponent<Transform>().position.x, 1.2F);
}

void testMultipleSystemsAndOrderWithTiming()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::ActionState actions;
    std::vector<std::string> order;

    Entity& entity = manager.addEntity("entity");
    entity.addComponent<Transform>(Transform{Vec2{0.0F, 0.0F}, Vec2{10.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});

    SystemManager systems;
    systems.add<OrderRecorderSystem>(order, "A");
    systems.add<TransformMovementSystem>();
    systems.add<TransformSpinSystem>();
    systems.add<OrderRecorderSystem>(order, "B");

    systems.update(manager, actions, 0.5F);

    CHECK(order.size() == 2);
    if (order.size() == 2)
    {
        CHECK(order[0] == "A");
        CHECK(order[1] == "B");
    }

    // Both systems touched the same entity in the same frame.
    const Transform& transform = entity.getComponent<Transform>();
    CHECK_NEAR(transform.position.x, 5.0F);
    CHECK_NEAR(transform.angle, 0.5F);
}

void testEntitiesWithoutTransformAreUntouched()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::ActionState actions;
    Entity& plain = manager.addEntity("plain");
    plain.addComponent<MarkerComponent>();
    manager.addEntity("bare");

    SystemManager systems;
    systems.add<TransformMovementSystem>();
    systems.update(manager, actions, 1.0F);

    CHECK(manager.query<Transform>().empty());
    CHECK(manager.aliveEntityCount() == 2);
}

} // namespace

int main()
{
    const std::pair<const char*, void (*)()> testCases[] = {
        {"time starts at rest", &testTimeStartsAtRest},
        {"first tick reports zero", &testFirstTickReportsZero},
        {"delta is in seconds", &testDeltaIsInSeconds},
        {"delta matches real elapsed", &testDeltaMatchesRealElapsed},
        {"elapsed accumulates real time", &testElapsedAccumulatesRealTime},
        {"delta is clamped", &testDeltaIsClamped},
        {"clamp boundary", &testClampBoundary},
        {"time reset", &testTimeReset},
        {"real clock produces sane deltas", &testRealClockProducesSaneDeltas},
        {"transform defaults", &testTransformDefaults},
        {"transform aggregate construction", &testTransformAggregateConstruction},
        {"transform fields are writable", &testTransformFieldsAreWritable},
        {"transform uses engine Vec2", &testTransformUsesEngineVec2},
        {"transform is stored on entities", &testTransformIsStoredOnEntities},
        {"transforms are independent per entity", &testTransformsAreIndependentPerEntity},
        {"transform query", &testTransformQuery},
        {"movement system uses delta", &testMovementSystemUsesDelta},
        {"movement system matches Vec2 semantics", &testMovementSystemMatchesVec2Semantics},
        {"clamped delta protects movement", &testClampedDeltaProtectsMovement},
        {"real time drives movement", &testRealTimeDrivesMovement},
        {"multiple systems and order with timing", &testMultipleSystemsAndOrderWithTiming},
        {"entities without transform are untouched", &testEntitiesWithoutTransformAreUntouched},
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

    std::cout << groupCount << " Phase 5 test groups passed\n";
    return EXIT_SUCCESS;
}
