/// The collision API: `getOverlap`, `getPreviousOverlap`, the per-frame report, and
/// the landing/ceiling/side classification built on them.
///
/// ### What is driven and what is observed
///
/// The overlap arithmetic is checked against numbers written out by hand rather than
/// against a second call to the code under test, so a group cannot pass by comparing
/// the implementation with itself. The report is read after a **real** physics step
/// through the real system, because the thing being tested is what the step produces,
/// not what a hand-built record could be made to contain.
///
/// Nothing here is a player. The player is a consumer of this API and is tested as one
/// in `player_test`; keeping the two apart is what makes the claim "this is generic"
/// checkable rather than asserted.
#include "engine/components/Body.hpp"
#include "engine/components/Collider.hpp"
#include "engine/components/Transform.hpp"
#include "engine/ecs/EntityManager.hpp"
#include "engine/ecs/SystemManager.hpp"
#include "engine/input/ActionState.hpp"
#include "engine/math/Vec2.hpp"
#include "engine/physics/Aabb.hpp"
#include "engine/physics/Collision.hpp"
#include "engine/systems/PhysicsSystem.hpp"

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace
{

// ---------------------------------------------------------------------------
// Harness, matching the style of the other suites.
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

void checkNear(const float actual, const float expected, const float tolerance, const char* const expression,
               const char* const file, const int line)
{
    if (std::fabs(actual - expected) > tolerance)
    {
        ++g_failureCount;
        std::cerr << "    " << file << ':' << line << ": CHECK(" << expression << ") failed"
                  << "\n      actual = " << actual << ", expected = " << expected << " +/- " << tolerance << '\n';
    }
}

void checkNearVec(const engine::Vec2 actual, const engine::Vec2 expected, const char* const expression,
                  const char* const file, const int line)
{
    if (std::fabs(actual.x - expected.x) > 0.0001F || std::fabs(actual.y - expected.y) > 0.0001F)
    {
        ++g_failureCount;
        std::cerr << "    " << file << ':' << line << ": CHECK(" << expression << ") failed"
                  << "\n      actual = (" << actual.x << ", " << actual.y << "), expected = (" << expected.x
                  << ", " << expected.y << ")\n";
    }
}

#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)
#define CHECK_FALSE(...) check(!(__VA_ARGS__), "!" #__VA_ARGS__, __FILE__, __LINE__)
#define CHECK_NEAR(actual, expected, tolerance) \
    checkNear((actual), (expected), (tolerance), #actual " ~= " #expected, __FILE__, __LINE__)
#define CHECK_NEAR_VEC(actual, expected) checkNearVec((actual), (expected), #actual, __FILE__, __LINE__)

using engine::Vec2;
using engine::components::Body;
using engine::components::Collider;
using engine::components::Transform;
using engine::ecs::Entity;
using engine::ecs::EntityId;
using engine::ecs::EntityManager;
using engine::ecs::SystemManager;
using engine::input::ActionState;
using engine::physics::Aabb;
using engine::physics::BodyType;
using engine::physics::Collision;
using engine::physics::CollisionReport;
using engine::physics::getOverlap;
using engine::physics::getPreviousOverlap;
using engine::physics::hasStaticPartner;
using engine::physics::hitCeilingWith;
using engine::physics::hitSideWith;
using engine::physics::isParticipant;
using engine::physics::landedOn;
using engine::physics::partnerOf;
using engine::physics::resolutionFor;
using engine::systems::PhysicsSystem;

constexpr float kFrame = 1.0F / 60.0F;

/// A body with a transform, a collider and a body type - the three a collision needs.
Entity& addBody(EntityManager& world, const std::string_view tag, const Vec2 position, const Vec2 size,
                const BodyType type, const Vec2 velocity = Vec2{0.0F, 0.0F})
{
    Entity& entity = world.addEntity(std::string{tag});
    // Four values, exactly as the rest of the engine builds a transform. `prevPosition`
    // is left at {0, 0} on purpose: the physics system must not depend on anybody
    // having initialised it, and a group that quietly set it would hide that.
    entity.addComponent<Transform>(Transform{position, velocity, Vec2{1.0F, 1.0F}, 0.0F});
    entity.addComponent<Collider>(Collider{size});
    entity.addComponent<Body>(Body{type});
    return entity;
}

/// A transform and a collider but **no** body component.
///
/// A shape the level format allows and the loader produces for entities that are drawn
/// and bounded but never resolved. It has a box, so the *functions* can be asked about
/// it, and it takes part in no *collision*, because the physics system queries for
/// `Body` - which is the distinction the absent-box group below pins.
Entity& addBoxWithoutBody(EntityManager& world, const std::string_view tag, const Vec2 position, const Vec2 size)
{
    Entity& entity = world.addEntity(std::string{tag});
    entity.addComponent<Transform>(Transform{position, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
    entity.addComponent<Collider>(Collider{size});
    return entity;
}

/// A collider with no transform: not a box, because a box needs both.
Entity& addColliderWithoutTransform(EntityManager& world, const std::string_view tag, const Vec2 size)
{
    Entity& entity = world.addEntity(std::string{tag});
    entity.addComponent<Collider>(Collider{size});
    return entity;
}

[[nodiscard]] const Collision* findCollisionWith(const CollisionReport& report, const EntityId entity)
{
    for (const Collision& collision : report.collisions())
    {
        if (isParticipant(collision, entity))
        {
            return &collision;
        }
    }
    return nullptr;
}

[[nodiscard]] std::size_t countWith(const CollisionReport& report, const EntityId entity)
{
    std::size_t count = 0U;
    for (const Collision& collision : report.collisions())
    {
        if (isParticipant(collision, entity))
        {
            ++count;
        }
    }
    return count;
}

/// One physics system over one world, so a group can step and then read the report.
class PhysicsFixture
{
public:
    PhysicsFixture() : m_physics{m_systems.add<PhysicsSystem>()} {}

    void step(const float deltaSeconds = kFrame)
    {
        m_systems.update(m_world, ActionState{}, deltaSeconds);
    }

    [[nodiscard]] EntityManager& world() noexcept { return m_world; }
    [[nodiscard]] const CollisionReport& collisions() const noexcept { return m_physics.collisions(); }

private:
    EntityManager m_world;
    SystemManager m_systems;
    const PhysicsSystem& m_physics;
};

// ---------------------------------------------------------------------------
// A. Basic overlap
// ---------------------------------------------------------------------------

void testOverlappingBoxesGivePositivePenetrationOnBothAxes()
{
    // Two 20x20 boxes 10 apart on each axis: the halves sum to 20 on each, so the
    // penetration is 10 on each. Written out rather than delegated to the code.
    EntityManager world;
    Entity& first = addBody(world, "a", Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Static);
    Entity& second = addBody(world, "b", Vec2{10.0F, 10.0F}, Vec2{20.0F, 20.0F}, BodyType::Static);

    const Vec2 overlap = getOverlap(first, second);
    CHECK_NEAR(overlap.x, (10.0F + 10.0F) - 10.0F, 0.0001F);
    CHECK_NEAR(overlap.y, (10.0F + 10.0F) - 10.0F, 0.0001F);
    CHECK(overlap.x > 0.0F);
    CHECK(overlap.y > 0.0F);
}

void testSeparatedBoxesGiveTheNegativeGap()
{
    // 100 apart on x with halves summing to 10 each, so the gap is 90 and the answer is
    // -90. The y axis is not separated at all: both boxes are 20 tall at the same
    // height, so they share all 20 of it. Asserting the gap on one axis and ignoring
    // the other would have hidden which number was which.
    //
    // The negative range is the contract, not an accident. A value clamped at zero
    // would make "separated by 90" and "exactly touching" indistinguishable, and
    // [Aabb::overlaps] - the predicate for "do these touch" - would have nothing left
    // to test.
    EntityManager world;
    Entity& first = addBody(world, "a", Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Static);
    Entity& second = addBody(world, "b", Vec2{100.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Static);

    const Vec2 overlap = getOverlap(first, second);
    CHECK_NEAR(overlap.x, (10.0F + 10.0F) - 100.0F, 0.0001F);
    CHECK_NEAR(overlap.x, -80.0F, 0.0001F);
    CHECK(overlap.x < 0.0F);
    CHECK_NEAR(overlap.y, 20.0F, 0.0001F);
    CHECK(overlap.y > 0.0F);

    // And the predicate agrees with the sign: negative on x means not overlapping.
    const Aabb firstBox{Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}};
    const Aabb secondBox{Vec2{100.0F, 0.0F}, Vec2{20.0F, 20.0F}};
    CHECK_FALSE(firstBox.overlaps(secondBox));
}


void testOverlapOnOneAxisOnlyIsReportedAsSuch()
{
    // Side by side: overlapping on y, apart on x. The case a platformer is full of -
    // a player standing on a tile overlaps it vertically and not horizontally.
    EntityManager world;
    Entity& first = addBody(world, "a", Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Static);
    Entity& second = addBody(world, "b", Vec2{30.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Static);

    const Vec2 overlap = getOverlap(first, second);
    CHECK(overlap.x < 0.0F);
    CHECK_NEAR(overlap.y, 20.0F, 0.0001F);
    CHECK(overlap.y > 0.0F);

    // Stacked: the other way round, which is the landing case.
    Entity& third = addBody(world, "c", Vec2{0.0F, 30.0F}, Vec2{20.0F, 20.0F}, BodyType::Static);
    const Vec2 stacked = getOverlap(first, third);
    CHECK(stacked.x > 0.0F);
    CHECK(stacked.y < 0.0F);
}

void testTouchingIsExactlyZeroAndIsNotACollision()
{
    // Two 20-wide boxes with centres 20 apart touch exactly. The answer is zero, and
    // the brief is explicit that touching must not be treated as penetration - the
    // course's own arithmetic makes touching zero, and the engine's `overlaps` is
    // strict.
    EntityManager world;
    Entity& first = addBody(world, "a", Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Static);
    Entity& second = addBody(world, "b", Vec2{20.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Static);

    const Vec2 overlap = getOverlap(first, second);
    CHECK_NEAR(overlap.x, 0.0F, 0.0001F);
    CHECK(overlap.x > 0.0F == false);

    const Aabb firstBox{Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}};
    const Aabb secondBox{Vec2{20.0F, 0.0F}, Vec2{20.0F, 20.0F}};
    CHECK_FALSE(firstBox.overlaps(secondBox));

    // And no collision is reported for a pair that only touches, which is the property
    // the whole resting-contact design depends on: a player is never "supported" by
    // merely being next to something.
    PhysicsFixture fixture;
    Entity& a = addBody(fixture.world(), "a", Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Static);
    Entity& b = addBody(fixture.world(), "b", Vec2{20.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Dynamic);
    fixture.step();
    CHECK(fixture.collisions().empty());
    CHECK(findCollisionWith(fixture.collisions(), a.id()) == nullptr);
    CHECK(findCollisionWith(fixture.collisions(), b.id()) == nullptr);
}

void testContainmentIsWherePenetrationDiffersFromTheSharedExtent()
{
    // The one case where the course's `getOverlap` and a shared-extent reading come
    // apart, and the reason [Aabb::penetration] exists as its own function.
    //
    //   a   20 wide, half 10, centred at x = 0   -> spans -10..10
    //   b    6 wide, half  3, centred at x = 4   -> spans   1..7
    //
    // Shared extent: the run 1..7 is 6 wide. Penetrating b out of a takes 9 - push it
    // to x = 13 and the boxes meet at 10. Pushing by the shared extent would leave
    // them overlapping by 3.
    EntityManager world;
    Entity& big = addBody(world, "big", Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Static);
    Entity& small = addBody(world, "small", Vec2{4.0F, 0.0F}, Vec2{6.0F, 6.0F}, BodyType::Static);

    const Vec2 overlap = getOverlap(big, small);
    CHECK_NEAR(overlap.x, (10.0F + 3.0F) - 4.0F, 0.0001F);
    CHECK_NEAR(overlap.x, 9.0F, 0.0001F);

    // The old quantity, still available and still correct for its own question.
    const Aabb bigBox{Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}};
    const Aabb smallBox{Vec2{4.0F, 0.0F}, Vec2{6.0F, 6.0F}};
    CHECK_NEAR(bigBox.overlapX(smallBox), 6.0F, 0.0001F);
    CHECK_NEAR(bigBox.penetration(smallBox).x, 9.0F, 0.0001F);

    // And the consequence: the resolution pushes by the penetration, so the nested box
    // comes all the way out. Push b out along x and it should end exactly touching.
    PhysicsFixture fixture;
    addBody(fixture.world(), "big", Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Static);
    Entity& nested = addBody(fixture.world(), "nested", Vec2{4.0F, 0.0F}, Vec2{6.0F, 6.0F}, BodyType::Dynamic);
    fixture.step();

    // b's centre moves from 4 to 13, and 13 - 3 == 10 == a's right edge: touching, and
    // therefore not overlapping. Pushing by the shared extent would have left it at
    // 10, still 3 inside.
    CHECK_NEAR(nested.getComponent<Transform>().position.x, 13.0F, 0.001F);
    const Aabb resolved{nested.getComponent<Transform>().position, nested.getComponent<Collider>().size};
    CHECK_FALSE(resolved.overlaps(Aabb{Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}}));
}

void testReversedArgumentOrderGivesTheSameGeometry()
{
    // The pair is unordered, so the numbers must not depend on which entity is named
    // first. A sign error anywhere in the arithmetic would show up here.
    EntityManager world;
    Entity& first = addBody(world, "a", Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Static);
    Entity& second = addBody(world, "b", Vec2{-13.0F, 7.0F}, Vec2{14.0F, 8.0F}, BodyType::Static);

    const Vec2 forwards = getOverlap(first, second);
    const Vec2 backwards = getOverlap(second, first);
    CHECK_NEAR_VEC(forwards, backwards);
    CHECK_NEAR_VEC(getPreviousOverlap(first, second), getPreviousOverlap(second, first));
}

void testADegenerateBoxNeverOverlaps()
{
    // A zero-size collider is a point. Phase 8 established that it never overlaps
    // anything, and this group keeps that - but it also pins the *disagreement* between
    // the two quantities for this case, because it is surprising and a reader will
    // trip over it.
    //
    //   the shared extent on x is  min(0, 10) - max(0, -10) = 0
    //   the penetration depth is  (0 + 10) - |0 - 0|        = 10
    //
    // Both are right, and they answer different questions. The extent is "how much of
    // this axis do the boxes share", and a point shares none of it. The depth is "how
    // far must this move to merely touch", and the point is 10 from the boundary. So
    // the penetration is positive while the overlap is zero - and `overlaps` is strict,
    // so no collision is reported.
    //
    // That is the safe direction. A point collider that reported a collision would
    // resolve a zero-width body against a floor and shove it sideways for ever.
    EntityManager world;
    Entity& point = addBody(world, "point", Vec2{0.0F, 0.0F}, Vec2{0.0F, 0.0F}, BodyType::Static);
    Entity& big = addBody(world, "big", Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Static);

    CHECK_NEAR(getOverlap(point, big).x, 10.0F, 0.0001F);
    CHECK_NEAR(getOverlap(point, big).y, 10.0F, 0.0001F);

    const Aabb pointBox{Vec2{0.0F, 0.0F}, Vec2{0.0F, 0.0F}};
    CHECK_NEAR(pointBox.size().x, 0.0F, 0.0001F);
    CHECK_FALSE(pointBox.overlaps(Aabb{Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}}));
    CHECK_FALSE(Aabb{Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}}.overlaps(pointBox));

    // Through the system: a dynamic point resting on a dynamic box produces no record.
    PhysicsFixture fixture;
    addBody(fixture.world(), "big", Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Static);
    Entity& dot = addBody(fixture.world(), "dot", Vec2{0.0F, 0.0F}, Vec2{0.0F, 0.0F}, BodyType::Dynamic);
    fixture.step();
    CHECK(fixture.collisions().empty());
    CHECK(findCollisionWith(fixture.collisions(), dot.id()) == nullptr);
}


// ---------------------------------------------------------------------------
// B. Previous overlap
// ---------------------------------------------------------------------------

void testPreviousOverlapIsMeasuredAtThePreviousPosition()
{
    // The whole point of the field, stated as a difference: after one step, the current
    // overlap and the previous overlap differ by exactly the distance the body moved.
    //
    // A 20-wide static box at x = 0, and a 20-wide dynamic box that starts at x = 30 -
    // separated by 10 - moving left at 100 px/s. One frame at 1/60 s is 1.667, so it
    // ends at 28.333: the gap is 8.333 and was 10.
    PhysicsFixture fixture;
    addBody(fixture.world(), "wall", Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Static);
    Entity& mover = addBody(fixture.world(), "mover", Vec2{30.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Dynamic,
                            Vec2{-100.0F, 0.0F});
    fixture.step();

    const float moved = 100.0F * kFrame;
    CHECK_NEAR(mover.getComponent<Transform>().position.x, 30.0F - moved, 0.001F);

    // No collision - they never touched - so ask the API directly about the two frames.
    const Entity& wall = *fixture.world().getEntities("wall").begin();
    const Entity& moverView = *fixture.world().getEntities("mover").begin();
    const Vec2 now = getOverlap(wall, moverView);
    const Vec2 before = getPreviousOverlap(wall, moverView);

    CHECK_NEAR(before.x, -10.0F, 0.0001F);
    CHECK_NEAR(now.x, -(10.0F - moved), 0.0001F);
    CHECK_NEAR(now.x - before.x, moved, 0.001F);
}

void testPreviousOverlapIsCorrectOnTheVeryFirstFrame()
{
    // The invariant that makes `prevPosition` safe to leave uninitialised, and the
    // reason the overlap has to be captured before the correction.
    //
    // A 20-wide static box at x = 0 and a 20-wide dynamic box at x = 15: their
    // penetration is (10 + 10) - 15 = 5 on x, and all 20 on y. The dynamic box is then
    // pushed out horizontally by that 5 and ends at x = 20, exactly touching.
    //
    // So *after* the step, reading the current overlap gives 0 - the correction has
    // already destroyed the evidence - while the record still holds 5, and the previous
    // overlap is also 5 because the box started at 15 and did not move of its own
    // accord. All three numbers are asserted, because the whole design rests on their
    // being distinguishable.
    //
    // If the static box's `prevPosition` were still the {0, 0} that brace
    // initialisation gave it, the previous overlap would be measured against a box at
    // the origin and none of this would hold.
    PhysicsFixture fixture;
    Entity& wall = addBody(fixture.world(), "wall", Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Static);
    Entity& mover = addBody(fixture.world(), "mover", Vec2{15.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Dynamic);
    fixture.step();

    const Vec2 before = getPreviousOverlap(wall, mover);
    const Vec2 now = getOverlap(wall, mover);
    CHECK_NEAR(before.x, 5.0F, 0.001F);
    CHECK_NEAR(before.y, 20.0F, 0.001F);

    // Pushed clear, so the current overlap is exactly zero and the shared extent is
    // too: the two boxes now touch.
    CHECK_NEAR(now.x, 0.0F, 0.001F);
    CHECK_FALSE(getOverlap(wall, mover).x > 0.0F);

    // The record kept the pre-correction number, which is the only place it survives.
    const Collision* collision = findCollisionWith(fixture.collisions(), mover.id());
    CHECK(collision != nullptr);
    if (collision != nullptr)
    {
        CHECK_NEAR(collision->overlap.x, 5.0F, 0.001F);
        CHECK_NEAR_VEC(collision->previousOverlap, before);
    }
}


void testPreviousOverlapChangesFrameToFrame()
{
    // A falling body, four frames. The gap shrinks by exactly the distance travelled
    // each frame, and each frame's previous overlap is the frame before's current one -
    // which is what "the position before this step" has to mean for a landing test to
    // work at all.
    //
    // Nothing is read before the first step, and that is deliberate: until physics has
    // run, `prevPosition` is whatever the aggregate initialisation left there, which is
    // exactly the {0, 0} this design has to tolerate rather than initialise.
    PhysicsFixture fixture;
    addBody(fixture.world(), "floor", Vec2{0.0F, 100.0F}, Vec2{200.0F, 20.0F}, BodyType::Static);
    Entity& faller = addBody(fixture.world(), "faller", Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Dynamic,
                             Vec2{0.0F, 50.0F});

    const Entity& floor = *fixture.world().getEntities("floor").begin();
    fixture.step();

    float currentGap = getOverlap(floor, faller).y;
    CHECK(currentGap < 0.0F);

    for (int frame = 0; frame < 3; ++frame)
    {
        fixture.step();

        const Vec2 now = getOverlap(floor, faller);
        const Vec2 before = getPreviousOverlap(floor, faller);

        // Still short of the floor, so this is a gap and not a penetration.
        CHECK(before.y < 0.0F);
        CHECK(now.y < 0.0F);

        // This frame's previous overlap is the previous frame's current overlap.
        CHECK_NEAR(before.y, currentGap, 0.001F);
        // And the gap closed by exactly one frame of travel.
        CHECK_NEAR(now.y - before.y, 50.0F * kFrame, 0.001F);
        currentGap = now.y;
    }
}


void testTheCurrentMovementDoesNotOverwriteThePreviousStateEarly()
{
    // A body that starts exactly touching a floor and falls into it.
    //
    //   floor  400x20 at y = 100, so its top edge is at 90
    //   faller  20x20 at y = 80, so its bottom edge is at 90 - touching, not overlapping
    //   falling at 300 px/s, one frame is 5 pixels
    //
    // So the frame ends with 5 pixels of penetration, which the resolver cancels. If
    // the record were rebuilt after the correction - or read from the corrected
    // position - it would hold 0, and a landing would be indistinguishable from a body
    // that was already resting. Asserted through the system, because that is the only
    // place the ordering is observable.
    PhysicsFixture fixture;
    addBody(fixture.world(), "floor", Vec2{0.0F, 100.0F}, Vec2{400.0F, 20.0F}, BodyType::Static);
    Entity& faller = addBody(fixture.world(), "faller", Vec2{0.0F, 80.0F}, Vec2{20.0F, 20.0F}, BodyType::Dynamic,
                             Vec2{0.0F, 300.0F});
    fixture.step();

    const Collision* collision = findCollisionWith(fixture.collisions(), faller.id());
    CHECK(collision != nullptr);
    if (collision == nullptr)
    {
        return;
    }

    // The penetration the record kept.
    CHECK_NEAR(collision->overlap.y, 300.0F * kFrame, 0.001F);
    CHECK(collision->overlap.y > 0.0F);

    // And the previous overlap says it arrived from above: beside the floor, and not
    // previously overlapping it vertically.
    CHECK(collision->previousOverlap.x > 0.0F);
    CHECK_NEAR(collision->previousOverlap.y, 0.0F, 0.001F);
    CHECK(collision->previousOverlap.y <= 0.0F);

    // The position really was corrected, which is what makes the distinction matter:
    // reading the current overlap now gives nothing at all.
    CHECK_NEAR(getOverlap(*fixture.world().getEntities("floor").begin(), faller).y, 0.0F, 0.001F);
    CHECK_NEAR(faller.getComponent<Transform>().position.y, 80.0F, 0.001F);
    // And the downward velocity the impact carried is gone.
    CHECK_NEAR(faller.getComponent<Transform>().velocity.y, 0.0F, 0.001F);
}


// ---------------------------------------------------------------------------
// C. Absent boxes
// ---------------------------------------------------------------------------

void testAnAbsentColliderIsReportedAsZeroOverlap()
{
    // (0, 0) rather than a number derived from nothing, and the reason is the sign
    // convention: zero means "exactly touching", which is not a collision, so an absent
    // box can never be mistaken for one. Returning a large positive value would make
    // every uncollidable entity read as a floor to a consumer testing `overlap.y > 0`.
    //
    // A missing **Body** is deliberately *not* in this list. A body is Transform plus
    // Collider; a `Body` component says whether it can be pushed, which is the physics
    // system's business and not this function's. So an entity with a box and no `Body`
    // is asked about normally, and simply never collides - asserted at the end.
    EntityManager world;
    Entity& solid = addBody(world, "solid", Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Static);
    Entity& noCollider = world.addEntity("no.collider");
    noCollider.addComponent<Transform>(Transform{Vec2{0.0F, 0.0F}, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
    Entity& noTransform = addColliderWithoutTransform(world, "no.transform", Vec2{20.0F, 20.0F});
    Entity& nothing = world.addEntity("nothing");

    // Every combination, in both argument orders.
    CHECK_NEAR_VEC(getOverlap(solid, noCollider), Vec2(0.0F, 0.0F));
    CHECK_NEAR_VEC(getOverlap(noCollider, solid), Vec2(0.0F, 0.0F));
    CHECK_NEAR_VEC(getOverlap(solid, noTransform), Vec2(0.0F, 0.0F));
    CHECK_NEAR_VEC(getOverlap(noTransform, solid), Vec2(0.0F, 0.0F));
    CHECK_NEAR_VEC(getOverlap(solid, nothing), Vec2(0.0F, 0.0F));
    CHECK_NEAR_VEC(getOverlap(nothing, solid), Vec2(0.0F, 0.0F));
    CHECK_NEAR_VEC(getOverlap(nothing, nothing), Vec2(0.0F, 0.0F));

    // And the previous-frame form agrees, which matters because a consumer compares the
    // two: a disagreement would surface as a landing that never resolves.
    CHECK_NEAR_VEC(getPreviousOverlap(solid, noCollider), Vec2(0.0F, 0.0F));
    CHECK_NEAR_VEC(getPreviousOverlap(noCollider, solid), Vec2(0.0F, 0.0F));
    CHECK_NEAR_VEC(getPreviousOverlap(solid, noTransform), Vec2(0.0F, 0.0F));
    CHECK_NEAR_VEC(getPreviousOverlap(noTransform, solid), Vec2(0.0F, 0.0F));
    CHECK_NEAR_VEC(getPreviousOverlap(nothing, solid), Vec2(0.0F, 0.0F));

    // A box with no `Body` is still a box: the two functions answer for it.
    Entity& bounded = addBoxWithoutBody(world, "bounded", Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F});
    CHECK(getOverlap(solid, bounded).x > 0.0F);

    // But it is in no collision, because the physics system requires a `Body`.
    PhysicsFixture fixture;
    addBody(fixture.world(), "wall", Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Static);
    Entity& mover = addBody(fixture.world(), "mover", Vec2{10.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Dynamic);
    Entity& ghost = addBoxWithoutBody(fixture.world(), "ghost", Vec2{15.0F, 0.0F}, Vec2{20.0F, 20.0F});
    fixture.step();

    // One collision: wall with mover. The ghost overlaps the mover and the wall
    // geometrically and is in neither record.
    CHECK(fixture.collisions().size() == 1U);
    CHECK(findCollisionWith(fixture.collisions(), ghost.id()) == nullptr);
    CHECK(findCollisionWith(fixture.collisions(), mover.id()) != nullptr);
}

void testADestroyedEntityIsTreatedAsAbsent()
{
    // Deferred destruction: the entity is still in storage but not alive, and reading
    // its components would be reading a corpse.
    EntityManager world;
    Entity& solid = addBody(world, "solid", Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Static);
    Entity& doomed = addBody(world, "doomed", Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Static);

    // Alive, it overlaps.
    CHECK(getOverlap(solid, doomed).x > 0.0F);

    world.destroyEntity(doomed);
    world.update();

    CHECK_FALSE(doomed.isAlive());
    CHECK_NEAR_VEC(getOverlap(solid, doomed), Vec2(0.0F, 0.0F));
    CHECK_NEAR_VEC(getPreviousOverlap(solid, doomed), Vec2(0.0F, 0.0F));
}

void testARemovedColliderIsTreatedAsAbsent()
{
    // A component removed mid-life. `removeComponent` throws if it is not there, so
    // this is a legal sequence rather than a contrived one.
    EntityManager world;
    Entity& solid = addBody(world, "solid", Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Static);
    Entity& stripped = addBody(world, "stripped", Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Static);

    CHECK(getOverlap(solid, stripped).x > 0.0F);
    stripped.removeComponent<Collider>();
    CHECK_NEAR_VEC(getOverlap(solid, stripped), Vec2(0.0F, 0.0F));

    // And removing it mid-flight stops it being collided with, which is the same
    // contract observed through the system rather than the function.
    PhysicsFixture fixture;
    addBody(fixture.world(), "wall", Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Static);
    Entity& victim = addBody(fixture.world(), "victim", Vec2{10.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Dynamic);
    fixture.step();
    CHECK(fixture.collisions().size() == 1U);

    victim.removeComponent<Collider>();
    victim.getComponent<Transform>().position = Vec2{10.0F, 0.0F};
    fixture.step();
    CHECK(fixture.collisions().empty());
}

// ---------------------------------------------------------------------------
// D. Participants
// ---------------------------------------------------------------------------

void testTheRecordNamesBothParticipantsAndNothingElse()
{
    PhysicsFixture fixture;
    Entity& wall = addBody(fixture.world(), "wall", Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Static);
    Entity& mover = addBody(fixture.world(), "mover", Vec2{15.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Dynamic);
    Entity& bystander = addBody(fixture.world(), "bystander", Vec2{500.0F, 500.0F}, Vec2{20.0F, 20.0F},
                                BodyType::Dynamic);
    fixture.step();

    CHECK(fixture.collisions().size() == 1U);
    const Collision* collision = findCollisionWith(fixture.collisions(), wall.id());
    CHECK(collision != nullptr);
    if (collision == nullptr)
    {
        return;
    }

    // Both participants, and the bystander is in neither.
    CHECK(isParticipant(*collision, wall.id()));
    CHECK(isParticipant(*collision, mover.id()));
    CHECK_FALSE(isParticipant(*collision, bystander.id()));
    CHECK(partnerOf(*collision, wall.id()) == mover.id());
    CHECK(partnerOf(*collision, mover.id()) == wall.id());
    CHECK(partnerOf(*collision, bystander.id()) == engine::ecs::kInvalidEntityId);

    // The bystander is in no collision at all, so nothing about it reaches the report.
    CHECK(findCollisionWith(fixture.collisions(), bystander.id()) == nullptr);
    CHECK(countWith(fixture.collisions(), bystander.id()) == 0U);
}

void testTwoStaticBodiesAreNotRecorded()
{
    // The report is "collisions found *and resolved*". Two immovable bodies cannot be
    // resolved, so there is nothing to report - and a reader that wanted to know
    // whether they overlap asks `getOverlap`, which answers for any pair.
    PhysicsFixture fixture;
    addBody(fixture.world(), "a", Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Static);
    addBody(fixture.world(), "b", Vec2{10.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Static);
    fixture.step();

    CHECK(fixture.collisions().empty());
    CHECK(getOverlap(*fixture.world().getEntities("a").begin(), *fixture.world().getEntities("b").begin()).x > 0.0F);
}

void testADestroyedEntityDoesNotSurviveIntoTheNextFrame()
{
    // The lifetime rule: a record describes the frame that produced it, and a consumer
    // that reads a stale one would ground a player who is in the air.
    PhysicsFixture fixture;
    addBody(fixture.world(), "wall", Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Static);
    Entity& mover = addBody(fixture.world(), "mover", Vec2{15.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Dynamic);
    fixture.step();
    CHECK(fixture.collisions().size() == 1U);

    // Teleport it away and destroy it. The next frame must not mention it.
    mover.getComponent<Transform>().position = Vec2{900.0F, 900.0F};
    fixture.world().destroyEntity(mover);
    fixture.world().update();
    fixture.step();

    CHECK(fixture.collisions().empty());
    CHECK(findCollisionWith(fixture.collisions(), mover.id()) == nullptr);
}

void testTheReportIsEmptyBeforeAnythingHasRun()
{
    // A world where physics has never run must not report the last thing that happened
    // to it. This is what clearing at the top of the step buys, and it is why a lazily
    // cleared report would be a bug.
    PhysicsFixture fixture;
    CHECK(fixture.collisions().empty());
    CHECK(fixture.collisions().size() == 0U);
    CHECK(fixture.collisions().collisions().empty());
}

void testACollisionFromOneFrameIsGoneInTheNext()
{
    PhysicsFixture fixture;
    addBody(fixture.world(), "wall", Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Static);
    Entity& mover = addBody(fixture.world(), "mover", Vec2{15.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Dynamic);
    fixture.step();
    const std::size_t first = fixture.collisions().size();
    CHECK(first == 1U);

    // Pushed clear, and the next frame reports nothing at all.
    mover.getComponent<Transform>().position = Vec2{900.0F, 900.0F};
    fixture.step();
    CHECK(fixture.collisions().size() == 0U);
}

void testTwoPhysicsSystemsKeepIndependentReports()
{
    // The report is a member, not a singleton, so a process with two worlds - or a
    // scene and a test's own world - cannot see each other's collisions.
    PhysicsFixture first;
    PhysicsFixture second;

    addBody(first.world(), "wall", Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Static);
    addBody(first.world(), "mover", Vec2{15.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Dynamic);

    addBody(second.world(), "wall", Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Static);
    addBody(second.world(), "mover", Vec2{15.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Dynamic);

    // A second, far away overlapping pair, so the second world has strictly more to
    // report. Far enough that neither of its bodies can also touch the first pair -
    // otherwise the count would depend on how many pairs happen to intersect rather
    // than on the two worlds being independent.
    addBody(second.world(), "far.wall", Vec2{500.0F, 500.0F}, Vec2{20.0F, 20.0F}, BodyType::Static);
    addBody(second.world(), "far.mover", Vec2{515.0F, 500.0F}, Vec2{20.0F, 20.0F}, BodyType::Dynamic);

    first.step();
    second.step();

    CHECK(first.collisions().size() == 1U);
    CHECK(second.collisions().size() == 2U);
}

// ---------------------------------------------------------------------------
// E. Reading a record relative to one participant
// ---------------------------------------------------------------------------

void testResolutionAndBodyAreOrientedToWhoeverIsAsking()
{
    // The record stores one vector and two body types for a *pair*, and the iteration
    // order is arbitrary. So a consumer must be able to ask "what happened to me"
    // without knowing whether it was visited first.
    PhysicsFixture fixture;
    // Two dynamic bodies, so the pair is split evenly and both move - the case where a
    // single stored vector is most obviously ambiguous.
    Entity& upper = addBody(fixture.world(), "upper", Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Dynamic);
    Entity& lower = addBody(fixture.world(), "lower", Vec2{0.0F, 15.0F}, Vec2{20.0F, 20.0F}, BodyType::Dynamic);
    fixture.step();

    const Collision* collision = findCollisionWith(fixture.collisions(), upper.id());
    CHECK(collision != nullptr);
    if (collision == nullptr)
    {
        return;
    }

    // The two ask for the same collision and get opposite pushes, because they moved
    // in opposite directions.
    const Vec2 forUpper = resolutionFor(*collision, upper.id());
    const Vec2 forLower = resolutionFor(*collision, lower.id());
    CHECK_NEAR_VEC(forUpper, -forLower);
    CHECK_NEAR(std::fabs(forUpper.y), std::fabs(forLower.y), 0.001F);

    // And each is told the push in the direction it actually went: the upper body up,
    // the lower body down.
    CHECK(forUpper.y < 0.0F);
    CHECK(forLower.y > 0.0F);
    CHECK(upper.getComponent<Transform>().position.y < 0.0F);
    CHECK(lower.getComponent<Transform>().position.y > 15.0F);

    // The static-partner rule is about the *other* one, and neither of two dynamic
    // bodies is a floor.
    CHECK_FALSE(hasStaticPartner(*collision, upper.id()));
    CHECK_FALSE(hasStaticPartner(*collision, lower.id()));
}

void testTheStaticPartnerRuleNamesTheOtherParticipant()
{
    PhysicsFixture fixture;
    Entity& floor = addBody(fixture.world(), "floor", Vec2{0.0F, 20.0F}, Vec2{200.0F, 20.0F}, BodyType::Static);
    Entity& faller = addBody(fixture.world(), "faller", Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Dynamic,
                             Vec2{0.0F, 100.0F});
    fixture.step();

    const Collision* collision = findCollisionWith(fixture.collisions(), faller.id());
    CHECK(collision != nullptr);
    if (collision == nullptr)
    {
        return;
    }

    // For the falling body the partner is the static floor, so it has one.
    CHECK(hasStaticPartner(*collision, faller.id()));
    // For the floor the partner is dynamic, so it does not. Asking from the wrong side
    // is a real possibility - the iteration order decides who is `first` - and this is
    // what stops a floor being treated as a floor by itself.
    CHECK_FALSE(hasStaticPartner(*collision, floor.id()));
}

// ---------------------------------------------------------------------------
// F. The three outcomes partition every collision
// ---------------------------------------------------------------------------

void testLandingCeilingAndSidePartitionEveryCollision()
{
    // Stated as an invariant over every collision the system produces, over a scene
    // that contains all three kinds at once. If the three predicates were edited so
    // that a wall could read as a landing, or a landing as both, this fails.
    PhysicsFixture fixture;

    // Three surfaces, each placed so a body can reach it in exactly one frame at the
    // speed given, and far enough apart that no body can touch two of them.
    //
    //   floor    400x20 at y = 200, so its top edge is 190; the faller starts at 175
    //             and falls 10, ending with its bottom at 195 - 5 into the floor
    //   ceiling   20x20 at y = 0, so its underside is 10; the riser starts at 30 and
    //             rises 15, ending with its top at 5 - 5 into the ceiling
    //   wall      20x400 at x = 800; the runner starts at 780 and runs 15, ending
    //             5 into the wall while already deep inside its vertical extent
    addBody(fixture.world(), "floor", Vec2{0.0F, 200.0F}, Vec2{400.0F, 20.0F}, BodyType::Static);
    addBody(fixture.world(), "ceiling", Vec2{450.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Static);
    addBody(fixture.world(), "wall", Vec2{800.0F, 0.0F}, Vec2{20.0F, 400.0F}, BodyType::Static);

    Entity& faller = addBody(fixture.world(), "faller", Vec2{100.0F, 175.0F}, Vec2{20.0F, 20.0F}, BodyType::Dynamic,
                             Vec2{0.0F, 600.0F});
    Entity& riser = addBody(fixture.world(), "riser", Vec2{450.0F, 30.0F}, Vec2{20.0F, 20.0F}, BodyType::Dynamic,
                            Vec2{0.0F, -900.0F});
    Entity& runner = addBody(fixture.world(), "runner", Vec2{780.0F, 100.0F}, Vec2{20.0F, 20.0F}, BodyType::Dynamic,
                             Vec2{600.0F, 0.0F});
    fixture.step();

    std::size_t landings = 0U;
    std::size_t ceilings = 0U;
    std::size_t sides = 0U;
    for (const Collision& collision : fixture.collisions().collisions())
    {
        for (const EntityId entity : {faller.id(), riser.id(), runner.id()})
        {
            if (!isParticipant(collision, entity))
            {
                continue;
            }

            const bool landed = landedOn(collision, entity);
            const bool ceiling = hitCeilingWith(collision, entity);
            const bool side = hitSideWith(collision, entity);

            // Exactly one of the three, never two and never none. That is the whole
            // claim, and it is why `hitSideWith` is written as the negation of the other
            // two rather than as a test of its own.
            CHECK((static_cast<int>(landed) + static_cast<int>(ceiling) + static_cast<int>(side)) == 1);
            if (landed)
            {
                ++landings;
            }
            if (ceiling)
            {
                ++ceilings;
            }
            if (side)
            {
                ++sides;
            }
        }
    }

    CHECK(landings == 1U);
    CHECK(ceilings == 1U);
    CHECK(sides == 1U);
}

void testLandingRequiresAStaticPartner()
{
    // The course says the player lands on a **Tile**. A dynamic body is not a tile, and
    // a player that came to rest on a falling bullet would be two bodies pushing one
    // another forever.
    PhysicsFixture fixture;
    Entity& platform = addBody(fixture.world(), "platform", Vec2{0.0F, 20.0F}, Vec2{200.0F, 20.0F},
                               BodyType::Dynamic, Vec2{0.0F, 100.0F});
    Entity& faller = addBody(fixture.world(), "faller", Vec2{0.0F, 0.0F}, Vec2{20.0F, 20.0F}, BodyType::Dynamic,
                             Vec2{0.0F, 300.0F});
    fixture.step();

    const Collision* collision = findCollisionWith(fixture.collisions(), faller.id());
    CHECK(collision != nullptr);
    if (collision == nullptr)
    {
        return;
    }

    // The numbers say everything a landing needs - beside it, not previously overlapping
    // it, now penetrating, pushed up - and the one thing missing is a static partner.
    CHECK(collision->previousOverlap.x > 0.0F);
    CHECK(collision->previousOverlap.y <= 0.0F);
    CHECK(collision->overlap.y > 0.0F);
    CHECK(resolutionFor(*collision, faller.id()).y < 0.0F);
    CHECK_FALSE(landedOn(*collision, faller.id()));
    CHECK_FALSE(hasStaticPartner(*collision, faller.id()));

    // So it falls through to "not a landing", which is the honest classification.
    CHECK(hitSideWith(*collision, faller.id()));
    static_cast<void>(platform);
}

void testAWallIsNotALanding()
{
    // The single most important negative in the phase, isolated: a body driven sideways
    // into a tall box is already inside its vertical extent, so the previous vertical
    // overlap is positive and the landing test must be refused.
    PhysicsFixture fixture;
    addBody(fixture.world(), "wall", Vec2{100.0F, 0.0F}, Vec2{20.0F, 400.0F}, BodyType::Static);
    // The wall spans x 90..110 and is 400 tall, so a runner at y = 100 is already deep
    // inside its vertical extent before it moves - which is the whole point of the case.
    // 900 px/s for one frame is 15, so a runner starting at 70 ends at 85 with its right
    // edge at 95: five pixels into the wall.
    Entity& runner = addBody(fixture.world(), "runner", Vec2{70.0F, 100.0F}, Vec2{20.0F, 20.0F}, BodyType::Dynamic,
                             Vec2{900.0F, 0.0F});
    fixture.step();

    const Collision* collision = findCollisionWith(fixture.collisions(), runner.id());
    CHECK(collision != nullptr);
    if (collision == nullptr)
    {
        return;
    }

    // It arrived horizontally: the horizontal overlap was negative before the step.
    CHECK(collision->previousOverlap.x <= 0.0F);
    // And it was already inside the wall's vertical extent, which is the clause that
    // refuses the landing.
    CHECK(collision->previousOverlap.y > 0.0F);
    CHECK(collision->overlap.y > 0.0F);

    CHECK_FALSE(landedOn(*collision, runner.id()));
    CHECK_FALSE(hitCeilingWith(*collision, runner.id()));
    CHECK(hitSideWith(*collision, runner.id()));
}

} // namespace

int main()
{
    const std::pair<const char*, void (*)()> testCases[] = {
        // A. Basic overlap
        {"overlapping boxes give positive penetration on both axes",
         &testOverlappingBoxesGivePositivePenetrationOnBothAxes},
        {"separated boxes give the negative gap", &testSeparatedBoxesGiveTheNegativeGap},
        {"overlap on one axis only is reported as such", &testOverlapOnOneAxisOnlyIsReportedAsSuch},
        {"touching is exactly zero and is not a collision", &testTouchingIsExactlyZeroAndIsNotACollision},
        {"containment is where penetration differs from the shared extent",
         &testContainmentIsWherePenetrationDiffersFromTheSharedExtent},
        {"reversed argument order gives the same geometry", &testReversedArgumentOrderGivesTheSameGeometry},
        {"a degenerate box never overlaps", &testADegenerateBoxNeverOverlaps},
        // B. Previous overlap
        {"previous overlap is measured at the previous position",
         &testPreviousOverlapIsMeasuredAtThePreviousPosition},
        {"previous overlap is correct on the very first frame",
         &testPreviousOverlapIsCorrectOnTheVeryFirstFrame},
        {"previous overlap changes frame to frame", &testPreviousOverlapChangesFrameToFrame},
        {"the current movement does not overwrite the previous state early",
         &testTheCurrentMovementDoesNotOverwriteThePreviousStateEarly},
        // C. Absent boxes
        {"an absent collider is reported as zero overlap", &testAnAbsentColliderIsReportedAsZeroOverlap},
        {"a destroyed entity is treated as absent", &testADestroyedEntityIsTreatedAsAbsent},
        {"a removed collider is treated as absent", &testARemovedColliderIsTreatedAsAbsent},
        // D. Participants
        {"the record names both participants and nothing else",
         &testTheRecordNamesBothParticipantsAndNothingElse},
        {"two static bodies are not recorded", &testTwoStaticBodiesAreNotRecorded},
        {"a destroyed entity does not survive into the next frame",
         &testADestroyedEntityDoesNotSurviveIntoTheNextFrame},
        {"the report is empty before anything has run", &testTheReportIsEmptyBeforeAnythingHasRun},
        {"a collision from one frame is gone in the next", &testACollisionFromOneFrameIsGoneInTheNext},
        {"two physics systems keep independent reports", &testTwoPhysicsSystemsKeepIndependentReports},
        // E. Reading a record
        {"resolution and body are oriented to whoever is asking",
         &testResolutionAndBodyAreOrientedToWhoeverIsAsking},
        {"the static partner rule names the other participant",
         &testTheStaticPartnerRuleNamesTheOtherParticipant},
        // F. The three outcomes
        {"landing, ceiling and side partition every collision",
         &testLandingCeilingAndSidePartitionEveryCollision},
        {"landing requires a static partner", &testLandingRequiresAStaticPartner},
        {"a wall is not a landing", &testAWallIsNotALanding},
    };

    int failedGroups = 0;

    for (const auto& [name, testCase] : testCases)
    {
        const int failuresBefore = g_failureCount;

        try
        {
            testCase();
        }
        catch (const std::exception& error)
        {
            ++g_failureCount;
            std::cerr << "    unexpected exception escaped group \"" << name << "\": " << error.what() << '\n';
        }

        const bool passed = g_failureCount == failuresBefore;
        if (!passed)
        {
            ++failedGroups;
        }

        std::cout << (passed ? "  PASS  " : "  FAIL  ") << name << '\n';
    }

    const std::size_t groupCount = sizeof(testCases) / sizeof(testCases[0]);

    if (g_failureCount != 0)
    {
        std::cerr << g_failureCount << " check(s) failed across " << failedGroups << " of " << groupCount
                  << " test groups\n";
        return EXIT_FAILURE;
    }

    std::cout << groupCount << " collision test groups passed\n";
    return EXIT_SUCCESS;
}
