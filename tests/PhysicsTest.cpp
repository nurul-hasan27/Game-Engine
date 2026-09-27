#include "engine/components/Body.hpp"
#include "engine/components/Collider.hpp"
#include "engine/components/Transform.hpp"
#include "engine/ecs/EntityManager.hpp"
#include "engine/ecs/SystemManager.hpp"
#include "engine/input/Input.hpp"
#include "engine/math/Vec2.hpp"
#include "engine/physics/Aabb.hpp"
#include "engine/systems/MovementSystem.hpp"
#include "engine/systems/PhysicsSystem.hpp"

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{

using engine::Vec2;
using engine::components::Body;
using engine::components::Collider;
using engine::components::Transform;
using engine::ecs::Entity;
using engine::ecs::EntityManager;
using engine::input::Input;
using engine::input::Key;
using engine::physics::Aabb;
using engine::physics::BodyType;
using engine::systems::MovementSystem;
using engine::systems::PhysicsSystem;

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
    constexpr float kTolerance = 1e-4F;

    if (std::fabs(actual - expected) > kTolerance)
    {
        ++g_failureCount;
        std::cerr << "    " << file << ':' << line << ": CHECK_NEAR(" << expression << ") failed"
                  << "\n      actual = " << actual << ", expected = " << expected << '\n';
    }
}

void checkNearVec(const Vec2& actual, const Vec2& expected, const char* const expression, const char* const file,
                  const int line)
{
    checkNear(actual.x, expected.x, expression, file, line);
    checkNear(actual.y, expected.y, expression, file, line);
}

#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)
#define CHECK_FALSE(...) check(!(__VA_ARGS__), "!" #__VA_ARGS__, __FILE__, __LINE__)
#define CHECK_NEAR(actual, expected) checkNear((actual), (expected), #actual " ~= " #expected, __FILE__, __LINE__)
#define CHECK_NEAR_VEC(actual, expected) checkNearVec((actual), (expected), #actual, __FILE__, __LINE__)

// ---------------------------------------------------------------------------
// Compile-time guarantees.
// ---------------------------------------------------------------------------

// The physics layer must stay plain data and free of behaviour.
static_assert(std::is_trivially_copyable_v<Collider>, "Collider must be plain data");
static_assert(std::is_aggregate_v<Collider>, "Collider must remain an aggregate");
static_assert(std::is_trivially_copyable_v<Body>, "Body must be plain data");
static_assert(std::is_aggregate_v<Body>, "Body must remain an aggregate");

// Aabb is a value type: two of them can be made, compared and copied freely.
static_assert(std::is_trivially_copyable_v<Aabb>, "Aabb must be a trivial value type");

// ---------------------------------------------------------------------------
// AABB maths
// ---------------------------------------------------------------------------

void testAabbConstruction()
{
    const Aabb box{Vec2{100.0F, 50.0F}, Vec2{40.0F, 20.0F}};

    CHECK_NEAR_VEC(box.center(), Vec2(100.0F, 50.0F));
    CHECK_NEAR_VEC(box.size(), Vec2(40.0F, 20.0F));
    CHECK_NEAR_VEC(box.halfExtents(), Vec2(20.0F, 10.0F));
    // Centre-based: min and max are derived from the centre, never stored.
    CHECK_NEAR_VEC(box.min(), Vec2(80.0F, 40.0F));
    CHECK_NEAR_VEC(box.max(), Vec2(120.0F, 60.0F));
}

void testAabbIdenticalBoxes()
{
    const Aabb box{Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}};
    const Aabb same{Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}};

    CHECK(box.overlaps(same));
    CHECK(same.overlaps(box));
    CHECK_NEAR(box.overlapX(same), 20.0F);
    CHECK_NEAR(box.overlapY(same), 20.0F);
}

void testAabbSeparatedOnX()
{
    const Aabb left{Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}};
    const Aabb right{Vec2{100.0F, 0.0F}, Vec2{20.0F, 20.0F}};

    CHECK_FALSE(left.overlaps(right));
    CHECK_FALSE(right.overlaps(left));
    CHECK_NEAR(left.overlapX(right), -80.0F);
}

void testAabbSeparatedOnY()
{
    const Aabb top{Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}};
    const Aabb bottom{Vec2{0.0F, 100.0F}, Vec2{20.0F, 20.0F}};

    CHECK_FALSE(top.overlaps(bottom));
    CHECK_NEAR(top.overlapY(bottom), -80.0F);
}

void testAabbTouchingEdgesDoNotOverlap()
{
    // Deliberate, documented choice: sharing only an edge is not penetration.
    // This is what lets a body rest on a surface without fighting it forever.
    const Aabb left{Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}};
    const Aabb right{Vec2{20.0F, 0.0F}, Vec2{20.0F, 20.0F}};
    const Aabb below{Vec2{0.0F, 20.0F}, Vec2{20.0F, 20.0F}};

    // Touching on x only.
    CHECK_FALSE(left.overlaps(right));
    CHECK_NEAR(left.overlapX(right), 0.0F);
    CHECK_NEAR(left.overlapY(right), 20.0F);

    // Touching on y only.
    CHECK_FALSE(left.overlaps(below));
    CHECK_NEAR(left.overlapY(below), 0.0F);

    // A corner touch overlaps on neither axis.
    const Aabb corner{Vec2{20.0F, 20.0F}, Vec2{20.0F, 20.0F}};
    CHECK_FALSE(left.overlaps(corner));
}

void testAabbPartialOverlap()
{
    const Aabb first{Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}};
    const Aabb second{Vec2{10.0F, 10.0F}, Vec2{20.0F, 20.0F}};

    CHECK(first.overlaps(second));
    CHECK_NEAR(first.overlapX(second), 10.0F);
    CHECK_NEAR(first.overlapY(second), 10.0F);
}

void testAabbCompleteContainment()
{
    const Aabb big{Vec2{0.0F, 0.0F}, Vec2{100.0F, 100.0F}};
    const Aabb small{Vec2{0.0F, 0.0F}, Vec2{10.0F, 10.0F}};

    CHECK(big.overlaps(small));
    CHECK(small.overlaps(big));
    // Overlap is measured against the smaller box when it is fully inside.
    CHECK_NEAR(big.overlapX(small), 10.0F);
    CHECK_NEAR(small.overlapX(big), 10.0F);
}

void testAabbPenetrationAxis()
{
    // Least penetration on x: two 100-wide boxes almost touching horizontally
    // (5 of overlap) and fully overlapping vertically (100).
    const Aabb wide{Vec2{0.0F, 0.0F}, Vec2{100.0F, 100.0F}};
    const Aabb shiftedRight{Vec2{95.0F, 0.0F}, Vec2{100.0F, 100.0F}};
    CHECK_NEAR(wide.overlapX(shiftedRight), 5.0F);
    CHECK_NEAR(wide.overlapY(shiftedRight), 100.0F);
    CHECK(wide.penetrationAxis(shiftedRight) == Aabb::PenetrationAxis::Horizontal);

    // Least penetration on y: the same pair, offset vertically instead.
    const Aabb shiftedDown{Vec2{0.0F, 95.0F}, Vec2{100.0F, 100.0F}};
    CHECK_NEAR(wide.overlapX(shiftedDown), 100.0F);
    CHECK_NEAR(wide.overlapY(shiftedDown), 5.0F);
    CHECK(wide.penetrationAxis(shiftedDown) == Aabb::PenetrationAxis::Vertical);

    // Separated on both axes.
    CHECK(Aabb{Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}}.penetrationAxis(Aabb{Vec2{99.0F, 99.0F}, Vec2{1.0F, 1.0F}}) ==
          Aabb::PenetrationAxis::None);
}

void testAabbMinimumTranslationDirection()
{
    const Aabb left{Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}};
    const Aabb right{Vec2{10.0F, 0.0F}, Vec2{20.0F, 20.0F}};

    // The left box is pushed left, the right box is pushed right: the direction
    // comes from the relative centres, never from a fixed convention.
    CHECK_NEAR_VEC(left.minimumTranslation(right), Vec2(-10.0F, 0.0F));
    CHECK_NEAR_VEC(right.minimumTranslation(left), Vec2(10.0F, 0.0F));

    // Same horizontally, stacked vertically.
    const Aabb above{Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}};
    const Aabb below{Vec2{0.0F, 10.0F}, Vec2{20.0F, 20.0F}};
    CHECK_NEAR_VEC(above.minimumTranslation(below), Vec2(0.0F, -10.0F));
    CHECK_NEAR_VEC(below.minimumTranslation(above), Vec2(0.0F, 10.0F));
}

void testAabbZeroAndNegativeSize()
{
    // A zero-size box is a degenerate point, and because overlap is strict a
    // point has no extent, so it never overlaps anything. An inert collider
    // rather than an error.
    const Aabb point{Vec2{0.0F, 0.0F}, Vec2{0.0F, 0.0F}};
    CHECK_NEAR_VEC(point.size(), Vec2(0.0F, 0.0F));

    const Aabb big{Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}};
    CHECK_FALSE(point.overlaps(big));
    CHECK_FALSE(big.overlaps(point));
    CHECK_FALSE(point.overlaps(Aabb{Vec2{0.0F, 0.0F}, Vec2{0.0F, 0.0F}}));
    CHECK(point.penetrationAxis(big) == Aabb::PenetrationAxis::None);

    // A negative size is clamped to zero rather than inverting the box.
    const Aabb negative{Vec2{0.0F, 0.0F}, Vec2{-20.0F, -20.0F}};
    CHECK_NEAR_VEC(negative.size(), Vec2(0.0F, 0.0F));
    CHECK(negative.min().x <= negative.max().x);
    CHECK(negative.min().y <= negative.max().y);
    CHECK_FALSE(negative.overlaps(big));
}

// ---------------------------------------------------------------------------
// World helpers
// ---------------------------------------------------------------------------

/// A dynamic body with a collider.
Entity& makeBody(EntityManager& manager, const Vec2& position, const Vec2& size, const BodyType type,
                 const Vec2& velocity = Vec2{0.0F, 0.0F})
{
    Entity& entity = manager.addEntity("body");
    entity.addComponent<Transform>(Transform{position, velocity, Vec2{1.0F, 1.0F}, 0.0F});
    entity.addComponent<Collider>(Collider{size});
    entity.addComponent<Body>(Body{type});
    return entity;
}

Transform& transformOf(Entity& entity)
{
    return entity.getComponent<Transform>();
}

/// Steps physics for `frames` frames of a fixed size, with nothing held so the
/// velocities set on the bodies are the only thing driving them.
///
/// Discrete detection means a body advances first and is corrected afterwards,
/// so a single huge step can end up past an obstacle. Real frames are small, so
/// the scenarios below step in realistic increments and assert the invariant
/// that matters: the body is stopped at the surface, not beyond it.
void step(EntityManager& manager, const int frames, const float deltaSeconds = 1.0F / 60.0F)
{
    Input input;
    PhysicsSystem physics;
    for (int frame = 0; frame < frames; ++frame)
    {
        physics.update(manager, input, deltaSeconds);
    }
}

// ---------------------------------------------------------------------------
// Integration: velocity
// ---------------------------------------------------------------------------

void testPhysicsIntegratesVelocity()
{
    EntityManager manager;
    Input input;
    engine::ecs::SystemManager systems;
    systems.add<PhysicsSystem>();

    Entity& entity = makeBody(manager, Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Dynamic);
    transformOf(entity).velocity = Vec2{100.0F, 0.0F};

    systems.update(manager, input, 1.0F);
    CHECK_NEAR_VEC(transformOf(entity).position, Vec2(100.0F, 0.0F));

    systems.update(manager, input, 0.5F);
    CHECK_NEAR_VEC(transformOf(entity).position, Vec2(150.0F, 0.0F));
}

void testStaticBodyNeverMoves()
{
    EntityManager manager;
    Input input;
    engine::ecs::SystemManager systems;
    systems.add<PhysicsSystem>();

    Entity& wall = makeBody(manager, Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Static);
    // Even if something hands a static body a velocity, physics discards it.
    transformOf(wall).velocity = Vec2{500.0F, 500.0F};

    systems.update(manager, input, 1.0F);

    CHECK_NEAR_VEC(transformOf(wall).position, Vec2(0.0F, 0.0F));
    CHECK_NEAR_VEC(transformOf(wall).velocity, Vec2(0.0F, 0.0F));
}

// ---------------------------------------------------------------------------
// Resolution: dynamic vs static
// ---------------------------------------------------------------------------

void testDynamicStopsAtStaticWall()
{
    EntityManager manager;

    // A tall wall at x=400, 100 wide, so its left face is at x=350.
    makeBody(manager, Vec2{400.0F, 0.0F}, Vec2{100.0F, 1000.0F}, BodyType::Static);

    // A 50-wide player starting at the origin, running right at 200 px/s.
    Entity& player = makeBody(manager, Vec2{0.0F, 0.0F}, Vec2{50.0F, 50.0F}, BodyType::Dynamic,
                              Vec2{200.0F, 0.0F});

    step(manager, 120);

    // Stopped with its right face exactly against the wall's left face at x=350,
    // so the player's centre rests at 350 - 25.
    //
    // The resting position is exact, not a range. Nothing re-applies velocity
    // here, so once the correction has pushed the body clear its velocity is
    // zero and it never moves again. That is what makes this a strong
    // assertion: a resolution that applied only half the correction, or the
    // wrong axis, would leave it somewhere else entirely.
    CHECK_NEAR(transformOf(player).position.x, 325.0F);
    // It did not tunnel through: still well left of the wall's right face.
    CHECK(transformOf(player).position.x < 450.0F);
    // Moving into the wall, so the horizontal velocity is gone.
    CHECK_NEAR(transformOf(player).velocity.x, 0.0F);
    // Nothing was in the way vertically, so that velocity is untouched.
    CHECK_NEAR(transformOf(player).velocity.y, 0.0F);
}

void testDynamicTakesTheWholeCorrectionAgainstAStatic()
{
    // A single resolution step with zero elapsed time, so the correction applied
    // is observable on its own rather than after a run of frames.
    //
    // This is the test that pins down *how much* of the correction a dynamic
    // body facing a static one takes. The stepped tests above only show the
    // resting position; those would still pass if the correction were halved
    // every frame, because halving a shrinking residual converges on the right
    // answer. Here the whole correction has to happen at once.
    EntityManager manager;
    Input input;
    engine::ecs::SystemManager systems;
    systems.add<PhysicsSystem>();

    // The player is created **first**, so it is the outer of the pair and the
    // dynamic-versus-static branch that moves the outer body is the one taken.
    // The opposite order, where the static body is first and the inner body has
    // to be moved, is covered by testStaticOnLeftPushesDynamicRight.
    Entity& player = makeBody(manager, Vec2{360.0F, 0.0F}, Vec2{50.0F, 50.0F}, BodyType::Dynamic);

    // Wall spans x 350..450. The player spans 335..385, so it overlaps the wall
    // by 35 horizontally and by its full 50 vertically, making x the shallower
    // axis to escape along.
    makeBody(manager, Vec2{400.0F, 0.0F}, Vec2{100.0F, 1000.0F}, BodyType::Static);

    systems.update(manager, input, 0.0F);

    // The full 35, not half of it: 360 - 35 = 325, which puts the player's right
    // face exactly on the wall's left face.
    CHECK_NEAR(transformOf(player).position.x, 325.0F);
    // The shallower axis was chosen, so y is untouched.
    CHECK_NEAR(transformOf(player).position.y, 0.0F);
}

void testStaticOnLeftPushesDynamicRight()
{
    EntityManager manager;

    // Wall to the left, its right face at x=50.
    makeBody(manager, Vec2{0.0F, 0.0F}, Vec2{100.0F, 1000.0F}, BodyType::Static);
    Entity& player = makeBody(manager, Vec2{400.0F, 0.0F}, Vec2{50.0F, 50.0F}, BodyType::Dynamic,
                              Vec2{-200.0F, 0.0F});

    step(manager, 120);

    // Stopped with its left face against the wall's right face: 50 + 25.
    CHECK_NEAR(transformOf(player).position.x, 75.0F);
    // Still on the right side of the wall, not pushed through it.
    CHECK(transformOf(player).position.x > 0.0F);
    CHECK_NEAR(transformOf(player).velocity.x, 0.0F);
}

void testVerticalCollisionFromAbove()
{
    EntityManager manager;

    // Floor below, its top face at y=350.
    makeBody(manager, Vec2{0.0F, 400.0F}, Vec2{1000.0F, 100.0F}, BodyType::Static);
    Entity& player = makeBody(manager, Vec2{0.0F, 0.0F}, Vec2{50.0F, 50.0F}, BodyType::Dynamic,
                              Vec2{0.0F, 200.0F});

    step(manager, 120);

    // Resting with its bottom face exactly on the floor's top face: 350 - 25.
    CHECK_NEAR(transformOf(player).position.y, 325.0F);
    CHECK(transformOf(player).position.y < 350.0F);
    CHECK_NEAR(transformOf(player).velocity.y, 0.0F);
    CHECK_NEAR(transformOf(player).velocity.x, 0.0F);
}

void testVerticalCollisionFromBelow()
{
    EntityManager manager;

    // Ceiling above, its bottom face at y=50.
    makeBody(manager, Vec2{0.0F, 0.0F}, Vec2{1000.0F, 100.0F}, BodyType::Static);
    Entity& player = makeBody(manager, Vec2{0.0F, 400.0F}, Vec2{50.0F, 50.0F}, BodyType::Dynamic,
                              Vec2{0.0F, -200.0F});

    step(manager, 120);

    // Resting with its top face exactly on the ceiling's bottom face: 50 + 25.
    CHECK_NEAR(transformOf(player).position.y, 75.0F);
    CHECK(transformOf(player).position.y > 50.0F);
    CHECK_NEAR(transformOf(player).velocity.y, 0.0F);
}

void testStaticVersusStaticDoesNothing()
{
    EntityManager manager;
    Input input;
    engine::ecs::SystemManager systems;
    systems.add<PhysicsSystem>();

    // Two walls deliberately overlapping.
    Entity& first = makeBody(manager, Vec2{0.0F, 0.0F}, Vec2{40.0F, 40.0F}, BodyType::Static);
    Entity& second = makeBody(manager, Vec2{10.0F, 0.0F}, Vec2{40.0F, 40.0F}, BodyType::Static);

    systems.update(manager, input, 1.0F);

    // Neither can be pushed, so nothing moves. Overlapping walls are a level
    // design problem, not something the simulation should paper over.
    CHECK_NEAR_VEC(transformOf(first).position, Vec2(0.0F, 0.0F));
    CHECK_NEAR_VEC(transformOf(second).position, Vec2(10.0F, 0.0F));
}

// ---------------------------------------------------------------------------
// Resolution: dynamic vs dynamic
// ---------------------------------------------------------------------------

void testDynamicVersusDynamicSplitsCorrection()
{
    EntityManager manager;
    Input input;
    engine::ecs::SystemManager systems;
    systems.add<PhysicsSystem>();

    // Two 40-wide boxes: the first closes on a stationary second.
    Entity& first = makeBody(manager, Vec2{0.0F, 0.0F}, Vec2{40.0F, 40.0F}, BodyType::Dynamic,
                             Vec2{30.0F, 0.0F});
    Entity& second = makeBody(manager, Vec2{40.0F, 0.0F}, Vec2{40.0F, 40.0F}, BodyType::Dynamic);

    systems.update(manager, input, 1.0F);

    // After integration the first is at 30, giving 30 of horizontal overlap
    // against 40 of vertical overlap, so the correction is horizontal. It is
    // split evenly, so neither body is favoured: 15 each way.
    CHECK_NEAR(transformOf(first).position.x, 15.0F);
    CHECK_NEAR(transformOf(second).position.x, 55.0F);
    // The first was driving into the second, so its velocity is gone.
    CHECK_NEAR(transformOf(first).velocity.x, 0.0F);
    // The second was not moving, and did not cause its own push, so it is
    // untouched.
    CHECK_NEAR(transformOf(second).velocity.x, 0.0F);
}

// ---------------------------------------------------------------------------
// Velocity resolution
// ---------------------------------------------------------------------------

void testTangentialVelocitySurvives()
{
    EntityManager manager;

    makeBody(manager, Vec2{200.0F, 0.0F}, Vec2{20.0F, 400.0F}, BodyType::Static);

    // Running diagonally into a vertical wall: the x component is into the
    // surface, the y component is not.
    Entity& player = makeBody(manager, Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Dynamic,
                              Vec2{300.0F, 50.0F});

    step(manager, 120);

    CHECK_NEAR(transformOf(player).velocity.x, 0.0F);
    // The slide along the wall is the whole point of only zeroing one axis.
    CHECK_NEAR(transformOf(player).velocity.y, 50.0F);
    CHECK(transformOf(player).position.y > 0.0F);
}

void testMovingAwayKeepsVelocity()
{
    EntityManager manager;
    Input input;
    engine::ecs::SystemManager systems;
    systems.add<PhysicsSystem>();

    makeBody(manager, Vec2{200.0F, 0.0F}, Vec2{20.0F, 400.0F}, BodyType::Static);

    // Already overlapping the wall, and to the right of its centre, so the
    // correction pushes it further right. Its velocity already points that way,
    // so it must not be frozen merely because the boxes overlap.
    Entity& player = makeBody(manager, Vec2{205.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Dynamic,
                              Vec2{50.0F, 0.0F});

    // Zero elapsed time, so this exercises resolution alone with no integration.
    systems.update(manager, input, 0.0F);

    // Pushed clear to the right, and still moving right.
    CHECK(transformOf(player).position.x > 210.0F);
    CHECK_NEAR(transformOf(player).velocity.x, 50.0F);
}

void testNoCollisionLeavesEverythingAlone()
{
    EntityManager manager;
    Input input;
    engine::ecs::SystemManager systems;
    systems.add<PhysicsSystem>();

    Entity& moving = makeBody(manager, Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Dynamic,
                              Vec2{10.0F, 20.0F});
    Entity& still = makeBody(manager, Vec2{500.0F, 500.0F}, Vec2{20.0F, 20.0F}, BodyType::Dynamic);

    systems.update(manager, input, 1.0F);

    CHECK_NEAR_VEC(transformOf(moving).position, Vec2(10.0F, 20.0F));
    CHECK_NEAR_VEC(transformOf(moving).velocity, Vec2(10.0F, 20.0F));
    CHECK_NEAR_VEC(transformOf(still).position, Vec2(500.0F, 500.0F));
    CHECK_NEAR_VEC(transformOf(still).velocity, Vec2(0.0F, 0.0F));
}

// ---------------------------------------------------------------------------
// Filtering
// ---------------------------------------------------------------------------

void testDeadEntitiesAreIgnored()
{
    EntityManager manager;
    Input input;
    engine::ecs::SystemManager systems;
    systems.add<PhysicsSystem>();

    makeBody(manager, Vec2{200.0F, 0.0F}, Vec2{20.0F, 200.0F}, BodyType::Static);
    Entity& dead = makeBody(manager, Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Dynamic,
                            Vec2{300.0F, 0.0F});
    manager.destroyEntity(dead);

    systems.update(manager, input, 1.0F);

    // A dead body is not integrated and not collided, so it is left as it was.
    CHECK_NEAR_VEC(transformOf(dead).position, Vec2(0.0F, 0.0F));
    CHECK_NEAR_VEC(transformOf(dead).velocity, Vec2(300.0F, 0.0F));
}

void testEntitiesWithoutColliderAreIgnored()
{
    EntityManager manager;
    Input input;
    engine::ecs::SystemManager systems;
    systems.add<PhysicsSystem>();

    // Has a Transform and a Body but no Collider: moves, cannot collide.
    Entity& ghost = manager.addEntity("ghost");
    ghost.addComponent<Transform>(Transform{Vec2{0.0F, 0.0F}, Vec2{100.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
    ghost.addComponent<Body>(Body{BodyType::Dynamic});

    // Overlaps the first body in space, but has no box to be tested with.
    makeBody(manager, Vec2{200.0F, 0.0F}, Vec2{20.0F, 200.0F}, BodyType::Static);

    systems.update(manager, input, 1.0F);

    // It still integrates velocity, which is what a plain Transform entity does.
    CHECK_NEAR_VEC(transformOf(ghost).position, Vec2(100.0F, 0.0F));
}

void testEntityWithoutBodyIsNotResolved()
{
    EntityManager manager;
    Input input;
    engine::ecs::SystemManager systems;
    systems.add<PhysicsSystem>();

    // A Transform and a Collider but no Body: nothing says it participates in
    // collision, so it is not part of any pair.
    Entity& shape = manager.addEntity("shape");
    shape.addComponent<Transform>(Transform{Vec2{0.0F, 0.0F}, Vec2{300.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
    shape.addComponent<Collider>(Collider{Vec2{20.0F, 20.0F}});

    makeBody(manager, Vec2{200.0F, 0.0F}, Vec2{20.0F, 200.0F}, BodyType::Static);

    systems.update(manager, input, 1.0F);

    // It integrates, and is not pushed out of the wall.
    CHECK_NEAR_VEC(transformOf(shape).position, Vec2(300.0F, 0.0F));
    CHECK_NEAR_VEC(transformOf(shape).velocity, Vec2(300.0F, 0.0F));
}

void testEachPairResolvedOnce()
{
    // Three mutually overlapping dynamic bodies, no walls. If the pair loop ever
    // compared a body with itself, or processed a pair in both orders, the
    // corrections would compound and these exact positions would not come out.
    EntityManager manager;
    Input input;
    engine::ecs::SystemManager systems;
    systems.add<PhysicsSystem>();

    // 20-wide boxes 15 apart, so each neighbouring pair overlaps by 5 and the
    // first and last do not overlap at all.
    Entity& first = makeBody(manager, Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Dynamic);
    Entity& second = makeBody(manager, Vec2{15.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Dynamic);
    Entity& third = makeBody(manager, Vec2{30.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Dynamic);

    systems.update(manager, input, 0.0F);

    // Pairs visited, in ECS order, are (0,1), (0,2), (1,2). (0,2) is separated
    // and contributes nothing.
    //
    // (0,1) overlap 5, split evenly: first 0 - 2.5 = -2.5, second 15 + 2.5 = 17.5.
    // (1,2) is then measured against second's *moved* position of 17.5 against 30,
    // so the overlap is 7.5, split evenly: second 17.5 - 3.75 = 13.75,
    // third 30 + 3.75 = 33.75.
    CHECK_NEAR(transformOf(first).position.x, -2.5F);
    CHECK_NEAR(transformOf(third).position.x, 33.75F);
    // The middle body takes the push from the first pair and the push from the
    // second, and each was applied exactly once.
    CHECK_NEAR(transformOf(second).position.x, 13.75F);

    // A single pass does not fully separate a cluster, which is a known
    // limitation rather than a pair-counting bug: whatever overlap remains
    // must be the residual one, not a doubled correction.
    CHECK(transformOf(second).position.x > transformOf(first).position.x);
    CHECK(transformOf(third).position.x > transformOf(second).position.x);
}

void testBodiesDoNotSelfCollide()
{
    // A single body must never be treated as colliding with itself. If it were,
    // the correction would be the box's own full size and it would jump.
    EntityManager manager;
    Input input;
    engine::ecs::SystemManager systems;
    systems.add<PhysicsSystem>();

    Entity& only = makeBody(manager, Vec2{0.0F, 0.0F}, Vec2{40.0F, 40.0F}, BodyType::Dynamic);

    systems.update(manager, input, 1.0F);

    CHECK_NEAR_VEC(transformOf(only).position, Vec2(0.0F, 0.0F));
}

// ---------------------------------------------------------------------------
// Input -> MovementSystem -> PhysicsSystem
// ---------------------------------------------------------------------------

void testMovementThenPhysicsMatchesOldBehaviour()
{
    // The Phase 8 refactor: MovementSystem sets velocity, PhysicsSystem
    // integrates. The observable result must be identical to Phase 7's
    // position += direction * speed * deltaSeconds.
    EntityManager manager;
    Input input;
    engine::ecs::SystemManager systems;
    systems.add<MovementSystem>(100.0F);
    systems.add<PhysicsSystem>(); // registered after: consumes the velocity

    Entity& player = makeBody(manager, Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Dynamic);

    input.processKeyDown(Key::D);
    systems.update(manager, input, 1.0F);

    // Phase 7 would have produced position.x == 100 directly.
    CHECK_NEAR_VEC(transformOf(player).position, Vec2(100.0F, 0.0F));
    CHECK_NEAR(transformOf(player).velocity.x, 100.0F);
}

void testPlayerCannotPassThroughWall()
{
    EntityManager manager;
    Input input;
    engine::ecs::SystemManager systems;
    systems.add<MovementSystem>(300.0F);
    systems.add<PhysicsSystem>(); // registered after: consumes the velocity

    // The scenario from the brief: a wall and a player.
    makeBody(manager, Vec2{400.0F, 300.0F}, Vec2{50.0F, 300.0F}, BodyType::Static);
    Entity& player = makeBody(manager, Vec2{100.0F, 300.0F}, Vec2{50.0F, 50.0F}, BodyType::Dynamic);

    input.processKeyDown(Key::D);

    // Many frames of holding right. The player must be stopped, not tunnelled.
    for (int frame = 0; frame < 200; ++frame)
    {
        systems.update(manager, input, 1.0F / 60.0F);
    }

    // The wall spans x 375..425. The player never gets past its left face.
    CHECK(transformOf(player).position.x < 375.0F);
    CHECK(transformOf(player).position.x > 340.0F);
    // It is still pressing right, so it stays put rather than drifting away.
    CHECK_NEAR(transformOf(player).velocity.x, 0.0F);
}

void testPlayerStaysResponsiveAfterCollision()
{
    EntityManager manager;
    Input input;
    engine::ecs::SystemManager systems;
    systems.add<MovementSystem>(200.0F);
    systems.add<PhysicsSystem>();

    makeBody(manager, Vec2{400.0F, 0.0F}, Vec2{50.0F, 400.0F}, BodyType::Static);
    Entity& player = makeBody(manager, Vec2{100.0F, 0.0F}, Vec2{50.0F, 50.0F}, BodyType::Dynamic);

    // Run into the wall.
    input.processKeyDown(Key::D);
    for (int frame = 0; frame < 100; ++frame)
    {
        systems.update(manager, input, 1.0F / 60.0F);
    }
    const float stoppedAt = transformOf(player).position.x;
    CHECK(stoppedAt < 375.0F);

    // Walk away by pressing the opposite key: releasing D alone would only stop
    // the body, since MovementSystem zeroes velocity when nothing is held.
    input.beginFrame();
    input.processKeyUp(Key::D);
    input.processKeyDown(Key::A);
    for (int frame = 0; frame < 30; ++frame)
    {
        systems.update(manager, input, 1.0F / 60.0F);
    }
    const float backedOff = transformOf(player).position.x;
    CHECK(backedOff < stoppedAt - 1.0F);

    // And it can come back, which is the real test of not being stuck.
    input.beginFrame();
    input.processKeyUp(Key::A);
    input.processKeyDown(Key::D);
    for (int frame = 0; frame < 90; ++frame)
    {
        systems.update(manager, input, 1.0F / 60.0F);
    }
    CHECK(transformOf(player).position.x > backedOff + 1.0F);
    CHECK(transformOf(player).position.x < 375.0F);
}

void testDiagonalMovementStillNormalized()
{
    EntityManager manager;
    Input input;
    engine::ecs::SystemManager systems;
    systems.add<MovementSystem>(100.0F);
    systems.add<PhysicsSystem>();

    Entity& player = makeBody(manager, Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Dynamic);

    input.processKeyDown(Key::W);
    input.processKeyDown(Key::D);
    systems.update(manager, input, 1.0F);

    // Exactly one second of speed, not sqrt(2) times it.
    CHECK_NEAR(transformOf(player).position.length(), 100.0F);
    CHECK_NEAR(transformOf(player).velocity.length(), 100.0F);
}

void testDiagonalCollisionResolvesOnOneAxis()
{
    EntityManager manager;
    Input input;
    engine::ecs::SystemManager systems;
    systems.add<MovementSystem>(200.0F);
    systems.add<PhysicsSystem>();

    // A tall wall, so the player stays within its vertical range for the whole
    // run and keeps being blocked horizontally rather than sliding off the end.
    makeBody(manager, Vec2{400.0F, 0.0F}, Vec2{50.0F, 1000.0F}, BodyType::Static);

    // Started close enough to reach the wall well inside the run.
    Entity& player = makeBody(manager, Vec2{320.0F, 0.0F}, Vec2{50.0F, 50.0F}, BodyType::Dynamic);

    input.processKeyDown(Key::D);
    input.processKeyDown(Key::W);

    for (int frame = 0; frame < 60; ++frame)
    {
        systems.update(manager, input, 1.0F / 60.0F);
    }

    // Stopped horizontally against the wall, which spans x 375..425, so the
    // player's centre rests at 375 - 25.
    CHECK(transformOf(player).position.x < 375.0F);
    CHECK(transformOf(player).position.x > 340.0F);
    CHECK_NEAR(transformOf(player).velocity.x, 0.0F);
    // ...but still climbing vertically, which is the axis it was never blocked
    // on. A diagonal approach must not stop the whole body.
    CHECK(transformOf(player).velocity.y < 0.0F);
    CHECK(transformOf(player).position.y < 0.0F);
}

void testNoInputMeansNoMovement()
{
    EntityManager manager;
    Input input;
    engine::ecs::SystemManager systems;
    systems.add<MovementSystem>(100.0F);
    systems.add<PhysicsSystem>();

    Entity& player = makeBody(manager, Vec2{50.0F, 50.0F}, Vec2{20.0F, 20.0F}, BodyType::Dynamic,
                              Vec2{40.0F, 0.0F});

    systems.update(manager, input, 1.0F);

    // MovementSystem clears the velocity because nothing is held, so physics
    // integrates nothing. The body is not gliding.
    CHECK_NEAR_VEC(transformOf(player).position, Vec2(50.0F, 50.0F));
    CHECK_NEAR_VEC(transformOf(player).velocity, Vec2(0.0F, 0.0F));
}

void testSpeedIndependentOfFrameCount()
{
    Input input;
    input.processKeyDown(Key::D);

    auto travel = [&input](const int frames, const float each)
    {
        EntityManager manager;
        engine::ecs::SystemManager systems;
        systems.add<MovementSystem>(100.0F);
        systems.add<PhysicsSystem>();
        Entity& player = makeBody(manager, Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Dynamic);

        for (int frame = 0; frame < frames; ++frame)
        {
            systems.update(manager, input, each);
        }
        return transformOf(player).position.x;
    };

    // One second delivered two ways.
    const float one = travel(1, 1.0F);
    const float many = travel(100, 0.01F);

    CHECK_NEAR(one, 100.0F);
    CHECK_NEAR(many, 100.0F);
    CHECK_NEAR(one, many);
}

// ---------------------------------------------------------------------------
// The documented limitation
// ---------------------------------------------------------------------------

void testTunnellingIsPossibleAndUnprevented()
{
    // Phase 8 uses discrete detection, so a fast body can pass through a thin
    // collider in one step. This test exists to pin that behaviour down rather
    // than to suggest it is handled.
    EntityManager manager;
    Input input;
    engine::ecs::SystemManager systems;
    systems.add<PhysicsSystem>();

    makeBody(manager, Vec2{200.0F, 0.0F}, Vec2{2.0F, 200.0F}, BodyType::Static);
    Entity& bullet = makeBody(manager, Vec2{0.0F, 0.0F}, Vec2{10.0F, 10.0F}, BodyType::Dynamic,
                              Vec2{5000.0F, 0.0F});

    systems.update(manager, input, 1.0F);

    // It went clean through. Expected, documented, and not fixed in this phase.
    CHECK(transformOf(bullet).position.x > 200.0F);
}

} // namespace

int main()
{
    const std::pair<const char*, void (*)()> testCases[] = {
        {"aabb construction", &testAabbConstruction},
        {"aabb identical boxes", &testAabbIdenticalBoxes},
        {"aabb separated on x", &testAabbSeparatedOnX},
        {"aabb separated on y", &testAabbSeparatedOnY},
        {"aabb touching edges do not overlap", &testAabbTouchingEdgesDoNotOverlap},
        {"aabb partial overlap", &testAabbPartialOverlap},
        {"aabb complete containment", &testAabbCompleteContainment},
        {"aabb penetration axis", &testAabbPenetrationAxis},
        {"aabb minimum translation direction", &testAabbMinimumTranslationDirection},
        {"aabb zero and negative size", &testAabbZeroAndNegativeSize},
        {"physics integrates velocity", &testPhysicsIntegratesVelocity},
        {"static body never moves", &testStaticBodyNeverMoves},
        {"dynamic stops at static wall", &testDynamicStopsAtStaticWall},
        {"dynamic takes the whole correction against a static", &testDynamicTakesTheWholeCorrectionAgainstAStatic},
        {"static on left pushes dynamic right", &testStaticOnLeftPushesDynamicRight},
        {"vertical collision from above", &testVerticalCollisionFromAbove},
        {"vertical collision from below", &testVerticalCollisionFromBelow},
        {"static versus static does nothing", &testStaticVersusStaticDoesNothing},
        {"dynamic versus dynamic splits correction", &testDynamicVersusDynamicSplitsCorrection},
        {"tangential velocity survives", &testTangentialVelocitySurvives},
        {"moving away keeps velocity", &testMovingAwayKeepsVelocity},
        {"no collision leaves everything alone", &testNoCollisionLeavesEverythingAlone},
        {"dead entities are ignored", &testDeadEntitiesAreIgnored},
        {"entities without collider are ignored", &testEntitiesWithoutColliderAreIgnored},
        {"entity without body is not resolved", &testEntityWithoutBodyIsNotResolved},
        {"each pair resolved once", &testEachPairResolvedOnce},
        {"bodies do not self collide", &testBodiesDoNotSelfCollide},
        {"movement then physics matches old behaviour", &testMovementThenPhysicsMatchesOldBehaviour},
        {"player cannot pass through wall", &testPlayerCannotPassThroughWall},
        {"player stays responsive after collision", &testPlayerStaysResponsiveAfterCollision},
        {"diagonal movement still normalized", &testDiagonalMovementStillNormalized},
        {"diagonal collision resolves on one axis", &testDiagonalCollisionResolvesOnOneAxis},
        {"no input means no movement", &testNoInputMeansNoMovement},
        {"speed independent of frame count", &testSpeedIndependentOfFrameCount},
        {"tunnelling is possible and unprevented", &testTunnellingIsPossibleAndUnprevented},
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

    std::cout << groupCount << " Phase 8 test groups passed\n";
    return EXIT_SUCCESS;
}
