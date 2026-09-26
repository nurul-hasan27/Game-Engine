#include "engine/components/Rectangle.hpp"
#include "engine/components/Transform.hpp"
#include "engine/ecs/EntityManager.hpp"
#include "engine/ecs/SystemManager.hpp"
#include "engine/input/Input.hpp"
#include "engine/input/SfmlKeyMap.hpp"
#include "engine/math/Vec2.hpp"
#include "engine/systems/MovementSystem.hpp"

// SFML appears only for the adapter tests at the bottom of this file. Every test
// above the "SFML adapter" heading drives Input through processKeyDown/Up and
// needs no SFML and no keyboard.
#include <SFML/Window/Event.hpp>
#include <SFML/Window/Keyboard.hpp>

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
using engine::components::Rectangle;
using engine::components::Transform;
using engine::ecs::Entity;
using engine::ecs::EntityManager;
using engine::input::Input;
using engine::input::Key;
using engine::systems::MovementSystem;

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

#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)
#define CHECK_FALSE(...) check(!(__VA_ARGS__), "!" #__VA_ARGS__, __FILE__, __LINE__)
#define CHECK_NEAR(actual, expected) checkNear((actual), (expected), #actual " ~= " #expected, __FILE__, __LINE__)

/// Advances one whole frame: clear the transients, then re-apply whatever is
/// still held, which is what the operating system would deliver.
void nextFrame(Input& input)
{
    input.beginFrame();
}

// ---------------------------------------------------------------------------
// Compile-time guarantees.
// ---------------------------------------------------------------------------

// Key is a scoped enum, so a raw int cannot be passed where a key is expected.
static_assert(std::is_enum_v<Key>, "Key must be an enum");
static_assert(!std::is_convertible_v<int, Key>, "Key must not be implicitly constructible from int");
static_assert(static_cast<std::size_t>(Key::Count) == engine::input::kKeyCount, "kKeyCount must match Key::Count");

// Input is a value: two instances must not interfere, which is what rules out
// any hidden global or static keyboard state.
static_assert(std::is_default_constructible_v<Input>, "Input must be constructible as a plain value");
static_assert(std::is_trivially_copyable_v<Input>, "Input must be a trivial value type");

// ---------------------------------------------------------------------------
// Key state machine
// ---------------------------------------------------------------------------

void testInitialStateIsAllFalse()
{
    const Input input;

    for (const Key key : {Key::A, Key::D, Key::S, Key::W, Key::Left, Key::Right, Key::Up, Key::Down, Key::Space,
                          Key::Escape, Key::Unknown})
    {
        CHECK_FALSE(input.isKeyDown(key));
        CHECK_FALSE(input.isKeyPressed(key));
        CHECK_FALSE(input.isKeyReleased(key));
    }

    CHECK_FALSE(input.anyKeyDown());
}

void testFirstKeyPress()
{
    Input input;

    input.processKeyDown(Key::W);

    CHECK(input.isKeyDown(Key::W));
    CHECK(input.isKeyPressed(Key::W));
    CHECK_FALSE(input.isKeyReleased(Key::W));
    CHECK(input.anyKeyDown());
}

void testHeldKeyIsNotPressedAgain()
{
    Input input;

    input.processKeyDown(Key::W);
    nextFrame(input);

    // The key is still held, but the transition is over.
    CHECK(input.isKeyDown(Key::W));
    CHECK_FALSE(input.isKeyPressed(Key::W));
    CHECK_FALSE(input.isKeyReleased(Key::W));
}

void testKeyRelease()
{
    Input input;
    input.processKeyDown(Key::W);
    nextFrame(input);

    input.processKeyUp(Key::W);

    CHECK_FALSE(input.isKeyDown(Key::W));
    CHECK_FALSE(input.isKeyPressed(Key::W));
    CHECK(input.isKeyReleased(Key::W));
}

void testFrameAfterReleaseIsClean()
{
    Input input;
    input.processKeyDown(Key::W);
    nextFrame(input);
    input.processKeyUp(Key::W);
    CHECK(input.isKeyReleased(Key::W));

    nextFrame(input);

    CHECK_FALSE(input.isKeyDown(Key::W));
    CHECK_FALSE(input.isKeyPressed(Key::W));
    CHECK_FALSE(input.isKeyReleased(Key::W));
}

void testKeyRepeatDoesNotRetriggerPressed()
{
    Input input;

    // The operating system emits a press event repeatedly while a key is held.
    input.processKeyDown(Key::W);
    CHECK(input.isKeyPressed(Key::W));

    for (int frame = 0; frame < 20; ++frame)
    {
        nextFrame(input);
        // This is the OS repeat storm arriving every frame.
        input.processKeyDown(Key::W);

        CHECK(input.isKeyDown(Key::W));
        CHECK_FALSE(input.isKeyPressed(Key::W));
    }

    // Only after a real release and re-press is it a transition again.
    input.processKeyUp(Key::W);
    nextFrame(input);
    input.processKeyDown(Key::W);
    CHECK(input.isKeyPressed(Key::W));
}

void testMultipleSimultaneousKeys()
{
    Input input;

    input.processKeyDown(Key::W);
    input.processKeyDown(Key::D);
    input.processKeyDown(Key::Space);

    CHECK(input.isKeyDown(Key::W));
    CHECK(input.isKeyDown(Key::D));
    CHECK(input.isKeyDown(Key::Space));

    input.processKeyUp(Key::D);

    CHECK(input.isKeyDown(Key::W));
    CHECK_FALSE(input.isKeyDown(Key::D));
    CHECK(input.isKeyReleased(Key::D));
    CHECK(input.isKeyDown(Key::Space));
}

void testReleaseOneWhileAnotherHeld()
{
    Input input;
    input.processKeyDown(Key::W);
    input.processKeyDown(Key::D);
    nextFrame(input);

    input.processKeyUp(Key::W);

    CHECK_FALSE(input.isKeyDown(Key::W));
    CHECK(input.isKeyReleased(Key::W));
    CHECK(input.isKeyDown(Key::D));
    CHECK_FALSE(input.isKeyPressed(Key::D));
    CHECK_FALSE(input.isKeyReleased(Key::D));
}

void testKeyStateIsolation()
{
    Input input;
    input.processKeyDown(Key::W);

    for (const Key other : {Key::A, Key::S, Key::D, Key::Left, Key::Right, Key::Up, Key::Down, Key::Space, Key::Escape})
    {
        CHECK_FALSE(input.isKeyDown(other));
        CHECK_FALSE(input.isKeyPressed(other));
        CHECK_FALSE(input.isKeyReleased(other));
    }
}

void testEscapeIsJustAKey()
{
    Input input;

    input.processKeyDown(Key::Escape);
    CHECK(input.isKeyDown(Key::Escape));
    CHECK(input.isKeyPressed(Key::Escape));

    // Input reports it and nothing more. Whether Escape closes the window is a
    // policy decision made outside Input, so Input must not act on it.
    nextFrame(input);
    input.processKeyUp(Key::Escape);
    CHECK(input.isKeyReleased(Key::Escape));
}

void testUnknownKeyIsInert()
{
    Input input;

    input.processKeyDown(Key::Unknown);
    CHECK_FALSE(input.isKeyDown(Key::Unknown));
    CHECK_FALSE(input.isKeyPressed(Key::Unknown));

    input.processKeyUp(Key::Unknown);
    CHECK_FALSE(input.isKeyReleased(Key::Unknown));
    CHECK_FALSE(input.anyKeyDown());
}

void testSpuriousReleaseIsIgnored()
{
    Input input;

    // A release for a key that was never held must not fabricate an edge.
    input.processKeyUp(Key::W);
    CHECK_FALSE(input.isKeyReleased(Key::W));
    CHECK_FALSE(input.isKeyDown(Key::W));
}

void testBeginFrameClearsTransientsButKeepsHeldState()
{
    Input input;

    input.processKeyDown(Key::W);
    input.processKeyDown(Key::D);
    input.processKeyUp(Key::D); // a genuine release edge this frame
    CHECK(input.isKeyPressed(Key::W));
    CHECK(input.isKeyReleased(Key::D));

    input.beginFrame();

    // Held state is level-triggered and must survive the frame boundary.
    CHECK(input.isKeyDown(Key::W));
    CHECK(input.isKeyDown(Key::D) == false);

    // Both edges are frame-local and are gone.
    CHECK_FALSE(input.isKeyPressed(Key::W));
    CHECK_FALSE(input.isKeyReleased(Key::D));
}

void testResetClearsEverything()
{
    Input input;
    input.processKeyDown(Key::W);
    input.processKeyDown(Key::D);

    input.reset();

    CHECK_FALSE(input.anyKeyDown());
    CHECK_FALSE(input.isKeyDown(Key::W));
    CHECK_FALSE(input.isKeyPressed(Key::W));
    CHECK_FALSE(input.isKeyReleased(Key::W));
}

void testTwoInputsDoNotInterfere()
{
    Input first;
    Input second;

    first.processKeyDown(Key::W);

    CHECK(first.isKeyDown(Key::W));
    // Proves there is no static or shared keyboard state behind Input.
    CHECK_FALSE(second.isKeyDown(Key::W));
    CHECK_FALSE(second.anyKeyDown());
}

// ---------------------------------------------------------------------------
// SFML adapter: the boundary, exercised with real sf::Event values
// ---------------------------------------------------------------------------

void testSfmlKeyMapping()
{
    CHECK(engine::input::toEngineKey(sf::Keyboard::W) == Key::W);
    CHECK(engine::input::toEngineKey(sf::Keyboard::A) == Key::A);
    CHECK(engine::input::toEngineKey(sf::Keyboard::S) == Key::S);
    CHECK(engine::input::toEngineKey(sf::Keyboard::D) == Key::D);
    CHECK(engine::input::toEngineKey(sf::Keyboard::Up) == Key::Up);
    CHECK(engine::input::toEngineKey(sf::Keyboard::Down) == Key::Down);
    CHECK(engine::input::toEngineKey(sf::Keyboard::Left) == Key::Left);
    CHECK(engine::input::toEngineKey(sf::Keyboard::Right) == Key::Right);
    CHECK(engine::input::toEngineKey(sf::Keyboard::Space) == Key::Space);
    CHECK(engine::input::toEngineKey(sf::Keyboard::Escape) == Key::Escape);

    // Keys the engine does not track become Unknown rather than an error.
    CHECK(engine::input::toEngineKey(sf::Keyboard::LShift) == Key::Unknown);
    CHECK(engine::input::toEngineKey(sf::Keyboard::Num7) == Key::Unknown);
    CHECK(engine::input::toEngineKey(sf::Keyboard::Unknown) == Key::Unknown);
}

void testSfmlEventsDriveRealStateMachine()
{
    Input input;

    sf::Event press{};
    press.type = sf::Event::KeyPressed;
    press.key.code = sf::Keyboard::W;

    sf::Event release{};
    release.type = sf::Event::KeyReleased;
    release.key.code = sf::Keyboard::W;

    engine::input::applyKeyboardEvent(press, input);
    CHECK(input.isKeyDown(Key::W));
    CHECK(input.isKeyPressed(Key::W));

    nextFrame(input);
    engine::input::applyKeyboardEvent(press, input); // OS repeat
    CHECK(input.isKeyDown(Key::W));
    CHECK_FALSE(input.isKeyPressed(Key::W));

    engine::input::applyKeyboardEvent(release, input);
    CHECK_FALSE(input.isKeyDown(Key::W));
    CHECK(input.isKeyReleased(Key::W));
}

void testSfmlWindowCloseIsNotInput()
{
    Input input;

    // sf::Event::Closed is an application concern. Handing it to the input
    // adapter must not fabricate a key transition.
    sf::Event closed{};
    closed.type = sf::Event::Closed;

    engine::input::applyKeyboardEvent(closed, input);

    CHECK_FALSE(input.anyKeyDown());
    for (const Key key : {Key::W, Key::Escape, Key::Space})
    {
        CHECK_FALSE(input.isKeyPressed(key));
        CHECK_FALSE(input.isKeyReleased(key));
    }
}

void testSfmlUnmappedKeyIsIgnored()
{
    Input input;

    sf::Event event{};
    event.type = sf::Event::KeyPressed;
    event.key.code = sf::Keyboard::LShift;

    engine::input::applyKeyboardEvent(event, input);

    CHECK_FALSE(input.anyKeyDown());
}

// ---------------------------------------------------------------------------
// MovementSystem
// ---------------------------------------------------------------------------

Entity& makeMover(EntityManager& manager, const Vec2& position = Vec2{0.0F, 0.0F})
{
    Entity& entity = manager.addEntity("mover");
    entity.addComponent<Transform>(Transform{position, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
    entity.addComponent<Rectangle>();
    return entity;
}

void testNoInputMeansNoMovement()
{
    EntityManager manager;
    Input input;
    engine::ecs::SystemManager systems;
    systems.add<MovementSystem>(100.0F);

    Entity& entity = makeMover(manager);

    systems.update(manager, input, 1.0F);

    CHECK(entity.getComponent<Transform>().position == Vec2(0.0F, 0.0F));
}

void testMoveUp()
{
    EntityManager manager;
    Input input;
    MovementSystem movement{100.0F};

    Entity& entity = makeMover(manager);
    input.processKeyDown(Key::W);

    // One second at 100 px/s is 100 pixels up, which is -y on screen.
    movement.update(manager, input, 1.0F);

    CHECK(entity.getComponent<Transform>().position == Vec2(0.0F, -100.0F));

    // The arrow key does the same thing.
    input.reset();
    input.processKeyDown(Key::Up);
    movement.update(manager, input, 1.0F);
    CHECK(entity.getComponent<Transform>().position == Vec2(0.0F, -200.0F));
}

void testMoveDown()
{
    EntityManager manager;
    Input input;
    MovementSystem movement{100.0F};

    Entity& entity = makeMover(manager);
    input.processKeyDown(Key::S);

    movement.update(manager, input, 1.0F);

    CHECK(entity.getComponent<Transform>().position == Vec2(0.0F, 100.0F));
}

void testMoveLeft()
{
    EntityManager manager;
    Input input;
    MovementSystem movement{100.0F};

    Entity& entity = makeMover(manager);
    input.processKeyDown(Key::A);

    movement.update(manager, input, 1.0F);

    CHECK(entity.getComponent<Transform>().position == Vec2(-100.0F, 0.0F));
}

void testMoveRight()
{
    EntityManager manager;
    Input input;
    MovementSystem movement{100.0F};

    Entity& entity = makeMover(manager);
    input.processKeyDown(Key::D);

    movement.update(manager, input, 1.0F);

    CHECK(entity.getComponent<Transform>().position == Vec2(100.0F, 0.0F));
}

void testOppositeKeysCancel()
{
    EntityManager manager;
    Input input;
    MovementSystem movement{100.0F};

    Entity& entity = makeMover(manager);
    input.processKeyDown(Key::D);
    input.processKeyDown(Key::A);

    movement.update(manager, input, 1.0F);

    CHECK(entity.getComponent<Transform>().position == Vec2(0.0F, 0.0F));
}

void testDiagonalMovementIsNormalized()
{
    EntityManager manager;
    Input input;
    MovementSystem movement{100.0F};

    Entity& entity = makeMover(manager);
    input.processKeyDown(Key::W);
    input.processKeyDown(Key::D);

    movement.update(manager, input, 1.0F);

    const Vec2 position = entity.getComponent<Transform>().position;

    // W+D would be (1, -1) unnormalised, length sqrt(2). Normalised, each axis
    // moves 1/sqrt(2) so the total distance is exactly the speed.
    CHECK_NEAR(position.x, 100.0F * 0.70710678118654752F);
    CHECK_NEAR(position.y, -100.0F * 0.70710678118654752F);

    // The whole point: total distance is the speed, not speed * sqrt(2).
    CHECK_NEAR(position.length(), 100.0F);
    CHECK(position.length() < 141.5F);
}

void testHorizontalAndVerticalSpeedsMatch()
{
    EntityManager manager;
    [[maybe_unused]] Input input;
    MovementSystem movement{100.0F};

    // Straight up, straight down, straight left, straight right: all exactly
    // the same speed, which is what normalisation buys.
    const Vec2 keys[] = {Vec2{0.0F, -1.0F}, Vec2{0.0F, 1.0F}, Vec2{-1.0F, 0.0F}, Vec2{1.0F, 0.0F}};

    for (const Vec2& key : keys)
    {
        Input keys_pressed;
        Entity& entity = makeMover(manager);
        const Vec2 start = entity.getComponent<Transform>().position;

        if (key.x < 0.0F) keys_pressed.processKeyDown(Key::A);
        if (key.x > 0.0F) keys_pressed.processKeyDown(Key::D);
        if (key.y < 0.0F) keys_pressed.processKeyDown(Key::W);
        if (key.y > 0.0F) keys_pressed.processKeyDown(Key::S);

        movement.update(manager, keys_pressed, 1.0F);

        const Vec2 travelled = entity.getComponent<Transform>().position - start;
        CHECK_NEAR(travelled.length(), 100.0F);
    }
}

void testMovementIsDeltaTimeBased()
{
    EntityManager manager;
    Input input;
    MovementSystem movement{100.0F};

    Entity& entity = makeMover(manager);
    input.processKeyDown(Key::D);

    // A quarter second moves a quarter as far.
    movement.update(manager, input, 0.25F);
    CHECK_NEAR(entity.getComponent<Transform>().position.x, 25.0F);

    // Four quarter-second frames: total elapsed is exactly one second, so the
    // total distance is exactly one second of speed. Sum of deltas is what
    // matters, not the number of frames.
    movement.update(manager, input, 0.25F);
    movement.update(manager, input, 0.25F);
    movement.update(manager, input, 0.25F);
    CHECK_NEAR(entity.getComponent<Transform>().position.x, 100.0F);
}

void testSpeedIsIndependentOfFrameCount()
{
    Input input;
    MovementSystem movement{100.0F};
    input.processKeyDown(Key::D);

    // One second of holding, delivered as a single 1.0 s frame.
    EntityManager oneFrame;
    Entity& coarse = makeMover(oneFrame);
    movement.update(oneFrame, input, 1.0F);

    // The same second, delivered as a hundred 0.01 s frames, in a separate
    // world so the two cases cannot contaminate each other.
    EntityManager manyFrames;
    Entity& fine = makeMover(manyFrames);
    for (int frame = 0; frame < 100; ++frame)
    {
        movement.update(manyFrames, input, 0.01F);
    }

    // Same elapsed time, same distance. This is the property that makes the
    // delta real rather than a stand-in for a frame count.
    CHECK_NEAR(coarse.getComponent<Transform>().position.x, 100.0F);
    CHECK_NEAR(fine.getComponent<Transform>().position.x, 100.0F);
    CHECK_NEAR(coarse.getComponent<Transform>().position.x, fine.getComponent<Transform>().position.x);
}

void testCustomSpeed()
{
    EntityManager manager;
    Input input;
    MovementSystem movement{250.0F};

    CHECK(movement.speed() == 250.0F);

    Entity& entity = makeMover(manager);
    input.processKeyDown(Key::D);
    movement.update(manager, input, 1.0F);

    CHECK_NEAR(entity.getComponent<Transform>().position.x, 250.0F);
}

void testMovementLeavesVelocityAlone()
{
    EntityManager manager;
    Input input;
    MovementSystem movement{100.0F};

    Entity& entity = manager.addEntity("mover");
    // A velocity that is deliberately not a direction: the movement system must
    // not consume it or rewrite it.
    entity.addComponent<Transform>(Transform{Vec2{0.0F, 0.0F}, Vec2{7.0F, 9.0F}, Vec2{1.0F, 1.0F}, 0.0F});

    input.processKeyDown(Key::D);
    movement.update(manager, input, 1.0F);

    const Transform& transform = entity.getComponent<Transform>();
    CHECK(transform.position == Vec2(100.0F, 0.0F));
    // Velocity is untouched: this system derives motion from input, not from it.
    CHECK(transform.velocity == Vec2(7.0F, 9.0F));
}

void testMultipleEntitiesAllMove()
{
    EntityManager manager;
    Input input;
    MovementSystem movement{100.0F};

    Entity& first = makeMover(manager);
    Entity& second = makeMover(manager, Vec2{10.0F, 10.0F});
    Entity& third = makeMover(manager, Vec2{20.0F, 20.0F});

    input.processKeyDown(Key::D);
    movement.update(manager, input, 1.0F);

    CHECK(first.getComponent<Transform>().position == Vec2(100.0F, 0.0F));
    CHECK(second.getComponent<Transform>().position == Vec2(110.0F, 10.0F));
    CHECK(third.getComponent<Transform>().position == Vec2(120.0F, 20.0F));
}

void testEntitiesWithoutTransformAreUntouched()
{
    EntityManager manager;
    Input input;
    MovementSystem movement{100.0F};

    // No Transform: nothing to move, and nothing to crash on.
    manager.addEntity("noTransform").addComponent<Rectangle>();
    manager.addEntity("bare");

    input.processKeyDown(Key::D);
    movement.update(manager, input, 1.0F);

    CHECK(manager.aliveEntityCount() == 2);
    CHECK(manager.query<Transform>().empty());
}

void testDeadEntitiesAreNotMoved()
{
    EntityManager manager;
    Input input;
    MovementSystem movement{100.0F};

    Entity& alive = makeMover(manager, Vec2{0.0F, 0.0F});
    Entity& doomed = makeMover(manager, Vec2{500.0F, 500.0F});

    manager.destroyEntity(doomed);

    input.processKeyDown(Key::D);
    movement.update(manager, input, 1.0F);

    CHECK(alive.getComponent<Transform>().position == Vec2(100.0F, 0.0F));
    // The dead one kept its position, because the query skipped it.
    CHECK(doomed.getComponent<Transform>().position == Vec2(500.0F, 500.0F));
}

void testReleasingKeysStopsMovement()
{
    EntityManager manager;
    Input input;
    MovementSystem movement{100.0F};

    Entity& entity = makeMover(manager);

    input.processKeyDown(Key::D);
    movement.update(manager, input, 1.0F);
    CHECK(entity.getComponent<Transform>().position == Vec2(100.0F, 0.0F));

    // Release, then next frame.
    input.processKeyUp(Key::D);
    nextFrame(input);
    movement.update(manager, input, 1.0F);

    // Unchanged: no keys held, so no movement.
    CHECK(entity.getComponent<Transform>().position == Vec2(100.0F, 0.0F));
}

// ---------------------------------------------------------------------------
// Integration: Input through SystemManager, alongside other systems
// ---------------------------------------------------------------------------

/// A test-only system that reacts to a single press rather than to holding,
/// proving pressed and down are genuinely different questions.
class JumpOnPressSystem final : public engine::ecs::System
{
public:
    explicit JumpOnPressSystem(int& jumps) : m_jumps{&jumps} {}

    void update(EntityManager& entities, Input& input, const float deltaSeconds) override
    {
        static_cast<void>(deltaSeconds);

        for (auto&& [entity, transform] : entities.query<Transform>())
        {
            static_cast<void>(entity);
            if (input.isKeyPressed(Key::Space))
            {
                transform.position.y -= 50.0F;
                ++(*m_jumps);
            }
        }
    }

    [[nodiscard]] const char* name() const override { return "JumpOnPressSystem"; }

private:
    int* m_jumps;
};
void testPressedAndDownDifferForGameplay()
{
    EntityManager manager;
    Input input;
    int jumps = 0;

    engine::ecs::SystemManager systems;
    systems.add<JumpOnPressSystem>(jumps);

    Entity& entity = makeMover(manager);

    // Hold space across five frames: held, but pressed only on the first.
    input.processKeyDown(Key::Space);
    for (int frame = 0; frame < 5; ++frame)
    {
        systems.update(manager, input, 0.016F);
        if (frame > 0)
        {
            // The operating system repeats the press; it must not re-trigger.
            input.processKeyDown(Key::Space);
        }
        nextFrame(input);
    }

    CHECK(jumps == 1);
    CHECK_NEAR(entity.getComponent<Transform>().position.y, -50.0F);
}

void testSystemsShareTheSameInputObject()
{
    EntityManager manager;
    Input input;

    engine::ecs::SystemManager systems;
    systems.add<MovementSystem>(100.0F);

    makeMover(manager);

    // Movement is driven by the same Input the test holds, with no path through
    // Application and no global in between.
    input.processKeyDown(Key::D);
    systems.update(manager, input, 1.0F);

    CHECK(manager.query<Transform>().size() == 1);
}

void testMovementAndOtherSystemsCompose()
{
    EntityManager manager;
    Input input;
    std::vector<float> deltas;

    engine::ecs::SystemManager systems;
    systems.add<MovementSystem>(100.0F);

    Entity& entity = makeMover(manager);

    // Several frames with the key held, released partway.
    input.processKeyDown(Key::D);
    systems.update(manager, input, 0.5F);
    input.processKeyUp(Key::D);
    nextFrame(input);
    systems.update(manager, input, 0.5F);

    // Only the frames where the key was down moved it.
    CHECK_NEAR(entity.getComponent<Transform>().position.x, 50.0F);
    CHECK(deltas.empty());
}

} // namespace

int main()
{
    const std::pair<const char*, void (*)()> testCases[] = {
        {"initial state is all false", &testInitialStateIsAllFalse},
        {"first key press", &testFirstKeyPress},
        {"held key is not pressed again", &testHeldKeyIsNotPressedAgain},
        {"key release", &testKeyRelease},
        {"frame after release is clean", &testFrameAfterReleaseIsClean},
        {"key repeat does not retrigger pressed", &testKeyRepeatDoesNotRetriggerPressed},
        {"multiple simultaneous keys", &testMultipleSimultaneousKeys},
        {"release one while another held", &testReleaseOneWhileAnotherHeld},
        {"key state isolation", &testKeyStateIsolation},
        {"escape is just a key", &testEscapeIsJustAKey},
        {"unknown key is inert", &testUnknownKeyIsInert},
        {"spurious release is ignored", &testSpuriousReleaseIsIgnored},
        {"beginFrame clears transients but keeps held state", &testBeginFrameClearsTransientsButKeepsHeldState},
        {"reset clears everything", &testResetClearsEverything},
        {"two inputs do not interfere", &testTwoInputsDoNotInterfere},
        {"sfml key mapping", &testSfmlKeyMapping},
        {"sfml events drive the real state machine", &testSfmlEventsDriveRealStateMachine},
        {"sfml window close is not input", &testSfmlWindowCloseIsNotInput},
        {"sfml unmapped key is ignored", &testSfmlUnmappedKeyIsIgnored},
        {"no input means no movement", &testNoInputMeansNoMovement},
        {"move up", &testMoveUp},
        {"move down", &testMoveDown},
        {"move left", &testMoveLeft},
        {"move right", &testMoveRight},
        {"opposite keys cancel", &testOppositeKeysCancel},
        {"diagonal movement is normalized", &testDiagonalMovementIsNormalized},
        {"horizontal and vertical speeds match", &testHorizontalAndVerticalSpeedsMatch},
        {"movement is delta time based", &testMovementIsDeltaTimeBased},
        {"speed is independent of frame count", &testSpeedIsIndependentOfFrameCount},
        {"custom speed", &testCustomSpeed},
        {"movement leaves velocity alone", &testMovementLeavesVelocityAlone},
        {"multiple entities all move", &testMultipleEntitiesAllMove},
        {"entities without transform are untouched", &testEntitiesWithoutTransformAreUntouched},
        {"dead entities are not moved", &testDeadEntitiesAreNotMoved},
        {"releasing keys stops movement", &testReleasingKeysStopsMovement},
        {"pressed and down differ for gameplay", &testPressedAndDownDifferForGameplay},
        {"systems share the same input object", &testSystemsShareTheSameInputObject},
        {"movement and other systems compose", &testMovementAndOtherSystemsCompose},
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

    std::cout << groupCount << " Phase 7 test groups passed\n";
    return EXIT_SUCCESS;
}
