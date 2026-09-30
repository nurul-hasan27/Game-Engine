/// Player behaviour: spawn, movement, facing, the state machine, jumping, variable
/// jump height, gravity, respawn and animation selection.
///
/// ### What is driven, and what is observed
///
/// Every group here runs the **real** pipeline in the production order -
/// [engine::systems::PlayerSystem], then [engine::systems::PhysicsSystem], then
/// [engine::systems::AnimationSystem] - against a player spawned by the **real**
/// [engine::level::LevelLoader] from level data, drawn from the **real** committed
/// asset table. Nothing is a stand-in, because a movement bug that only appears with
/// the real collider size or the real asset names is exactly the kind this suite
/// exists to catch.
///
/// Input is pressed as **keys** on a real [engine::input::Input] and resolved through
/// the real default action map, the way [engine::Application] does it. That is not
/// ceremony: `W` drives both `Jump` and `MoveUp`, so a menu... a player that confirmed
/// on the wrong action is only distinguishable from one that got it right by pressing
/// the key and letting the map decide. A snapshot built by hand would make that
/// untestable.
///
/// ### The world these tests build
///
/// ```text
///   (0,0)  ground tile, 64x64, static          the floor
///   (0,1)  the player, 40x60, dynamic          standing on the floor
/// ```
///
/// The player is placed at grid cell (0, 1) so that its collider's bottom edge lands
/// exactly on the floor tile's top edge. That is a real arrangement rather than a
/// convenient one: the loader derives the position from the grid cell, and a fixture
/// that dropped the player at a hand-picked pixel would not be testing the placement
/// the level format actually produces.
#include "engine/Color.hpp"
#include "engine/Application.hpp"
#include "engine/EngineConfig.hpp"
#include "engine/assets/SfmlAssetManager.hpp"
#include "engine/components/Animation.hpp"
#include "engine/components/Body.hpp"
#include "engine/components/Collider.hpp"
#include "engine/components/Player.hpp"
#include "engine/components/PlayerConfig.hpp"
#include "engine/components/Transform.hpp"
#include "engine/ecs/EntityManager.hpp"
#include "engine/ecs/SystemManager.hpp"
#include "engine/graphics/Camera.hpp"
#include "engine/graphics/SfmlRenderer.hpp"
#include "engine/input/Action.hpp"
#include "engine/input/ActionMap.hpp"
#include "engine/input/ActionState.hpp"
#include "engine/input/Input.hpp"
#include "engine/level/Level.hpp"
#include "engine/level/LevelFile.hpp"
#include "engine/level/LevelGrid.hpp"
#include "engine/level/LevelLoader.hpp"
#include "engine/math/Vec2.hpp"
#include "engine/physics/Aabb.hpp"
#include "engine/scene/PlayScene.hpp"
#include "engine/scene/Scene.hpp"
#include "engine/systems/AnimationSystem.hpp"
#include "engine/systems/PhysicsSystem.hpp"
#include "engine/systems/PlayerStateSystem.hpp"
#include "engine/systems/PlayerSystem.hpp"

#include <SFML/Graphics/RenderWindow.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

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

#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)
#define CHECK_FALSE(...) check(!(__VA_ARGS__), "!" #__VA_ARGS__, __FILE__, __LINE__)
#define CHECK_NEAR(actual, expected, tolerance) \
    checkNear((actual), (expected), (tolerance), #actual " ~= " #expected, __FILE__, __LINE__)

using engine::Vec2;
using engine::assets::AssetManager;
using engine::components::Animation;
using engine::components::Body;
using engine::components::Collider;
using engine::components::Player;
using engine::components::PlayerConfig;
using engine::components::PlayerState;
using engine::components::Transform;
using engine::ecs::Entity;
using engine::ecs::EntityManager;
using engine::ecs::SystemManager;
using engine::graphics::Camera;
using engine::input::Action;
using engine::input::ActionState;
using engine::input::Input;
using engine::input::Key;
using engine::level::Level;
using engine::level::LevelGrid;
using engine::level::LevelLoader;
using engine::level::PlayerRecord;
using engine::level::TileRecord;
using engine::systems::AnimationSystem;
using engine::systems::PhysicsSystem;
using engine::systems::PlayerSystem;

// The frame length almost every group steps with. One sixtieth of a second: the
// engine's frame cap is 60 and a test that stepped with a different number would be
// testing a different game.
constexpr float kFrame = 1.0F / 60.0F;

// How many frames a test may run while waiting for something to happen. A generous
// bound on a bound: ten seconds of simulated time, which is far longer than any of
// these behaviours takes and short enough that a failure is a hang rather than a
// wait.
constexpr int kMaxFrames = 600;

// ### The world's coordinates, because they are not what a first reading suggests
//
// Y grows **downwards**, and grid Y grows **upwards** from the bottom of the world, so
// the loader flips one into the other. A 16-cell world is 1024 pixels tall, and:
//
//   gridY 0   the bottom cell   spans world y 960..1024   (a 64x64 tile centres at 992)
//   gridY 1   the cell above   spans world y 896..960
//
// A player with a 60-pixel collider placed at gridY 1 has its bottom edge on the
// cell's bottom edge - world y 960, which is the top of the floor tile - so it is
// standing on the floor with its centre at 960 - 30 = **930**.
//
// Every height in this file is written as arithmetic on those numbers rather than as a
// bare literal, so a reader can check it and a change to the world height moves the
// expectations with it. The first version of this suite wrote 94, from remembering that
// y grows downwards and forgetting that the *world* starts at the top of a 1024-pixel
// space; with the wrong base, every height-based assertion was wrong by exactly 836.
constexpr float kCell = engine::level::LevelGrid::kCellSize;
constexpr float kWorldHeight = 16.0F * kCell;
/// The world y of the *top* of the floor run, which is where a player's feet rest.
constexpr float kFloorTop = kWorldHeight - kCell;
/// Where a 60-tall player stands when its feet are on the floor.
constexpr float kPlayerRestY = kFloorTop - (60.0F * 0.5F);
/// Where the player spawns at gridY 1 - the same as resting, because that cell's
/// bottom edge *is* the floor's top edge.
constexpr float kPlayerSpawnY = kWorldHeight - kCell - (60.0F * 0.5F);

// The committed player's numbers, restated here so a test can say "the level says
// 200" without reading the value it is about to compare against. These are the
// numbers in `assets/levels/level1.txt`, and the committed-level group checks that
// the file still agrees with them - so this is a statement about the *game*, not a
// copy of a value used to make a comparison pass.
constexpr float kLevelLeftRightSpeed = 200.0F;
constexpr float kLevelJumpSpeed = 400.0F;
constexpr float kLevelMaxSpeed = 250.0F;
constexpr float kLevelGravity = 900.0F;

/// Drives the real action map with real key presses.
///
/// `pressedNow` is a fresh press - active *and* pressed this frame. `held` is the
/// frames after it, where the key is still down but no longer a new press, which is
/// the entire distinction between an edge-triggered action and a level-triggered one.
class ActionDriver
{
public:
    [[nodiscard]] ActionState pressedNow(const Key key)
    {
        m_input.processKeyDown(key);
        return snapshot();
    }

    [[nodiscard]] ActionState held()
    {
        m_input.beginFrame();
        return snapshot();
    }

    [[nodiscard]] ActionState released()
    {
        m_input.beginFrame();
        m_input.processKeyUp(Key::W);
        return snapshot();
    }

    [[nodiscard]] ActionState idle()
    {
        m_input.beginFrame();
        return snapshot();
    }

    /// One frame in which `key` has come **up**.
    ///
    /// Distinct from [idle], and the distinction is the whole point of a held key. A
    /// frame with no new press still has the key physically down, so `isActive` is
    /// still true and a level-triggered system would still see it. Releasing needs the
    /// key to actually go up.
    ///
    /// The first version of `releasing movement stops the player` tried to fake this by
    /// pressing a *different* key, which moved the player the other way and asserted it
    /// had stopped.
    [[nodiscard]] ActionState release(const Key key)
    {
        m_input.beginFrame();
        m_input.processKeyUp(key);
        return snapshot();
    }

private:
    [[nodiscard]] ActionState snapshot() const
    {
        ActionState actions;
        actions.update(engine::input::defaultActionMap(), m_input);
        return actions;
    }

    Input m_input;
};

// ---------------------------------------------------------------------------
// A level, built in memory, and a world spawned from it by the real loader.
// ---------------------------------------------------------------------------

/// A level record whose numbers are the committed player's, so a group that wants
/// "the level says 200" is testing the engine against a level rather than against
/// itself.
[[nodiscard]] PlayerRecord playerRecordAt(const float gridX, const float gridY,
                                          const Vec2 boundingBox = Vec2{40.0F, 60.0F})
{
    PlayerRecord record;
    record.gridX = gridX;
    record.gridY = gridY;
    record.boundingBoxSize = boundingBox;
    record.leftRightSpeed = kLevelLeftRightSpeed;
    record.jumpSpeed = kLevelJumpSpeed;
    record.maxSpeed = kLevelMaxSpeed;
    record.gravity = kLevelGravity;
    record.bulletAnimationName = "megaman_megaBuster_shot";
    return record;
}

[[nodiscard]] TileRecord tileAt(const float gridX, const float gridY, const std::string_view animationName)
{
    TileRecord record;
    record.animationName = std::string{animationName};
    record.gridX = gridX;
    record.gridY = gridY;
    return record;
}

/// A floor of `tiles` ground tiles along the bottom row, with `record` as the player.
///
/// ### Why the player is a parameter and not set afterwards
///
/// [engine::level::Level::setPlayer] **ignores a second call**. That is right for a
/// parsed level - the format allows exactly one `Player` line, and a second one is an
/// error the parser reports - and it is a trap for a constructed one, because a test
/// that built a level and then "changed" the player would silently keep the first
/// record. Three groups did exactly that and measured 200 px/s while asserting 60.
///
/// So every level here is built with the player it wants, in one call. That is also
/// closer to what a real level does: a level's player is part of the level, not an
/// override applied afterwards.
[[nodiscard]] Level levelWith(const PlayerRecord& record, const int tiles = 6)
{
    Level level;
    for (int index = 0; index < tiles; ++index)
    {
        level.addTile(tileAt(static_cast<float>(index), 0.0F, "mario_ground_tile"));
    }
    level.setPlayer(record);
    return level;
}

/// The world every group here plays in: a floor at cell (0,0) and the player at
/// (0, 1), standing on it.
///
/// The floor is the committed `mario_ground_tile` at its real 64x64 and the player the
/// committed 40x60, so the colliders are the ones the game actually collides.
[[nodiscard]] Level oneTileLevel() { return levelWith(playerRecordAt(0.0F, 1.0F), 1); }

/// A floor several cells wide, for the walking groups.
///
/// A one-cell floor would let the player walk off the end in half a second, which
/// would make a movement test a fall test by accident. Six cells is 384 pixels, which
/// at the level's 200 px/s is nearly two seconds of walking.
[[nodiscard]] Level wideFloorLevel(const int tiles = 6)
{
    return levelWith(playerRecordAt(0.0F, 1.0F), tiles);
}

// ---------------------------------------------------------------------------
// The fixture: a world, the production system order, and the player's components.
// ---------------------------------------------------------------------------

class PlayerFixture
{
public:
    /// @param level The level to spawn, built by the helpers above.
    /// @param fallLimitY The world coordinate below which the player respawns.
    ///
    ///        Defaults to the world's bottom edge, which is what
    ///        [engine::scene::PlayScene] passes, and the groups below override it
    ///        only to move the boundary rather than to shrink the world. The group
    ///        that checks the *value* this is given in production is
    ///        `the fall limit is the world's bottom edge`, which builds a real
    ///        [engine::scene::PlayScene] and reads the number off it.
    PlayerFixture(const Level& level, const float fallLimitY = kWorldHeight)
        : m_assets{std::filesystem::path{engine::config::kAssetsConfig}}
    {
        // The production order, minus the camera and the zoom keys, which need a
        // camera this fixture has no use for. All four of the rest are kept, and the
        // two that Phase 17 changed the order of are the interesting ones:
        // `PlayerSystem` still comes first so its velocity is integrated this frame,
        // and `PlayerStateSystem` comes straight after physics so `grounded` is read
        // from the frame's own collisions rather than the previous frame's.
        //
        // `add` hands back a reference, and the reference is safe to keep: the system
        // manager stores systems through `unique_ptr` precisely so that registering
        // another does not move this one.
        m_systems.add<PlayerSystem>(fallLimitY);
        const PhysicsSystem& physics = m_systems.add<PhysicsSystem>();
        m_systems.add<engine::systems::PlayerStateSystem>(physics.collisions());
        m_systems.add<AnimationSystem>(m_assets);

        const LevelLoader loader{m_assets};
        const LevelGrid grid = LevelGrid::withCellsTall(16.0F);
        static_cast<void>(loader.spawn(level, m_world, grid));
    }

    /// One frame, in the production order.
    void step(const ActionState& actions, const float deltaSeconds = kFrame)
    {
        m_systems.update(m_world, actions, deltaSeconds);
        // The scene flushes its own world's deferred destruction; so does this, for the
        // same reason and in the same place.
        m_world.update();
    }

    /// Runs up to `frames` frames with the same snapshot, stopping early once `until`
    /// is satisfied. Returns the number of frames actually run.
    ///
    /// Used by the groups that wait for a landing. A loop with a bound rather than a
    /// bare `while` so a behaviour that never arrives is a **failed assertion** and not
    /// a hung suite - which is the difference between a test and a trap.
    template <typename Predicate>
    int stepUntil(const ActionState& actions, const Predicate& until, const int frames = kMaxFrames)
    {
        // Steps **then** checks, and the order is load bearing.
        //
        // Checking first would return on the frame the condition became true - the
        // frame the system *wrote* it - and never run the frame that acts on it. For
        // a landing that means returning while the player is still a frame above the
        // floor and `jumping` is still set, so the group would assert against a state
        // one frame stale. `landing returns to a grounded state` failed exactly that
        // way: grounded, 6.7 pixels in the air, and still jumping.
        for (int frame = 0; frame < frames; ++frame)
        {
            step(actions);
            if (until())
            {
                return frame;
            }
        }
        return frames;
    }

    /// The player entity.
    ///
    /// Found through a **query** rather than through
    /// [engine::ecs::EntityManager::getEntities], and that is not a style choice.
    /// `getEntities` returns an [engine::ecs::EntityView], which is a read-only view
    /// by design - its iterator yields `const Entity&` and always has. So a mutable
    /// `Entity&` cannot come from it, and the query is the engine's own idiom for
    /// reaching a specific entity's components, the way every system does it.
    ///
    /// The query is on the player's own component set rather than on the tag, so the
    /// player found is one this system would actually drive. An entity with the tag but
    /// missing a component would not be found, and the groups that check components
    /// would then see a missing player rather than a silently unmodifiable one.
    [[nodiscard]] Entity& player()
    {
        for (auto&& [entity, transform, config, state, animation] :
             m_world.query<Transform, PlayerConfig, Player, Animation>())
        {
            static_cast<void>(transform);
            static_cast<void>(config);
            static_cast<void>(state);
            static_cast<void>(animation);
            return entity;
        }
        throw std::logic_error{"the world has no player"};
    }

    [[nodiscard]] const Entity& player() const
    {
        for (const auto& [entity, transform, config, state, animation] :
             m_world.query<Transform, PlayerConfig, Player, Animation>())
        {
            static_cast<void>(transform);
            static_cast<void>(config);
            static_cast<void>(state);
            static_cast<void>(animation);
            return entity;
        }
        throw std::logic_error{"the world has no player"};
    }

    [[nodiscard]] Transform& transform() { return player().getComponent<Transform>(); }
    [[nodiscard]] const Transform& transform() const { return player().getComponent<Transform>(); }
    [[nodiscard]] Player& state() { return player().getComponent<Player>(); }
    [[nodiscard]] const Player& state() const { return player().getComponent<Player>(); }
    [[nodiscard]] PlayerConfig& config() { return player().getComponent<PlayerConfig>(); }
    [[nodiscard]] Collider& collider() { return player().getComponent<Collider>(); }
    [[nodiscard]] Animation& animation() { return player().getComponent<Animation>(); }
    [[nodiscard]] EntityManager& world() noexcept { return m_world; }
    [[nodiscard]] const EntityManager& world() const noexcept { return m_world; }
    [[nodiscard]] SystemManager& systems() noexcept { return m_systems; }
    [[nodiscard]] const AssetManager& assets() const noexcept { return m_assets; }

    [[nodiscard]] float playerX() const { return transform().position.x; }
    [[nodiscard]] float playerY() const { return transform().position.y; }

    /// The player's box, in world space.
    [[nodiscard]] engine::physics::Aabb box() const
    {
        return engine::physics::Aabb{transform().position, player().getComponent<Collider>().size};
    }

private:
    /// The asset manager is declared before the systems because
    /// [engine::systems::AnimationSystem] binds a reference to it, and because the
    /// systems are populated in the constructor body. This is the same member ordering
    /// discipline `Application` documents for its own renderer.
    engine::assets::SfmlAssetManager m_assets;
    EntityManager m_world;
    SystemManager m_systems;
};

/// The player's state as a word, for failure messages.
[[nodiscard]] std::string_view nameOf(const PlayerState state)
{
    switch (state)
    {
        case PlayerState::Stand:
            return "Stand";
        case PlayerState::Run:
            return "Run";
        case PlayerState::Air:
            return "Air";
    }

    return "?";
}

void expectState(const PlayerFixture& fixture, const PlayerState expected, const char* const file, const int line)
{
    if (fixture.state().state != expected)
    {
        ++g_failureCount;
        std::cerr << "    " << file << ':' << line << ": expected state " << nameOf(expected) << ", was "
                  << nameOf(fixture.state().state) << '\n';
    }
}

#define EXPECT_STATE(fixture, expected) expectState((fixture), (expected), __FILE__, __LINE__)

/// The vertical velocity a jump leaves behind, after the step that integrated it.
///
/// ### Why this is arithmetic and not `-jumpSpeed`
///
/// Gravity is applied **every** frame now, including the frame the jump launches,
/// because withholding it while grounded is mutually exclusive with detecting support
/// from a real collision - see the note in [engine::systems::PlayerSystem]. So the
/// launch velocity is the level's jump speed less exactly one frame of gravity, and
/// the honest assertion is that difference.
///
/// Writing it as arithmetic rather than as `-400.0F` is the point: it says "the
/// level's jump speed, minus this frame's gravity", so a jump that ignored
/// [engine::components::PlayerConfig] and used a literal 400 would still pass, but a
/// jump that ignored *gravity* on the launch frame would not. The Phase 16 version of
/// this assertion pinned the withheld-gravity artefact, and passing with the gravity
/// removed would have been the bug it could not see.
[[nodiscard]] float launchVelocity(const float jumpSpeed)
{
    return -jumpSpeed + (kLevelGravity * kFrame);
}

/// Settles the player onto the floor.
///
/// A freshly spawned player is one frame of falling away from rest, because the
/// loader honestly reports it as airborne. Running a few idle frames gets it onto the
/// floor and to zero velocity, so a movement group starts from a known state instead
/// of from whatever the spawn arithmetic left behind.
void settle(PlayerFixture& fixture, const int frames = 12)
{
    for (int frame = 0; frame < frames; ++frame)
    {
        fixture.step(ActionState{});
    }
}

// ---------------------------------------------------------------------------
// A. Spawn
// ---------------------------------------------------------------------------

void testThePlayerIsSpawnedFromLevelData()
{
    PlayerFixture fixture{oneTileLevel()};

    CHECK(fixture.world().getEntities(engine::level::kPlayerTag).begin() !=
          fixture.world().getEntities(engine::level::kPlayerTag).end());

    // The collider is the level's box, not a constant. A system that assumed 40x60
    // would pass every other test in this file and be wrong for every other level.
    CHECK_NEAR(fixture.collider().size.x, 40.0F, 0.0001F);
    CHECK_NEAR(fixture.collider().size.y, 60.0F, 0.0001F);

    // The level's numbers, copied through rather than interpreted.
    CHECK_NEAR(fixture.config().leftRightSpeed, kLevelLeftRightSpeed, 0.0001F);
    CHECK_NEAR(fixture.config().jumpSpeed, kLevelJumpSpeed, 0.0001F);
    CHECK_NEAR(fixture.config().maxSpeed, kLevelMaxSpeed, 0.0001F);
    CHECK_NEAR(fixture.config().gravity, kLevelGravity, 0.0001F);
    CHECK(fixture.config().bulletAnimationName == "megaman_megaBuster_shot");

    // The body is dynamic, which is what makes physics move it at all.
    CHECK(fixture.player().getComponent<Body>().type == engine::physics::BodyType::Dynamic);
}

void testTheSpawnPositionComesFromTheGridCell()
{
    // The player at cell (0, 1), with a 40x60 collider.
    //
    // `LevelGrid::centreOf` puts the collider's **bottom-left** on the cell's
    // bottom-left. In world pixels the cell's bottom edge for (0, 1) is
    // `worldHeight - 1 cell` = 960, and the centre is 30 above that, because the
    // collider is 60 tall and the centre of a box whose bottom is 960 is 930.
    //
    // The arithmetic is written out rather than delegated to the same call the loader
    // makes, because a group that computed the expected value by calling `centreOf`
    // would pass if `centreOf` were wrong.
    PlayerFixture fixture{oneTileLevel()};

    CHECK_NEAR(fixture.playerX(), 0.0F + (40.0F * 0.5F), 0.0001F);
    CHECK_NEAR(fixture.playerY(), kWorldHeight - kCell - (60.0F * 0.5F), 0.0001F);
    CHECK_NEAR(fixture.playerY(), kPlayerSpawnY, 0.0001F);

    // And the respawn anchor is that same point, copied rather than recomputed.
    CHECK_NEAR(fixture.state().spawnPosition.x, fixture.playerX(), 0.0001F);
    CHECK_NEAR(fixture.state().spawnPosition.y, fixture.playerY(), 0.0001F);

    // A different cell gives a different position, which is what "from the level" means.
    // Built with the player it wants, because `setPlayer` ignores a second call.
    PlayerFixture elsewhere{levelWith(playerRecordAt(3.0F, 2.0F), 1)};
    CHECK_NEAR(elsewhere.playerX(), (3.0F * kCell) + 20.0F, 0.0001F);
    CHECK_NEAR(elsewhere.playerY(), kWorldHeight - (2.0F * kCell) - 30.0F, 0.0001F);
}

void testAFreshlySpawnedPlayerIsNotGrounded()
{
    // The loader places the player in the air and says so.
    //
    // The committed level spawns the player at cell (3, 4) with the ground at cell 0, so
    // the spawn is four cells up and the player is genuinely airborne. Claiming
    // `grounded` at that moment would let it jump on its very first frame from mid-air,
    // and would stop gravity acting on the frame it is spawned - which is the frame
    // `settle` exists to absorb, and the reason it is invisible everywhere else.
    //
    // This is the *state as spawned*, so it is asserted on the very first frame, with no
    // settling at all. It is also the only place in the suite that a load-time value
    // like this can be observed, because after one frame the world has already made it
    // true or false on its own.
    Level level = levelWith(playerRecordAt(0.0F, 6.0F), 1);
    PlayerFixture fixture{level, 100000.0F};

    // Spawned in the air, six cells up, with the floor one cell below the origin.
    CHECK(fixture.state().grounded == false);
    CHECK(fixture.playerY() < kFloorTop - (2.0F * kCell));

    // The first frame therefore applies gravity: the player starts falling.
    const float before = fixture.playerY();
    fixture.step(ActionState{});

    CHECK(fixture.playerY() > before);
    CHECK(fixture.transform().velocity.y > 0.0F);
    CHECK_FALSE(fixture.state().grounded);

    // And it cannot jump from there, which is the consequence that matters.
    ActionDriver jump;
    fixture.step(jump.pressedNow(Key::W));
    CHECK(fixture.transform().velocity.y > 0.0F);
    CHECK_FALSE(fixture.state().jumping);
}

void testTheSpriteAndTheColliderShareTheTransformPosition()
{
    // The course: "The player's sprite and bounding box are centered on the player's
    // position." Two consumers make two independent assumptions about that - the
    // renderer centres the frame it draws on the transform, and the physics builds an
    // Aabb centred on it - so the invariant is checked from both sides.
    PlayerFixture fixture{oneTileLevel()};
    const engine::assets::Animation& stand = fixture.assets().animation("megaman_megaStand_stand");

    const engine::physics::Aabb box = fixture.box();
    CHECK_NEAR(box.center().x, fixture.playerX(), 0.0001F);
    CHECK_NEAR(box.center().y, fixture.playerY(), 0.0001F);

    // The sprite frame is larger than the collider on both axes - 190x208 against
    // 40x60 - and the honest relationship is a shared centre, not one sized to the
    // other. If they were the same size this would prove nothing, which is why the
    // committed artwork is used rather than a rectangle.
    CHECK(static_cast<int>(fixture.collider().size.x) < stand.frameWidth());
    CHECK(static_cast<int>(fixture.collider().size.y) < stand.frameHeight());

    // And the player is drawn from the same transform, so there is no second position
    // anywhere for the sprite to disagree with.
    CHECK(fixture.animation().assetName == "megaman_megaStand_stand");
}

void testTheCommittedLevelSpawnsAPlayerWithThoseNumbers()
{
    // The real file, through the real parser and the real loader, so none of the above
    // is only true of a level built in memory.
    const Level level = engine::level::loadLevelFile(std::filesystem::path{engine::config::kLevelFile});
    CHECK(level.hasPlayer());

    // The file's own numbers, stated here so a change to `level1.txt` is a test
    // failure rather than a silent change of the game's feel.
    CHECK_NEAR(level.player().leftRightSpeed, kLevelLeftRightSpeed, 0.0001F);
    CHECK_NEAR(level.player().jumpSpeed, kLevelJumpSpeed, 0.0001F);
    CHECK_NEAR(level.player().maxSpeed, kLevelMaxSpeed, 0.0001F);
    CHECK_NEAR(level.player().gravity, kLevelGravity, 0.0001F);
    CHECK_NEAR(level.player().boundingBoxSize.x, 40.0F, 0.0001F);
    CHECK_NEAR(level.player().boundingBoxSize.y, 60.0F, 0.0001F);

    engine::assets::SfmlAssetManager assets{std::filesystem::path{engine::config::kAssetsConfig}};
    EntityManager world;
    const LevelLoader loader{assets};
    const LevelGrid grid = LevelGrid::withCellsTall(16.0F);
    static_cast<void>(loader.spawn(level, world, grid));

    // Exactly one player, and it is a player: it carries every component the behaviour
    // system queries for. A missing one would make [engine::systems::PlayerSystem]
    // silently skip it - which is a failure with no symptom at all.
    std::size_t players = 0U;
    for (const Entity& entity : world.getEntities(engine::level::kPlayerTag))
    {
        ++players;
        CHECK(entity.hasComponent<Transform>());
        CHECK(entity.hasComponent<Animation>());
        CHECK(entity.hasComponent<Collider>());
        CHECK(entity.hasComponent<Body>());
        CHECK(entity.hasComponent<PlayerConfig>());
        CHECK(entity.hasComponent<Player>());
    }
    CHECK(players == 1U);

    // The committed world has a floor under the player's spawn cell, which is the whole
    // reason the level is playable: the player spawns at cell (3, 4) in the air and
    // falls onto the run of ground tiles along the bottom.
    CHECK(level.tiles().size() > 20U);
}

// ---------------------------------------------------------------------------
// B. Horizontal movement
// ---------------------------------------------------------------------------

void testMovingRightWalksRight()
{
    PlayerFixture fixture{wideFloorLevel()};
    settle(fixture);

    const float start = fixture.playerX();
    ActionDriver driver;
    for (int frame = 0; frame < 30; ++frame)
    {
        fixture.step(frame == 0 ? driver.pressedNow(Key::D) : driver.held());
    }

    // 30 frames at 200 px/s and 1/60 s is 100 pixels.
    CHECK(fixture.playerX() > start + 90.0F);
    CHECK_NEAR(fixture.transform().velocity.x, kLevelLeftRightSpeed, 0.0001F);
}

void testMovingLeftWalksLeft()
{
    PlayerFixture fixture{wideFloorLevel()};
    settle(fixture);

    // Start in the middle of the floor so there is room to walk left.
    ActionDriver setup;
    for (int frame = 0; frame < 30; ++frame)
    {
        fixture.step(frame == 0 ? setup.pressedNow(Key::D) : setup.held());
    }
    const float start = fixture.playerX();

    ActionDriver driver;
    for (int frame = 0; frame < 30; ++frame)
    {
        fixture.step(frame == 0 ? driver.pressedNow(Key::A) : driver.held());
    }

    CHECK(fixture.playerX() < start - 90.0F);
    CHECK_NEAR(fixture.transform().velocity.x, -kLevelLeftRightSpeed, 0.0001F);
}

void testReleasingMovementStopsThePlayer()
{
    PlayerFixture fixture{wideFloorLevel()};
    settle(fixture);

    ActionDriver driver;
    for (int frame = 0; frame < 20; ++frame)
    {
        fixture.step(frame == 0 ? driver.pressedNow(Key::D) : driver.held());
    }
    CHECK(std::fabs(fixture.transform().velocity.x) > 1.0F);

    // Release. The velocity is zero on the frame the key goes up, so the player stops
    // rather than sliding - the engine has no friction and no deceleration, and the
    // course does not ask for either.
    fixture.step(driver.release(Key::D));
    CHECK_NEAR(fixture.transform().velocity.x, 0.0F, 0.0001F);

    const float stopped = fixture.playerX();
    for (int frame = 0; frame < 30; ++frame)
    {
        fixture.step(driver.idle());
    }
    // It stays exactly where it stopped. "Stops" means stops, not "slows down".
    CHECK_NEAR(fixture.playerX(), stopped, 0.0001F);
    CHECK_NEAR(fixture.transform().velocity.x, 0.0F, 0.0001F);
}

void testTheWalkSpeedIsTheLevelsNotAConstant()
{
    // A level with a different number, so the assertion cannot be satisfied by a
    // system that read its speed from its own source. This is the group that would
    // fail if someone hardcoded 200.
    PlayerRecord record = playerRecordAt(0.0F, 1.0F);
    record.leftRightSpeed = 60.0F;
    PlayerFixture fixture{levelWith(record)};
    settle(fixture);

    ActionDriver driver;
    for (int frame = 0; frame < 60; ++frame)
    {
        fixture.step(frame == 0 ? driver.pressedNow(Key::D) : driver.held());
    }

    // 60 px/s for one second, from the player's spawn x of 20. The starting point is
    // part of the distance, and leaving it out is how a "one second at 60 px/s" check
    // ends up asserting 60 against an actual 80.
    CHECK_NEAR(fixture.playerX(), 20.0F + 60.0F, 2.0F);
    CHECK_NEAR(fixture.transform().velocity.x, 60.0F, 0.0001F);
}

void testTheMaximumSpeedClampsInBothDirections()
{
    // A level whose maximum speed is *below* its movement speed, which is the only way
    // the clamp can be observed at all: with the committed level, 200 is already under
    // 250 and the clamp never bites.
    //
    // That is worth stating, because it means the committed level does not exercise
    // this at all - and a clamp that is never exercised is a clamp that does not
    // exist. So the clamp is tested with a level built to make it matter.
    PlayerRecord record = playerRecordAt(0.0F, 1.0F);
    record.leftRightSpeed = 900.0F;
    record.maxSpeed = 50.0F;
    PlayerFixture fixture{levelWith(record)};
    settle(fixture);

    ActionDriver driver;
    for (int frame = 0; frame < 20; ++frame)
    {
        fixture.step(frame == 0 ? driver.pressedNow(Key::D) : driver.held());
    }
    // Clamped to the maximum, not to the movement speed and not to the wall.
    CHECK_NEAR(fixture.transform().velocity.x, 50.0F, 0.0001F);

    ActionDriver other;
    for (int frame = 0; frame < 20; ++frame)
    {
        fixture.step(frame == 0 ? other.pressedNow(Key::A) : other.held());
    }
    // And the other direction, which is a separate branch and is the one a sign error
    // in the clamp would leave correct.
    CHECK_NEAR(fixture.transform().velocity.x, -50.0F, 0.0001F);

    // The player actually moved, so the clamp did not achieve its result by refusing
    // to move at all.
    CHECK(fixture.playerX() > 10.0F);
}

void testThePlayerCanMoveInTheAir()
{
    // The course: "The player can move left, move right, or shoot at any time during
    // the game. This means the player can move left/right while in the air."
    //
    // Tested by jumping and steering on the way up, which is the only place this can
    // be observed: a player walking off a ledge would be falling *and* the ground
    // probe would only notice a frame later, so the two are not cleanly separated.
    PlayerFixture fixture{wideFloorLevel()};
    settle(fixture);

    // **One** driver for both keys. A second driver starts with nothing held, so
    // pressing `D` through it would also *release* `W` - and the variable-jump rule
    // would correctly cut the ascent, dropping the player before it had really left
    // the ground. Three groups made that mistake, and the symptom was the player
    // arriving at each subsequent assertion already landed.
    //
    // `Input::processKeyDown` sets the pressed edge only if the key was not already
    // down, so pressing `D` through the driver that is holding `W` adds a key rather
    // than replacing one.
    ActionDriver driver;
    fixture.step(driver.pressedNow(Key::W));
    EXPECT_STATE(fixture, PlayerState::Air);

    fixture.step(driver.pressedNow(Key::D));
    for (int frame = 0; frame < 20; ++frame)
    {
        fixture.step(driver.held());
    }

    CHECK_NEAR(fixture.transform().velocity.x, kLevelLeftRightSpeed, 0.0001F);
    EXPECT_STATE(fixture, PlayerState::Air);
    CHECK(fixture.playerY() < kPlayerRestY);
}

// ---------------------------------------------------------------------------
// C. Facing
// ---------------------------------------------------------------------------

void testFacingFollowsTheDirectionPressed()
{
    PlayerFixture fixture{wideFloorLevel()};
    settle(fixture);

    // Starts facing right, which is the spawn state.
    CHECK_NEAR(fixture.transform().scale.x, 1.0F, 0.0001F);

    ActionDriver left;
    for (int frame = 0; frame < 10; ++frame)
    {
        fixture.step(frame == 0 ? left.pressedNow(Key::A) : left.held());
    }
    CHECK_NEAR(fixture.transform().scale.x, -1.0F, 0.0001F);
    CHECK(fixture.transform().scale.x < 0.0F);

    ActionDriver right;
    for (int frame = 0; frame < 10; ++frame)
    {
        fixture.step(frame == 0 ? right.pressedNow(Key::D) : right.held());
    }
    CHECK_NEAR(fixture.transform().scale.x, 1.0F, 0.0001F);
    CHECK(fixture.transform().scale.x > 0.0F);
}

void testFacingSurvivesStoppingJumpingAndLanding()
{
    // The course: facing lasts "until the other direction has been pressed". So the
    // three things that must NOT change it are stopping, jumping and falling - and a
    // system that wrote `scale.x` unconditionally each frame would reset it every time
    // the key was released.
    PlayerFixture fixture{wideFloorLevel()};
    settle(fixture);

    // Face left...
    ActionDriver left;
    for (int frame = 0; frame < 10; ++frame)
    {
        fixture.step(frame == 0 ? left.pressedNow(Key::A) : left.held());
    }
    CHECK_NEAR(fixture.transform().scale.x, -1.0F, 0.0001F);

    // ...then stop.
    for (int frame = 0; frame < 30; ++frame)
    {
        fixture.step(left.idle());
    }
    CHECK_NEAR(fixture.transform().scale.x, -1.0F, 0.0001F);

    // ...then jump while standing still, and rise.
    ActionDriver jump;
    fixture.step(jump.pressedNow(Key::W));
    CHECK_NEAR(fixture.transform().scale.x, -1.0F, 0.0001F);
    for (int frame = 0; frame < 20; ++frame)
    {
        fixture.step(jump.held());
    }
    CHECK_NEAR(fixture.transform().scale.x, -1.0F, 0.0001F);

    // ...and fall all the way back down.
    for (int frame = 0; frame < 200; ++frame)
    {
        fixture.step(jump.idle());
        if (fixture.state().grounded)
        {
            break;
        }
    }
    CHECK(fixture.state().grounded);
    CHECK_NEAR(fixture.transform().scale.x, -1.0F, 0.0001F);
}

void testJumpingDoesNotResetFacing()
{
    // Facing is a *horizontal* concern and jumping is a vertical one. A system that
    // wrote the whole `scale` when it handled a jump would zero the x, which is neither
    // +1 nor -1 and so renders as no sprite at all.
    PlayerFixture fixture{wideFloorLevel()};
    settle(fixture);

    ActionDriver left;
    for (int frame = 0; frame < 10; ++frame)
    {
        fixture.step(frame == 0 ? left.pressedNow(Key::A) : left.held());
    }
    const float facing = fixture.transform().scale.x;
    CHECK_NEAR(facing, -1.0F, 0.0001F);

    ActionDriver jump;
    fixture.step(jump.pressedNow(Key::W));
    for (int frame = 0; frame < 20; ++frame)
    {
        fixture.step(jump.held());
    }

    // Unchanged, and still exactly +/-1: not merely the same sign.
    CHECK_NEAR(fixture.transform().scale.x, facing, 0.0001F);
    CHECK_NEAR(std::fabs(fixture.transform().scale.x), 1.0F, 0.0001F);
    // The vertical scale is untouched too, or the sprite would be squashed.
    CHECK_NEAR(fixture.transform().scale.y, 1.0F, 0.0001F);
}

// ---------------------------------------------------------------------------
// D. The state machine
// ---------------------------------------------------------------------------

void testAStillGroundedPlayerIsStanding()
{
    PlayerFixture fixture{wideFloorLevel()};
    settle(fixture);

    EXPECT_STATE(fixture, PlayerState::Stand);
    CHECK(fixture.state().grounded);

    // And stays standing while nothing happens.
    for (int frame = 0; frame < 30; ++frame)
    {
        fixture.step(ActionState{});
    }
    EXPECT_STATE(fixture, PlayerState::Stand);
    CHECK(fixture.state().grounded);
}

void testAMovingGroundedPlayerIsRunning()
{
    PlayerFixture fixture{wideFloorLevel()};
    settle(fixture);

    ActionDriver driver;
    for (int frame = 0; frame < 10; ++frame)
    {
        fixture.step(frame == 0 ? driver.pressedNow(Key::D) : driver.held());
    }
    EXPECT_STATE(fixture, PlayerState::Run);
    CHECK(fixture.state().grounded);

    // And stops running the moment it stops, on the same frame. `release`, not
    // `idle`: an idle frame leaves the key physically down, so `isActive` is still
    // true and the player is still running - correctly, because it is still walking.
    fixture.step(driver.release(Key::D));
    EXPECT_STATE(fixture, PlayerState::Stand);
}

void testAnAirbornePlayerIsInAirEvenWhileMovingHorizontally()
{
    // The negative case the prompt asks for: "airborne state must not become Run merely
    // because horizontal input exists". A state machine that tested only the horizontal
    // input would call this Run.
    PlayerFixture fixture{wideFloorLevel()};
    settle(fixture);

    // One driver for both keys - see `the player can move in the air` for why a second
    // driver would release `W` and cut the jump short before the assertion runs.
    ActionDriver driver;
    fixture.step(driver.pressedNow(Key::W));
    EXPECT_STATE(fixture, PlayerState::Air);

    fixture.step(driver.pressedNow(Key::D));
    for (int frame = 0; frame < 25; ++frame)
    {
        fixture.step(driver.held());
    }

    // Moving horizontally, still airborne, and still Air.
    CHECK(std::fabs(fixture.transform().velocity.x) > 1.0F);
    CHECK_FALSE(fixture.state().grounded);
    EXPECT_STATE(fixture, PlayerState::Air);
}

void testTheJumpFrameIsAirborneImmediately()
{
    // The frame the jump starts, the probe still finds the tile being left. The state
    // must be Air on that frame and not Stand - a one-frame Stand at the bottom of every
    // jump is a visible stutter, and it is exactly what happens if the state is decided
    // from `grounded` alone.
    PlayerFixture fixture{wideFloorLevel()};
    settle(fixture);

    ActionDriver jump;
    fixture.step(jump.pressedNow(Key::W));

    EXPECT_STATE(fixture, PlayerState::Air);
    CHECK(fixture.state().jumping);

    // And `grounded` is **false** on the launch frame, which is the Phase 16
    // limitation this phase removed.
    //
    // That group used to assert the opposite - that the probe still found the tile
    // being left, so the player was "grounded" and the standing sprite showed for one
    // frame at the bottom of every jump. `jumping` is what papered over it. Now the
    // player is genuinely unsupported the moment they leave the tile, because that is
    // what "no support collision was resolved this frame" means, and the assertion
    // below is the removal of the workaround rather than a change of expectation.
}

void testLandingReturnsToAGroundedState()
{
    PlayerFixture fixture{wideFloorLevel()};
    settle(fixture);

    ActionDriver jump;
    fixture.step(jump.pressedNow(Key::W));
    EXPECT_STATE(fixture, PlayerState::Air);

    // Fall back down and wait for the ground.
    const int frames = fixture.stepUntil(jump.held(), [&fixture] { return fixture.state().grounded; });
    CHECK(frames < kMaxFrames);
    CHECK(fixture.state().grounded);
    EXPECT_STATE(fixture, PlayerState::Stand);

    // Back on the floor at the height it started from - the jump did not leave it
    // somewhere else, and the collision did not sink it.
    CHECK_NEAR(fixture.playerY(), kPlayerRestY, 0.5F);

    // And the jump flag did not survive the landing.
    CHECK_FALSE(fixture.state().jumping);
}

void testWalkingOffALedgeMakesThePlayerAirborne()
{
    // Not a jump: the player leaves the ground with no input at all, and the state
    // machine has to notice from the probe rather than from a flag.
    PlayerFixture fixture{wideFloorLevel(1)};
    settle(fixture);
    EXPECT_STATE(fixture, PlayerState::Stand);

    ActionDriver driver;
    int airborneAt = -1;
    for (int frame = 0; frame < 120; ++frame)
    {
        fixture.step(frame == 0 ? driver.pressedNow(Key::D) : driver.held());
        if (airborneAt < 0 && fixture.state().state == PlayerState::Air)
        {
            airborneAt = frame;
        }
    }

    CHECK(airborneAt >= 0);
    EXPECT_STATE(fixture, PlayerState::Air);
    CHECK_FALSE(fixture.state().grounded);
}

// ---------------------------------------------------------------------------
// E. Jumping
// ---------------------------------------------------------------------------

void testTheJumpSpeedIsTheLevelsNotAConstant()
{
    // A level whose jump speed is not 400, because a test that only ever compares
    // against 400 cannot tell "read the level" from "wrote 400". The committed level
    // happens to say 400, so the mutation replacing the level's value with the literal
    // 400 passed every other group in this file.
    PlayerRecord record = playerRecordAt(0.0F, 1.0F);
    record.jumpSpeed = 250.0F;
    PlayerFixture fixture{levelWith(record)};
    settle(fixture);

    ActionDriver jump;
    fixture.step(jump.pressedNow(Key::W));

    CHECK_NEAR(fixture.transform().velocity.y, launchVelocity(250.0F), 0.0001F);
    EXPECT_STATE(fixture, PlayerState::Air);

    // And the peak follows the level's number, not the committed one: `v^2 / 2g` for
    // 250 is 34.7 pixels, against 88.9 for 400.
    float highest = fixture.playerY();
    for (int frame = 0; frame < 120; ++frame)
    {
        fixture.step(jump.held());
        highest = std::min(highest, fixture.playerY());
    }

    const float expectedPeak = kPlayerRestY - ((250.0F * 250.0F) / (2.0F * kLevelGravity));
    CHECK_NEAR(highest, expectedPeak, 3.5F);
}

void testTheJumpKeyLaunchesThePlayerAtTheLevelsJumpSpeed()
{
    PlayerFixture fixture{wideFloorLevel()};
    settle(fixture);

    ActionDriver jump;
    fixture.step(jump.pressedNow(Key::W));

    // The level's jump speed, upward, which in this engine is negative Y. Stated
    // against the level's own number rather than against `config.jumpSpeed` read
    // through the component, so this cannot pass by comparing a value with itself.
    CHECK_NEAR(fixture.transform().velocity.y, launchVelocity(kLevelJumpSpeed), 0.0001F);
    EXPECT_STATE(fixture, PlayerState::Air);
    CHECK(fixture.state().jumping);

    // And the player goes up.
    const float before = fixture.playerY();
    fixture.step(jump.held());
    CHECK(fixture.playerY() < before);
}

void testTheJumpOnlyWorksFromTheGround()
{
    PlayerFixture fixture{wideFloorLevel()};
    settle(fixture);

    // Jump.
    ActionDriver jump;
    fixture.step(jump.pressedNow(Key::W));
    CHECK_NEAR(fixture.transform().velocity.y, launchVelocity(kLevelJumpSpeed), 0.0001F);

    // Hold until the player is **falling**, because a mid-air jump cannot be observed
    // while the player is still rising: a re-launch would set the velocity to -400 and
    // gravity would immediately add 15, giving -385 - and so would a *missing* re-launch
    // from a velocity of -385, giving the same -400... no, the same -385. The first
    // version of this group compared velocities and passed with the grounded check
    // deleted, because gravity's 15 dominated every comparison.
    //
    // Descending, the two are unmistakable: a re-launch sends the player *up*, and
    // without one they keep going down. Position, not velocity.
    for (int frame = 0; frame < 120; ++frame)
    {
        fixture.step(jump.held());
        if (fixture.transform().velocity.y > 0.0F)
        {
            break;
        }
    }
    CHECK(fixture.transform().velocity.y > 0.0F);
    CHECK_FALSE(fixture.state().grounded);

    // Three fresh presses mid-descent. A fresh press, not a held key - the harder
    // case, and the one an `isActive` bug would also fail.
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        const float before = fixture.playerY();
        ActionDriver again;
        fixture.step(again.pressedNow(Key::W));

        // Still descending, on this very frame. A re-launch would have made the
        // velocity negative and the player would have moved up.
        CHECK(fixture.transform().velocity.y > 0.0F);
        CHECK(fixture.playerY() > before);
        EXPECT_STATE(fixture, PlayerState::Air);

        // The `jumping` flag is deliberately **not** required to be clear here. It
        // means "a jump this player started has not landed yet", and the descent is
        // part of that jump's arc, so it stays set until touchdown. What the mid-air
        // press must not do is add upward velocity, and the two checks above are
        // exactly that.
    }
}

void testHoldingTheJumpKeyJumpsOnlyOnce()
{
    // The course: "If the jump key is held, the player should not continuously jump,
    // but instead it should only jump once per button press."
    //
    // The test that catches an `isActive` implementation: hold W for longer than the
    // whole ascent and descent, and count the launches. An `isActive` jump would
    // re-launch on every frame it is held and the player would climb forever.
    PlayerFixture fixture{wideFloorLevel()};
    settle(fixture);

    ActionDriver jump;
    fixture.step(jump.pressedNow(Key::W));
    const float launchedAt = fixture.playerY();

    float highest = launchedAt;
    int framesAscending = 0;
    for (int frame = 0; frame < 120; ++frame)
    {
        fixture.step(jump.held());
        highest = std::min(highest, fixture.playerY());
        if (fixture.transform().velocity.y < 0.0F)
        {
            ++framesAscending;
        }
    }

    // A single launch. The peak is the height one jump speed reaches against one
    // gravity, which is `v^2 / (2g)` to within the integration error of a discrete
    // simulation - and, decisively, nowhere near the height 120 frames of continuous
    // jumping would reach.
    //
    // The tolerance is `v * dt / 2` = 3.33 pixels, and that is not slack: semi-implicit
    // Euler moves the position with the *new* velocity, so the last frame that is
    // still rising is a whole step short of the true apex, and it undershoots by
    // about half a step's worth of travel. Measured, the peak came out 3.36 pixels
    // low, which is the number to the second decimal.
    CHECK(framesAscending > 5);
    CHECK(framesAscending < 60);
    const float expectedPeak = kPlayerRestY - ((kLevelJumpSpeed * kLevelJumpSpeed) / (2.0F * kLevelGravity));
    CHECK_NEAR(highest, expectedPeak, 3.5F);

    // And the player came back down rather than hovering, which is the observable form
    // of "it did not jump again".
    CHECK(fixture.state().grounded);
    CHECK_NEAR(fixture.playerY(), kPlayerRestY, 0.5F);
}

void testAGroundedPlayerCannotJumpFromTheAir()
{
    // Stated as the complement of the group above: a player that has landed *can*
    // jump again, on a new press. Without this, "jumps only once" would be satisfied
    // by a player that could never jump at all.
    PlayerFixture fixture{wideFloorLevel()};
    settle(fixture);

    ActionDriver first;
    fixture.step(first.pressedNow(Key::W));
    for (int frame = 0; frame < 200; ++frame)
    {
        fixture.step(first.held());
        if (fixture.state().grounded && frame > 10)
        {
            break;
        }
    }
    CHECK(fixture.state().grounded);

    // A second, separate press, after the landing.
    ActionDriver second;
    fixture.step(second.pressedNow(Key::W));
    CHECK_NEAR(fixture.transform().velocity.y, launchVelocity(kLevelJumpSpeed), 0.0001F);
    EXPECT_STATE(fixture, PlayerState::Air);
}

// ---------------------------------------------------------------------------
// F. Variable jump height
// ---------------------------------------------------------------------------

void testAShortPressIsAShorterJumpThanAHeldKey()
{
    // The behaviour, measured as peak height, with the two runs otherwise identical.
    //
    // The short jump holds W for a single frame and then lets go. The long jump holds
    // it for the whole ascent. Anything that made them the same height - a missing
    // release rule, a timer that ignored the key, a gravity change - fails here, and
    // so does anything that made the short jump *higher*.
    const auto peakFor = [](const int heldFrames) {
        PlayerFixture fixture{wideFloorLevel()};
        settle(fixture);

        ActionDriver jump;
        fixture.step(jump.pressedNow(Key::W));
        float highest = fixture.playerY();
        for (int frame = 0; frame < 120; ++frame)
        {
            fixture.step(frame < heldFrames ? jump.held() : jump.release(Key::W));
            highest = std::min(highest, fixture.playerY());
        }
        return highest;
    };

    const float tap = peakFor(0);
    const float full = peakFor(40);

    // The tap rose at all - it is a jump, not nothing.
    CHECK(tap < kPlayerRestY);
    // And it rose less than half as far as the held jump.
    CHECK(tap > full - 1.0F);
    CHECK((kPlayerRestY - tap) < (kPlayerRestY - full) * 0.5F);

    // And the held jump reached the height one jump speed reaches against one gravity,
    // which ties the two ends of this to the level's own numbers.
    // The same `v * dt / 2` allowance as the group above; see it for why.
    const float expectedPeak = kPlayerRestY - ((kLevelJumpSpeed * kLevelJumpSpeed) / (2.0F * kLevelGravity));
    CHECK_NEAR(full, expectedPeak, 3.5F);
}

void testReleasingTheJumpKeyStopsTheAscentImmediately()
{
    // The course, in words: "If the player lets go of the jump key mid-jump, it should
    // start falling back down immediately."
    PlayerFixture fixture{wideFloorLevel()};
    settle(fixture);

    ActionDriver jump;
    fixture.step(jump.pressedNow(Key::W));
    CHECK_NEAR(fixture.transform().velocity.y, launchVelocity(kLevelJumpSpeed), 0.0001F);

    // Hold for a few frames so the player is genuinely climbing...
    for (int frame = 0; frame < 5; ++frame)
    {
        fixture.step(jump.held());
    }
    CHECK(fixture.transform().velocity.y < 0.0F);
    const float peakBefore = fixture.playerY();

    // ...then let go. Not "more slowly", not "eventually": the ascent ends here.
    //
    // `release`, not `idle`: an `idle` frame clears the press edge but leaves W
    // physically down, so `isActive` is still true and the release rule - correctly -
    // does not fire. Releasing the key is a different act from not pressing it again.
    fixture.step(jump.release(Key::W));
    // The ascent has ended, which is the claim. Not "the velocity is exactly zero":
    // the release sets it to zero and then gravity is applied in the *same* frame, so
    // what a caller sees is one gravity step of downward speed. Demanding zero here
    // would be demanding that gravity not act, and gravity always acts.
    CHECK(fixture.transform().velocity.y >= 0.0F);
    CHECK_FALSE(fixture.state().jumping);

    // And from that frame on, the player descends. Gravity is positive, so the next
    // frame's velocity is positive and the player goes down.
    fixture.step(jump.idle());
    CHECK(fixture.transform().velocity.y > 0.0F);
    CHECK(fixture.playerY() > peakBefore);
}

void testReleasingTheJumpKeyWhileFallingDoesNotCancelTheFall()
{
    // The guard `velocity.y < 0` in the release rule exists for this. Without it, a
    // player holding W as they fall would have their descent zeroed on every frame and
    // would hover at the apex forever.
    PlayerFixture fixture{wideFloorLevel()};
    settle(fixture);

    ActionDriver jump;
    fixture.step(jump.pressedNow(Key::W));

    // Hold past the apex and then keep falling, so the descent is **fast** before the
    // key is released.
    //
    // The frame count matters. Releasing at the first downward velocity catches the
    // player at 5 px/s, and at that speed the two behaviours are indistinguishable: the
    // guard-less release sets the velocity to zero and gravity puts it back to 15 in the
    // same frame, which is still "greater than 5". Letting the fall develop first makes
    // it 300 px/s or so, against 15 after a cancelled descent, and there is no reading
    // of that which confuses the two.
    //
    // 60 frames, because the arithmetic is worth writing down. The apex is reached at
    // frame 27 (400 / 15), and landing is a further 26 frames after that, so the
    // descent passes 300 px/s at about frame 47 and the floor arrives at about 53. A
    // 40-frame window stopped at 200, which is still too close to 15 to tell the two
    // behaviours apart; 60 comfortably reaches the threshold and still breaks before
    // the player lands.
    for (int frame = 0; frame < 60; ++frame)
    {
        fixture.step(jump.held());
        if (fixture.transform().velocity.y > 300.0F)
        {
            break;
        }
    }
    CHECK(fixture.transform().velocity.y > 300.0F);
    CHECK_FALSE(fixture.state().grounded);

    // Now stop holding.
    //
    // The check is on **this** frame, not ten frames later. The rule only applies while
    // ascending, so with the `velocity.y < 0` guard the release does nothing at all and
    // gravity adds to the descent; without the guard the release zeroes a descent that
    // is under way, and the fall has to re-accelerate from nothing. Ten frames later
    // both have caught up, which is why the first version of this group passed with
    // the guard deleted.
    const float fallingAt = fixture.transform().velocity.y;
    fixture.step(jump.release(Key::W));

    // The descent was not interrupted: still moving down, and at least as fast as it
    // was, plus this frame's gravity.
    CHECK(fixture.transform().velocity.y > 0.0F);
    CHECK(fixture.transform().velocity.y > fallingAt);

    // And it keeps accelerating rather than hovering.
    //
    // Two frames, not ten: at 300 px/s and 15 px/s per frame, ten frames of descent
    // would carry the player onto the floor, and the velocity would then be zero
    // because of the landing rather than because of the release. Two frames is +30,
    // which is twice the 15 a cancelled descent would have had, so the claim survives
    // without the player arriving anywhere.
    for (int frame = 0; frame < 2; ++frame)
    {
        fixture.step(jump.idle());
    }
    CHECK(fixture.transform().velocity.y > fallingAt + 30.0F);

    // And the fall is a *fall*: the player is well below the apex.
    const float apex = kPlayerRestY - ((kLevelJumpSpeed * kLevelJumpSpeed) / (2.0F * kLevelGravity));
    CHECK(fixture.playerY() > apex);
    CHECK(fixture.playerY() > kPlayerRestY - 200.0F);
}

void testVariableJumpDoesNotChangeTheGlobalPhysicsBehaviour()
{
    // The prompt forbids faking variable jump height by changing gravity globally, and
    // this is the group that says so.
    //
    // A level with **no jump input at all**, over many frames. If the player system
    // altered shared physics state, or a global gravity, or the physics system's own
    // behaviour, this fall would not match `g * t^2`. It is the same arithmetic a
    // plain free fall obeys, and it is asserted as a range rather than an equality
    // because semi-implicit Euler is not exact.
    PlayerFixture fixture{wideFloorLevel()};
    settle(fixture);

    // A floor far below, so the player is in free fall for the whole measurement.
    PlayerFixture falling{wideFloorLevel(), 100000.0F};
    settle(falling, 1);

    // Put it well above the floor so there is room.
    // "Above the floor" is *smaller* y here, since y grows down.
    const float startY = kFloorTop - 300.0F;
    falling.transform().position = Vec2{falling.playerX(), startY};

    constexpr int frames = 30;
    for (int frame = 0; frame < frames; ++frame)
    {
        falling.step(ActionState{});
    }

    // Free fall from rest: `y = y0 + 0.5 * g * t^2`. The discrete form lands a little
    // short of that, so the check is a range, and the point is the *order of
    // magnitude*: a gravity change of any kind would be off by a large factor.
    //
    // The discrete fall is a half-step *past* the continuous one, for the same reason
    // the discrete jump peak is a half-step short: semi-implicit Euler uses the new
    // velocity to move, so after n steps it has travelled `g*dt^2*n*(n+1)/2` against a
    // continuous `g*dt^2*n^2/2`. The excess here is `0.5*g*dt^2*n` = 3.75 pixels, so the
    // band is that wide and it is centred on the continuous answer.
    const float expected = startY + (0.5F * kLevelGravity * ((frames * kFrame) * (frames * kFrame)));
    CHECK(falling.playerY() > expected - 1.0F);
    CHECK(falling.playerY() < expected + 4.0F);
}

void testADynamicBodyIsNotAFloor()
{
    // The course says the player "land on a **Tile** entity". A dynamic entity is not a
    // tile, and the ground probe has to agree - otherwise a player would stand on a
    // falling bullet, and the two would push each other forever.
    //
    // This is the only way the body-type half of the probe is observable, and it took
    // a mutation that deleted the check to notice: with the committed level there is
    // exactly one dynamic body, the player, and the probe skips the entity it is asking
    // about, so every collider it looked at was already static and the check could not
    // be reached. A check that cannot be reached is a check that does not exist.
    //
    // The arrangement: the player in the air, and a second **dynamic** box directly
    // beneath it, with no floor in the world at all.
    Level level = levelWith(playerRecordAt(0.0F, 4.0F), 0);
    PlayerFixture fixture{level, 100000.0F};

    // A dynamic body just below the player, in mid-air.
    engine::ecs::Entity& crate = fixture.world().addEntity("crate");
    crate.addComponent<Transform>(Transform{Vec2{fixture.playerX(), fixture.playerY() + 40.0F},
                                            Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
    crate.addComponent<Collider>(Collider{Vec2{40.0F, 40.0F}});
    crate.addComponent<Body>(Body{engine::physics::BodyType::Dynamic});

    // One frame. The player must still be falling: a dynamic body is not a floor.
    fixture.step(ActionState{});

    CHECK_FALSE(fixture.state().grounded);
    EXPECT_STATE(fixture, PlayerState::Air);

    // And over several frames it keeps descending past the dynamic body, rather than
    // settling on it.
    const float startY = fixture.playerY();
    for (int frame = 0; frame < 30; ++frame)
    {
        fixture.step(ActionState{});
    }
    CHECK(fixture.playerY() > startY);

    // The complement, and it is what makes the group a test of the *body type* rather
    // than of the probe generally: a **static** body in the same place is a floor.
    PlayerFixture staticFixture{levelWith(playerRecordAt(0.0F, 4.0F), 0), 100000.0F};
    engine::ecs::Entity& platform = staticFixture.world().addEntity("platform");
    platform.addComponent<Transform>(Transform{Vec2{staticFixture.playerX(), staticFixture.playerY() + 40.0F},
                                               Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
    platform.addComponent<Collider>(Collider{Vec2{40.0F, 40.0F}});
    platform.addComponent<Body>(Body{engine::physics::BodyType::Static});

    staticFixture.step(ActionState{});
    staticFixture.step(ActionState{});
    CHECK(staticFixture.state().grounded);
    EXPECT_STATE(staticFixture, PlayerState::Stand);
}

// ---------------------------------------------------------------------------
// G. Gravity
// ---------------------------------------------------------------------------

void testGravityAcceleratesThePlayerDownward()
{
    PlayerFixture fixture{wideFloorLevel(), 100000.0F};
    settle(fixture, 1);
    fixture.transform().position = Vec2{fixture.playerX(), 500.0F};

    ActionState nothing;
    fixture.step(nothing);
    // First frame: gravity has been added to a zero velocity, and physics has
    // integrated it.
    CHECK(fixture.transform().velocity.y > 0.0F);
    CHECK_NEAR(fixture.transform().velocity.y, kLevelGravity * kFrame, 0.0001F);
    CHECK(fixture.playerY() > 500.0F);

    // Second frame: larger, by gravity again. Free acceleration, not a constant
    // terminal velocity.
    fixture.step(nothing);
    CHECK_NEAR(fixture.transform().velocity.y, kLevelGravity * kFrame * 2.0F, 0.0001F);
}

void testLandingClearsTheDownwardVelocity()
{
    // The course's landing behaviour, and the prompt's "the player does not remain
    // stuck with a stale negative vertical velocity" in the positive direction: a
    // landed player has no downward speed at all, not a small one.
    PlayerFixture fixture{wideFloorLevel()};
    settle(fixture);

    // Fall from height so the impact speed is real.
    fixture.transform().position = Vec2{fixture.playerX(), kFloorTop - 300.0F};
    for (int frame = 0; frame < 200; ++frame)
    {
        fixture.step(ActionState{});
        if (fixture.state().grounded)
        {
            break;
        }
    }
    CHECK(fixture.state().grounded);

    // The physics system resolved the landing and zeroed the component; the player
    // system then keeps it at zero rather than accumulating.
    CHECK_NEAR(fixture.transform().velocity.y, 0.0F, 0.0001F);
    CHECK(fixture.transform().velocity.y >= 0.0F);

    // And it stays at zero across more frames, which is where a "gravity is applied
    // every frame and the collision cancels it" implementation would show a
    // non-zero value between frames.
    for (int frame = 0; frame < 30; ++frame)
    {
        fixture.step(ActionState{});
    }
    CHECK_NEAR(fixture.transform().velocity.y, 0.0F, 0.0001F);
}

void testAGroundedPlayerCarriesNoVerticalSpeed()
{
    PlayerFixture fixture{wideFloorLevel()};
    settle(fixture);

    for (int frame = 0; frame < 60; ++frame)
    {
        fixture.step(ActionState{});
    }

    CHECK(fixture.state().grounded);
    CHECK_NEAR(fixture.transform().velocity.y, 0.0F, 0.0001F);
    // And the player has not sunk into the floor over a second of standing.
    CHECK_NEAR(fixture.playerY(), kPlayerRestY, 0.5F);
}

// ---------------------------------------------------------------------------
// H. Respawn
// ---------------------------------------------------------------------------

void testFallingOutOfTheWorldRespawnsThePlayer()
{
    // The course: "If the player falls below the bottom of the screen, they respawn at
    // the start."
    //
    // "Below the bottom" is **larger** y in this engine, so the world ends at 1024 and
    // the player has to be pushed past it, not towards zero. The first version of this
    // group used a fall limit of 200 and dropped the player at 400 - which put the
    // limit *above* the floor at 960, so the player respawned on every frame of every
    // group in this file and every height assertion was really asserting that the
    // player had been teleported home.
    PlayerFixture fixture{wideFloorLevel()};

    fixture.transform().position = Vec2{fixture.playerX(), kWorldHeight + 200.0F};
    fixture.transform().velocity = Vec2{120.0F, 250.0F};
    fixture.transform().scale = Vec2{-1.0F, 1.0F};
    fixture.state().state = PlayerState::Air;
    fixture.state().jumping = true;

    fixture.step(ActionState{});

    // Position restored to the level's spawn point, not to a nearby guess.
    CHECK_NEAR(fixture.playerX(), fixture.state().spawnPosition.x, 0.0001F);
    CHECK_NEAR(fixture.playerY(), fixture.state().spawnPosition.y, 0.0001F);

    // Velocity cleared, both axes. Restoring only the position would look right for one
    // frame and then shoot the player off at 250 px/s.
    CHECK_NEAR(fixture.transform().velocity.x, 0.0F, 0.0001F);
    CHECK_NEAR(fixture.transform().velocity.y, 0.0F, 0.0001F);

    // State reset.
    EXPECT_STATE(fixture, PlayerState::Stand);
    CHECK(fixture.state().grounded);
    CHECK_FALSE(fixture.state().jumping);

    // Facing reset to the spawn facing, which is right.
    CHECK_NEAR(fixture.transform().scale.x, 1.0F, 0.0001F);

    // The animation is valid: the state selection at the end of the frame picked the
    // standing picture, and the frame index was reset rather than carried over.
    CHECK(fixture.animation().assetName == "megaman_megaStand_stand");
    CHECK_NEAR(static_cast<float>(fixture.animation().currentFrame), 0.0F, 0.0001F);
    CHECK_NEAR(static_cast<float>(fixture.animation().ticksOnFrame), 0.0F, 0.0001F);
    CHECK_FALSE(fixture.animation().ended);
    CHECK(fixture.animation().repeat);
}

void testNoStaleJumpStateSurvivesARespawn()
{
    // The specific thing the prompt calls out. A respawn that restored the position but
    // left `jumping` set would let the player cut a jump that does not exist, and - worse
    // - would keep the player in Air, so they would fall straight through the world
    // again and respawn forever.
    PlayerFixture fixture{wideFloorLevel()};

    // Settled first, and that is now a precondition rather than a convenience.
    //
    // A freshly spawned player is airborne - the loader says so honestly - so the jump
    // gate refuses the press on the very first frame, correctly. Phase 16 got away
    // with jumping on frame 1 because its *probe* had already found the floor by then;
    // collision-based grounding has no such shortcut, and pretending otherwise would
    // have meant reintroducing one.
    settle(fixture);

    // Get genuinely airborne, so the state is Air with a live jump.
    ActionDriver jump;
    fixture.step(jump.pressedNow(Key::W));
    EXPECT_STATE(fixture, PlayerState::Air);
    CHECK(fixture.state().jumping);

    // Then teleport out of the world mid-ascent.
    fixture.transform().position = Vec2{fixture.playerX(), kWorldHeight + 200.0F};
    fixture.step(jump.held());

    CHECK(fixture.state().grounded);
    EXPECT_STATE(fixture, PlayerState::Stand);
    CHECK_NEAR(fixture.transform().velocity.y, 0.0F, 0.0001F);

    // And the player stays put: the next thirty frames must not walk them back down.
    for (int frame = 0; frame < 30; ++frame)
    {
        fixture.step(ActionState{});
    }
    CHECK_NEAR(fixture.playerY(), fixture.state().spawnPosition.y, 0.0001F);
    EXPECT_STATE(fixture, PlayerState::Stand);
}

void testARespawnedPlayerCanJumpImmediately()
{
    // A respawn that left the player ungrounded would trap them: they would fall, miss
    // the floor check, and respawn again, forever. This is the group that catches it.
    PlayerFixture fixture{wideFloorLevel()};

    fixture.transform().position = Vec2{fixture.playerX(), kWorldHeight + 200.0F};
    fixture.step(ActionState{});
    CHECK(fixture.state().grounded);

    ActionDriver jump;
    fixture.step(jump.pressedNow(Key::W));
    CHECK_NEAR(fixture.transform().velocity.y, launchVelocity(kLevelJumpSpeed), 0.0001F);
    EXPECT_STATE(fixture, PlayerState::Air);
}

void testTheFallLimitIsTheWorldsBottomEdge()
{
    // The respawn groups above pass a limit of 200 to keep them fast. This is the group
    // that says the *production* limit is derived from the world rather than invented,
    // by reading the number off a real play scene.
    engine::Application application;
    application.changeScene(engine::scene::SceneId::Play);
    application.update();

    const auto* play = dynamic_cast<const engine::scene::PlayScene*>(application.currentScene());
    CHECK(play != nullptr);
    if (play == nullptr)
    {
        return;
    }

    // `PlayerSystem` is first, and its fall limit is the world height in pixels.
    CHECK(play->systems().systemCount() == 6U);
    if (play->systems().systemCount() != 6U)
    {
        return;
    }
    CHECK(std::string{play->systems().systemAt(0U).name()} == "PlayerSystem");

    const auto* player = dynamic_cast<const engine::systems::PlayerSystem*>(&play->systems().systemAt(0U));
    CHECK(player != nullptr);
    if (player == nullptr)
    {
        return;
    }

    const float cellsTall = static_cast<float>(engine::config::kLevelCellsTall);
    CHECK_NEAR(player->fallLimitY(), cellsTall * engine::level::LevelGrid::kCellSize, 0.0001F);
    CHECK_NEAR(player->fallLimitY(), 16.0F * 64.0F, 0.0001F);
}

void testAPlayerAboveTheFallLimitIsNotRespawned()
{
    // The other side of the boundary. A respawn that triggered on the way *up* - or
    // whenever the player was merely low - would make the world unplayable, and a
    // check that only ever tests the falling case would not notice.
    PlayerFixture fixture{wideFloorLevel()};

    // Standing on a floor at y=94, comfortably above a limit of 400.
    settle(fixture);
    CHECK(fixture.playerY() < kWorldHeight);

    for (int frame = 0; frame < 60; ++frame)
    {
        fixture.step(ActionState{});
    }
    EXPECT_STATE(fixture, PlayerState::Stand);
    CHECK_NEAR(fixture.playerY(), kPlayerRestY, 0.5F);

    // And just above the limit, the player still does not respawn - the test is about
    // the feet crossing it, not the centre.
    fixture.transform().position = Vec2{fixture.playerX(), 400.0F - 30.0F};
    fixture.step(ActionState{});
    CHECK_NEAR(fixture.playerY(), 370.0F, 1.0F);
}

// ---------------------------------------------------------------------------
// I. Animation integration
// ---------------------------------------------------------------------------

void testEachStateSelectsItsOwnAnimation()
{
    // Through real input and the real state machine. Nothing here sets a state, so
    // nothing here can pass by testing a setter.
    PlayerFixture fixture{wideFloorLevel()};
    settle(fixture);

    // Stand.
    EXPECT_STATE(fixture, PlayerState::Stand);
    CHECK(fixture.animation().assetName == "megaman_megaStand_stand");

    // Run.
    ActionDriver run;
    for (int frame = 0; frame < 5; ++frame)
    {
        fixture.step(frame == 0 ? run.pressedNow(Key::D) : run.held());
    }
    EXPECT_STATE(fixture, PlayerState::Run);
    CHECK(fixture.animation().assetName == "megaman_megaStand_stand");

    // Air.
    ActionDriver jump;
    fixture.step(jump.pressedNow(Key::W));
    EXPECT_STATE(fixture, PlayerState::Air);
    CHECK(fixture.animation().assetName == "megaman_megaJump_air");
}

void testTheAnimationChangesWhenTheStateChanges()
{
    // Both directions, and the change is *immediate* - the frame the state changes is
    // the frame the animation changes, because PlayerSystem runs before
    // AnimationSystem and picks the picture the same frame it is advanced.
    PlayerFixture fixture{wideFloorLevel()};
    settle(fixture);
    CHECK(fixture.animation().assetName == "megaman_megaStand_stand");

    ActionDriver jump;
    fixture.step(jump.pressedNow(Key::W));
    // The very next line, with no extra frames: not one frame later.
    CHECK(fixture.animation().assetName == "megaman_megaJump_air");

    // Land, and it goes back on the same frame the state does.
    for (int frame = 0; frame < 200; ++frame)
    {
        fixture.step(jump.held());
        if (fixture.state().grounded && frame > 5)
        {
            break;
        }
    }
    CHECK(fixture.state().grounded);
    CHECK(fixture.animation().assetName == "megaman_megaStand_stand");
}

void testTheAnimationFrameResetsWhenTheAnimationChanges()
{
    // The failure this exists to catch: [engine::components::Animation] carries
    // `currentFrame`, `ticksOnFrame` and `ended` for the *previous* animation, and
    // carried across a change they are all wrong. `ended` in particular is the
    // dangerous one, because [engine::systems::AnimationSystem] destroys any entity
    // whose animation reports itself ended - so a stale `ended` would delete the
    // player on the frame its state changed, leaving a level with no player in it and
    // no error anywhere.
    //
    // ### Arranged so the reset is what saves the player
    //
    // A stale name and a stale `ended` are put on the entity by hand while the player
    // is airborne, so the very next [engine::systems::PlayerSystem] pass sees a name
    // that is not the one the state calls for and must reset all of it. If the reset
    // were missing, `ended` would survive into that same frame's `AnimationSystem` pass
    // and the player would be gone - which is the assertion.
    //
    // Setting `ended` and then *waiting* would not test the reset: the player would be
    // destroyed on the very next frame, before the landing that would have changed the
    // animation. The first version of this group did exactly that and the failure it
    // reported was an escaped exception rather than a failed check.
    //
    // `currentFrame` and `ticksOnFrame` are deliberately **not** asserted here: both
    // player animations are single frame at speed 1, so `advanceAnimation` returns
    // them to zero every frame and there is nothing to observe. The reset is still
    // performed for them - the code sets all four fields together - and a multi-frame
    // player animation would be the thing that made them observable.
    PlayerFixture fixture{wideFloorLevel()};
    settle(fixture);

    // Airborne, so the system is selecting the air animation.
    ActionDriver jump;
    fixture.step(jump.pressedNow(Key::W));
    EXPECT_STATE(fixture, PlayerState::Air);
    CHECK(fixture.animation().assetName == "megaman_megaJump_air");

    // Now leave behind exactly the debris a finished previous animation would: the
    // wrong name, a frame index part way along, a non-zero tick count, `ended` set, and
    // `repeat` cleared.
    fixture.animation().assetName = "megaman_megaStand_stand";
    fixture.animation().currentFrame = 1U;
    fixture.animation().ticksOnFrame = 5U;
    fixture.animation().ended = true;
    fixture.animation().repeat = false;

    // One frame. The player is still airborne, so the system wants the air animation -
    // a different name from the one it finds - and must reset the lot.
    fixture.step(jump.held());

    CHECK(fixture.animation().assetName == "megaman_megaJump_air");
    CHECK_NEAR(static_cast<float>(fixture.animation().currentFrame), 0.0F, 0.0001F);
    CHECK_NEAR(static_cast<float>(fixture.animation().ticksOnFrame), 0.0F, 0.0001F);
    CHECK_FALSE(fixture.animation().ended);
    CHECK(fixture.animation().repeat);

    // And the player is still in the world. A stale `ended` would have had
    // `AnimationSystem` flag it for destruction and the end-of-frame flush would have
    // removed it.
    CHECK(fixture.world().getEntities(engine::level::kPlayerTag).begin() !=
          fixture.world().getEntities(engine::level::kPlayerTag).end());
    CHECK(fixture.world().aliveEntityCount() > 0U);

    // And it completes the jump and lands, which the whole sequence depends on.
    for (int frame = 0; frame < 200; ++frame)
    {
        fixture.step(jump.held());
        if (fixture.state().grounded)
        {
            break;
        }
    }
    CHECK(fixture.state().grounded);
    CHECK(fixture.animation().assetName == "megaman_megaStand_stand");
}void testThePlayerAnimationLoopsSoItIsNeverDestroyed()
{
    // A player's animation must repeat. A non-repeating one would report itself ended
    // after its single frame, and AnimationSystem would delete the player - leaving a
    // level with no player in it and no error anywhere.
    PlayerFixture fixture{wideFloorLevel()};
    settle(fixture);

    for (int frame = 0; frame < 600; ++frame)
    {
        fixture.step(ActionState{});
    }

    CHECK(fixture.animation().repeat);
    CHECK_FALSE(fixture.animation().ended);
    CHECK(fixture.world().getEntities(engine::level::kPlayerTag).begin() !=
          fixture.world().getEntities(engine::level::kPlayerTag).end());
}

void testEveryAnimationThePlayerCanSelectIsADeclaredAsset()
{
    // The names are resolved through the **real** asset table, so a typo or a removed
    // declaration throws here rather than at the first frame of a game.
    //
    // All three states are checked even though Run and Stand currently share a picture,
    // because the sharing is a documented limitation rather than a design, and a later
    // phase that fixes the run artwork must not have to rediscover that a third name
    // has to exist.
    engine::assets::SfmlAssetManager assets{std::filesystem::path{engine::config::kAssetsConfig}};

    CHECK(assets.animation("megaman_megaStand_stand").frameCount() == 1U);
    CHECK(assets.animation("megaman_megaJump_air").frameCount() == 1U);

    // And the run picture is the standing one, which is a *decision* rather than an
    // accident. `megaman_megaRun` cannot be declared: 733 pixels, the course's 3
    // frames, 733 / 3 is 244.33, and 733 is prime.
    bool threw = false;
    try
    {
        static_cast<void>(assets.animation("megaman_megaRun"));
    }
    catch (const engine::assets::AssetNotFoundError&)
    {
        threw = true;
    }
    CHECK(threw);
}

// ---------------------------------------------------------------------------
// The update order
// ---------------------------------------------------------------------------

void testPlayerBehaviourLandsInTheSameFrameAsTheInput()
{
    // The reason `PlayerSystem` is registered *before* `PhysicsSystem`, observable.
    //
    // One frame of `D` must move the player by `speed * dt`. If the player system ran
    // after physics, the movement would appear on the *next* frame and this would be
    // zero.
    PlayerFixture fixture{wideFloorLevel()};
    settle(fixture);

    const float before = fixture.playerX();
    ActionDriver driver;
    fixture.step(driver.pressedNow(Key::D));

    CHECK_NEAR(fixture.playerX() - before, kLevelLeftRightSpeed * kFrame, 0.001F);
    // And the state changed on the same frame too, not one frame later.
    EXPECT_STATE(fixture, PlayerState::Run);
}

void testThePlayerSystemOnlyActsOnEntitiesThatArePlayers()
{
    // The reason the system exists rather than reusing
    // [engine::systems::MovementSystem]: that one sets a velocity on **every**
    // transform, so the ground tiles and the clouds would walk. This group is the
    // negative of that - a tile in the same world must not move.
    PlayerFixture fixture{wideFloorLevel()};

    ActionDriver driver;
    for (int frame = 0; frame < 60; ++frame)
    {
        fixture.step(frame == 0 ? driver.pressedNow(Key::D) : driver.held());
    }

    // Six floor tiles, all still exactly where the grid put them: cell `i` centres at
    // `i * 64 + 32` across, and at `1024 - 32` down - the bottom row of the world.
    //
    // Counted rather than indexed, so a tile that moved is reported against its own
    // position instead of against an index that no longer means anything.
    std::size_t tiles = 0U;
    for (const Entity& entity : fixture.world().getEntities(engine::level::kTileTag))
    {
        ++tiles;
        const Transform& transform = entity.getComponent<Transform>();
        const float expectedX = ((static_cast<float>(tiles) - 1.0F) * kCell) + (kCell * 0.5F);
        CHECK_NEAR(transform.position.x, expectedX, 0.0001F);
        CHECK_NEAR(transform.position.y, kWorldHeight - (kCell * 0.5F), 0.0001F);
        CHECK_NEAR(transform.velocity.x, 0.0F, 0.0001F);
        CHECK_NEAR(transform.velocity.y, 0.0F, 0.0001F);
    }
    CHECK(tiles == 6U);
}

} // namespace

int main()
{
    const std::pair<const char*, void (*)()> testCases[] = {
        // A. Spawn
        {"the player is spawned from level data", &testThePlayerIsSpawnedFromLevelData},
        {"the spawn position comes from the grid cell", &testTheSpawnPositionComesFromTheGridCell},
        {"a freshly spawned player is not grounded", &testAFreshlySpawnedPlayerIsNotGrounded},
        {"the sprite and the collider share the transform position",
         &testTheSpriteAndTheColliderShareTheTransformPosition},
        {"the committed level spawns a player with those numbers",
         &testTheCommittedLevelSpawnsAPlayerWithThoseNumbers},
        // B. Movement
        {"moving right walks right", &testMovingRightWalksRight},
        {"moving left walks left", &testMovingLeftWalksLeft},
        {"releasing movement stops the player", &testReleasingMovementStopsThePlayer},
        {"the walk speed is the level's, not a constant", &testTheWalkSpeedIsTheLevelsNotAConstant},
        {"the maximum speed clamps in both directions", &testTheMaximumSpeedClampsInBothDirections},
        {"the player can move in the air", &testThePlayerCanMoveInTheAir},
        // C. Facing
        {"facing follows the direction pressed", &testFacingFollowsTheDirectionPressed},
        {"facing survives stopping, jumping and landing", &testFacingSurvivesStoppingJumpingAndLanding},
        {"jumping does not reset facing", &testJumpingDoesNotResetFacing},
        // D. State machine
        {"a still grounded player is standing", &testAStillGroundedPlayerIsStanding},
        {"a moving grounded player is running", &testAMovingGroundedPlayerIsRunning},
        {"an airborne player is in Air even while moving horizontally",
         &testAnAirbornePlayerIsInAirEvenWhileMovingHorizontally},
        {"the jump frame is airborne immediately", &testTheJumpFrameIsAirborneImmediately},
        {"landing returns to a grounded state", &testLandingReturnsToAGroundedState},
        {"walking off a ledge makes the player airborne", &testWalkingOffALedgeMakesThePlayerAirborne},
        // E. Jump
        {"the jump speed is the level's, not a constant", &testTheJumpSpeedIsTheLevelsNotAConstant},
        {"the jump key launches the player at the level's jump speed",
         &testTheJumpKeyLaunchesThePlayerAtTheLevelsJumpSpeed},
        {"the jump only works from the ground", &testTheJumpOnlyWorksFromTheGround},
        {"holding the jump key jumps only once", &testHoldingTheJumpKeyJumpsOnlyOnce},
        {"a grounded player can jump again after landing", &testAGroundedPlayerCannotJumpFromTheAir},
        // F. Variable jump
        {"a short press is a shorter jump than a held key",
         &testAShortPressIsAShorterJumpThanAHeldKey},
        {"releasing the jump key stops the ascent immediately",
         &testReleasingTheJumpKeyStopsTheAscentImmediately},
        {"releasing the jump key while falling does not cancel the fall",
         &testReleasingTheJumpKeyWhileFallingDoesNotCancelTheFall},
        {"variable jump does not change the global physics behaviour",
         &testVariableJumpDoesNotChangeTheGlobalPhysicsBehaviour},
        // G. Gravity
        {"a dynamic body is not a floor", &testADynamicBodyIsNotAFloor},
        {"gravity accelerates the player downward", &testGravityAcceleratesThePlayerDownward},
        {"landing clears the downward velocity", &testLandingClearsTheDownwardVelocity},
        {"a grounded player carries no vertical speed", &testAGroundedPlayerCarriesNoVerticalSpeed},
        // H. Respawn
        {"falling out of the world respawns the player", &testFallingOutOfTheWorldRespawnsThePlayer},
        {"no stale jump state survives a respawn", &testNoStaleJumpStateSurvivesARespawn},
        {"a respawned player can jump immediately", &testARespawnedPlayerCanJumpImmediately},
        {"the fall limit is the world's bottom edge", &testTheFallLimitIsTheWorldsBottomEdge},
        {"a player above the fall limit is not respawned", &testAPlayerAboveTheFallLimitIsNotRespawned},
        // I. Animation
        {"each state selects its own animation", &testEachStateSelectsItsOwnAnimation},
        {"the animation changes when the state changes", &testTheAnimationChangesWhenTheStateChanges},
        {"the animation frame resets when the animation changes",
         &testTheAnimationFrameResetsWhenTheAnimationChanges},
        {"the player animation loops so it is never destroyed", &testThePlayerAnimationLoopsSoItIsNeverDestroyed},
        {"every animation the player can select is a declared asset",
         &testEveryAnimationThePlayerCanSelectIsADeclaredAsset},
        // The order
        {"player behaviour lands in the same frame as the input",
         &testPlayerBehaviourLandsInTheSameFrameAsTheInput},
        {"the player system only acts on entities that are players",
         &testThePlayerSystemOnlyActsOnEntitiesThatArePlayers},
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

    std::cout << groupCount << " player behaviour test groups passed\n";
    return EXIT_SUCCESS;
}
