/// Combat and tile behaviour: shooting, bullet travel and destruction, brick
/// explosions, the question block, and the coin it drops.
///
/// ### What is driven and what is observed
///
/// Every group runs the **real** pipeline in the production order -
/// [engine::systems::PlayerSystem], [engine::systems::ShootSystem],
/// [engine::systems::LifetimeSystem], [engine::systems::PhysicsSystem],
/// [engine::systems::PlayerStateSystem], [engine::systems::TileSystem],
/// [engine::systems::AnimationSystem] - against a world spawned by the **real**
/// [engine::level::LevelLoader] from level data, with animations resolved through the
/// **real** committed asset table. Input is pressed as **keys** on a real
/// [engine::input::Input] and resolved through the real default action map, the way
/// [engine::Application] does it.
///
/// Nothing here reads back a component the system under test wrote and compares it with
/// itself. A bullet's direction is asserted by firing and reading the bullet's velocity; a
/// coin's lifetime is asserted by stepping frames until it is gone; an explosion is asserted
/// by watching the animation advance to its end and then watching the entity disappear. The
/// only values written out by hand are the geometry and the level's own numbers, both so a
/// group cannot pass by comparing the implementation with itself.
///
/// ### The world these tests build
///
/// ```text
///   world y 992   a 64x64 floor tile in grid row 0
///   world y 960   the top of the floor - where a player's feet rest
///   world y 930   a 40x60 player standing there (its centre)
///   world y 928   a 64x64 tile in grid row 1 - where a fired bullet flies
///   world y 800   a 64x64 tile in grid row 3 - high enough to jump into
/// ```
///
/// The three heights are the whole geometry, and they are not interchangeable. A bullet
/// leaves the player at the player's centre - world y 930 - so only a tile in grid row 1 is
/// on its path; and a jump reaches `400^2 / (2 * 900) = 88.9` pixels above the floor, so
/// only a tile in grid row 3 can be hit from underneath. Putting both kinds of target at
/// one height would make one of the two behaviours untestable.
///
/// The player's column matters too, and it is not a detail. A block is resolved along the
/// axis of **least** penetration, so a player rising into a block's underside is stopped
/// upwards only once its vertical penetration exceeds its horizontal one - and a wide
/// horizontal overlap means the player is nudged sideways for several frames before it ever
/// gets a clean upward stop. The fixture puts the player under a block with a *small*
/// horizontal overlap (its box clips the block's by eight pixels) so the very first contact
/// resolves vertically, which is the case the phase is about.
///
/// ### Where a hand-built entity is necessary, and why
///
/// The question block's committed artwork is 360x360. The course sizes a tile's bounding
/// box from its animation, so a question block spawned by the real loader has a box five and
/// a half cells across that no player could stand next to, and the behaviour this phase has
/// to test would be unreachable through it. So the question-block groups build their block
/// by hand, with the same five components the loader produces and a collider of one grid
/// cell.
///
/// That is a property of the artwork rather than a shortcut in the test, so it is pinned
/// rather than only described: `the loader gives the question block its real 360 pixel box`
/// asserts what the real path does with the real image, and the shipped level contains no
/// question block for the reason `assets/assets.txt` records. The *behaviour* is exercised at
/// a cell size, through the real systems.
#include "engine/Application.hpp"
#include "engine/EngineConfig.hpp"
#include "engine/assets/SfmlAssetManager.hpp"
#include "engine/components/Animation.hpp"
#include "engine/components/Body.hpp"
#include "engine/components/Bullet.hpp"
#include "engine/components/Collider.hpp"
#include "engine/components/Lifetime.hpp"
#include "engine/components/Player.hpp"
#include "engine/components/PlayerConfig.hpp"
#include "engine/components/Tile.hpp"
#include "engine/components/Transform.hpp"
#include "engine/ecs/EntityManager.hpp"
#include "engine/ecs/SystemManager.hpp"
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
#include "engine/physics/Collision.hpp"
#include "engine/scene/PlayScene.hpp"
#include "engine/scene/Scene.hpp"
#include "engine/systems/AnimationSystem.hpp"
#include "engine/systems/LifetimeSystem.hpp"
#include "engine/systems/PhysicsSystem.hpp"
#include "engine/systems/PlayerStateSystem.hpp"
#include "engine/systems/PlayerSystem.hpp"
#include "engine/systems/ShootSystem.hpp"
#include "engine/systems/TileSystem.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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
using engine::components::Bullet;
using engine::components::Collider;
using engine::components::Lifetime;
using engine::components::Player;
using engine::components::PlayerConfig;
using engine::components::PlayerState;
using engine::components::Tile;
using engine::components::TileType;
using engine::components::Transform;
using engine::ecs::Entity;
using engine::ecs::EntityId;
using engine::ecs::EntityManager;
using engine::ecs::SystemManager;
using engine::input::Action;
using engine::input::ActionState;
using engine::input::Input;
using engine::input::Key;
using engine::level::DecorationRecord;
using engine::level::Level;
using engine::level::LevelGrid;
using engine::level::LevelLoader;
using engine::level::PlayerRecord;
using engine::level::TileRecord;
using engine::physics::Aabb;
using engine::physics::BodyType;
using engine::physics::Collision;
using engine::physics::CollisionReport;
using engine::systems::AnimationSystem;
using engine::systems::LifetimeSystem;
using engine::systems::PhysicsSystem;
using engine::systems::PlayerStateSystem;
using engine::systems::PlayerSystem;
using engine::systems::ShootSystem;
using engine::systems::TileSystem;

// One sixtieth of a second: the engine's frame cap, so a test that stepped with another
// number would be testing another game.
constexpr float kFrame = 1.0F / 60.0F;

/// A bound on a bound: ten seconds of simulated time. Long enough that no behaviour under
/// test takes it, short enough that a behaviour which never arrives is a failed assertion
/// rather than a hung suite.
constexpr int kMaxFrames = 600;

// The world, in pixels. See the file header.
constexpr float kCell = LevelGrid::kCellSize;
constexpr float kWorldHeight = 16.0F * kCell;
constexpr float kFloorTop = kWorldHeight - kCell;
constexpr float kPlayerHeight = 60.0F;
constexpr float kPlayerWidth = 40.0F;
constexpr float kRowOneCentreY = kWorldHeight - (1.0F * kCell) - (kCell * 0.5F);
constexpr float kRowThreeCentreY = kWorldHeight - (3.0F * kCell) - (kCell * 0.5F);

/// A grid cell's centre height in world pixels, for a one-cell animation.
[[nodiscard]] float rowCentreY(const float gridY) noexcept
{
    return kWorldHeight - (gridY * kCell) - (kCell * 0.5F);
}

/// The committed player's numbers, restated so a group can say "the level says 200" without
/// reading the value it is about to compare against.
constexpr float kLevelLeftRightSpeed = 200.0F;
constexpr float kLevelJumpSpeed = 400.0F;
constexpr float kLevelMaxSpeed = 250.0F;
constexpr float kLevelGravity = 900.0F;

/// The course's bullet speed factor, from `velocity.x = scale.x * SPEED * 6`.
constexpr float kCourseBulletSpeedFactor = 6.0F;

/// The course's bullet spawn offset, from `entityPos.x += 64`.
constexpr float kCourseBulletSpawnOffset = 64.0F;

/// The course's coin height above the question block, in world pixels.
constexpr float kCourseCoinSpawnHeight = 64.0F;

/// The course's coin lifetime, in game frames. See [engine::components::Lifetime].
constexpr std::uint32_t kCourseCoinLifetimeFrames = 30U;

/// The course's bullet lifespan, 1.8231 seconds, in game frames at this engine's 60 FPS:
/// `1.8231 * 60` is 109.4, and a whole number of frames is the honest way to say it.
constexpr std::uint32_t kCourseBulletLifetimeFrames = 109U;

/// The committed buster's real frame size, measured from the artwork.
constexpr float kBulletFrameWidth = 32.0F;
constexpr float kBulletFrameHeight = 26.0F;

/// The committed level's own numbers, so a group can shoot from the shipped level and say
/// what it expects without re-reading the file.
constexpr float kCommittedBulletSpeed = 200.0F * kCourseBulletSpeedFactor;

/// Forward declarations for the three id helpers, which the fixture's callers need before
/// the definitions further down.
[[nodiscard]] bool entityAlive(const EntityManager& world, EntityId id);
[[nodiscard]] Entity& entityOf(const EntityManager& world, EntityId id);

/// Drives the real action map with real key presses.
class ActionDriver
{
public:
    /// A fresh press: active *and* pressed this frame.
    [[nodiscard]] ActionState pressedNow(const Key key)
    {
        m_input.processKeyDown(key);
        return snapshot();
    }

    /// The frames after a press, where the key is still physically down but is no longer a
    /// new press. This is the entire distinction between an edge-triggered action and a
    /// level-triggered one, and it is what several groups below depend on.
    [[nodiscard]] ActionState held()
    {
        m_input.beginFrame();
        return snapshot();
    }

    /// Nothing held, and no edge.
    [[nodiscard]] ActionState idle()
    {
        m_input.beginFrame();
        return snapshot();
    }

    /// One frame in which `key` has come up. A frame with no new press still has the key
    /// physically down, so releasing needs the key to actually go up.
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

/// A player record at a cell, with the committed level's numbers.
[[nodiscard]] PlayerRecord playerAt(const float gridX, const float gridY)
{
    PlayerRecord record;
    record.gridX = gridX;
    record.gridY = gridY;
    record.boundingBoxSize = Vec2{kPlayerWidth, kPlayerHeight};
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

// ---------------------------------------------------------------------------
// The fixture: the real systems, in the production order, over a real world.
// ---------------------------------------------------------------------------

class CombatFixture
{
public:
    explicit CombatFixture(const Level& level, const float fallLimitY = kWorldHeight)
        : m_assets{std::filesystem::path{engine::config::kAssetsConfig}}
    {
        // The production order, minus the camera and the zoom keys, which need a camera
        // this fixture has no use for. All seven of the rest are registered exactly as
        // [engine::scene::PlayScene] registers them, because two of the positions are the
        // phase rather than a preference:
        //
        //   ShootSystem before LifetimeSystem before PhysicsSystem, so a bullet created
        //   this frame is integrated this frame and an entity whose last frame is this one
        //   neither moves nor collides;
        //   TileSystem straight after PlayerStateSystem, so both read the same report and
        //   the explosion it starts is advanced the same frame.
        m_systems.add<PlayerSystem>(fallLimitY);
        m_systems.add<ShootSystem>(m_assets);
        m_systems.add<LifetimeSystem>();
        m_physics = &m_systems.add<PhysicsSystem>();
        m_systems.add<PlayerStateSystem>(m_physics->collisions());
        m_systems.add<TileSystem>(m_physics->collisions(), m_assets);
        m_systems.add<AnimationSystem>(m_assets);

        const LevelLoader loader{m_assets};
        const LevelGrid grid = LevelGrid::withCellsTall(16.0F);
        static_cast<void>(loader.spawn(level, m_world, grid));
    }

    /// A tile built by hand, with the loader's component set.
    ///
    /// The animation name and the [TileType] are separate arguments so a group can produce
    /// a tile whose *artwork* says one thing and whose *type* says another - which is how
    /// "the type decides, not the picture" is checked rather than asserted.
    Entity& addHandBuiltTile(const float gridX, const float gridY, const std::string& animationName,
                             const TileType type, const Vec2 size = Vec2{kCell, kCell})
    {
        const LevelGrid grid = LevelGrid::withCellsTall(16.0F);
        Entity& entity = m_world.addEntity(std::string{engine::level::kTileTag});
        entity.addComponent<Transform>(
            Transform{grid.centreOf(gridX, gridY, size), Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
        entity.addComponent<Animation>(Animation{animationName, 0U, 0U, true, false});
        entity.addComponent<Collider>(Collider{size});
        entity.addComponent<Body>(Body{BodyType::Static});
        entity.addComponent<Tile>(Tile{type, false});
        return entity;
    }

    /// One frame, in the production order, followed by the owner's cleanup.
    void step(const ActionState& actions, const float deltaSeconds = kFrame)
    {
        m_systems.update(m_world, actions, deltaSeconds);
        m_world.update();
    }

    template <typename Predicate>
    int stepUntil(const ActionState& actions, const Predicate& until, const int frames = kMaxFrames,
                  const float deltaSeconds = kFrame)
    {
        // Steps **then** checks. Checking first would return on the frame the system wrote
        // the state and never run the frame that acts on it.
        for (int frame = 0; frame < frames; ++frame)
        {
            step(actions, deltaSeconds);
            if (until())
            {
                return frame;
            }
        }
        return frames;
    }

    // Deliberately **const** member functions returning mutable references, exactly as
    // [engine::ecs::EntityView] yields `const Entity&` whose components are writable. A
    // const fixture is a fixture nobody has structurally changed, and the whole component API
    // is reachable through it.
    [[nodiscard]] Entity& player() const
    {
        // The const is dropped for the walk, exactly as `getEntities()` needs it dropped for
        // the other accessors: a `const` fixture means "nobody has structurally changed
        // this", not "this world is read-only". The engine's own model is the same one -
        // [engine::ecs::EntityView] yields `const Entity&` whose components are writable.
        EntityManager& world = const_cast<EntityManager&>(m_world);

        for (auto&& [entity, transform, config, state, animation] :
             world.query<Transform, PlayerConfig, Player, Animation>())
        {
            static_cast<void>(transform);
            static_cast<void>(config);
            static_cast<void>(state);
            static_cast<void>(animation);
            return entity;
        }
        throw std::logic_error{"the world has no player"};
    }

    [[nodiscard]] Transform& transform() const { return player().getComponent<Transform>(); }
    [[nodiscard]] Player& state() const { return player().getComponent<Player>(); }
    [[nodiscard]] PlayerConfig& config() const { return player().getComponent<PlayerConfig>(); }
    [[nodiscard]] Collider& collider() const { return player().getComponent<Collider>(); }
    [[nodiscard]] EntityManager& world() noexcept { return m_world; }
    [[nodiscard]] const EntityManager& world() const noexcept { return m_world; }
    [[nodiscard]] SystemManager& systems() noexcept { return m_systems; }
    [[nodiscard]] const AssetManager& assets() const noexcept { return m_assets; }
    [[nodiscard]] const CollisionReport& collisions() const noexcept { return m_physics->collisions(); }

    [[nodiscard]] float playerX() const { return transform().position.x; }
    [[nodiscard]] float playerY() const { return transform().position.y; }
    [[nodiscard]] float playerHalfWidth() const { return collider().size.x * 0.5F; }

    [[nodiscard]] Aabb playerBox() const { return Aabb{transform().position, collider().size}; }

    // -- Bullets ---------------------------------------------------------------

    [[nodiscard]] std::size_t bulletCount() const
    {
        return countWhere([](const Entity& entity) { return entity.hasComponent<Bullet>(); });
    }

    [[nodiscard]] Entity& bulletAt(std::size_t index) const
    {
        std::size_t seen = 0;
        for (const Entity& entity : m_world.getEntities())
        {
            if (entity.hasComponent<Bullet>())
            {
                if (seen == index)
                {
                    return const_cast<Entity&>(entity);
                }
                ++seen;
            }
        }
        throw std::logic_error{"no bullet at index " + std::to_string(index)};
    }

    [[nodiscard]] Entity& onlyBullet() const { return bulletAt(0U); }
    [[nodiscard]] Transform& bulletTransform(Entity& bullet) { return bullet.getComponent<Transform>(); }
    [[nodiscard]] Collider& bulletCollider(Entity& bullet) { return bullet.getComponent<Collider>(); }
    [[nodiscard]] Animation& bulletAnimation(Entity& bullet) { return bullet.getComponent<Animation>(); }
    [[nodiscard]] Lifetime& bulletLifetime(Entity& bullet) { return bullet.getComponent<Lifetime>(); }
    [[nodiscard]] Body& bulletBody(Entity& bullet) { return bullet.getComponent<Body>(); }

    // -- Tiles -----------------------------------------------------------------

    [[nodiscard]] std::size_t tileCount() const
    {
        return countWhere([](const Entity& entity) { return entity.hasComponent<Tile>(); });
    }

    [[nodiscard]] std::size_t tileCount(const TileType type) const
    {
        std::size_t count = 0;
        for (const Entity& entity : m_world.getEntities())
        {
            if (const Tile* const tile = entity.tryGetConstComponent<Tile>())
            {
                if (tile->type == type)
                {
                    ++count;
                }
            }
        }
        return count;
    }

    /// The single tile of `type`, or throws when there is not exactly one.
    [[nodiscard]] Entity& onlyTile(const TileType type) const
    {
        std::size_t seen = 0;
        for (const Entity& entity : m_world.getEntities())
        {
            if (const Tile* const tile = entity.tryGetConstComponent<Tile>())
            {
                if (tile->type == type)
                {
                    ++seen;
                    if (seen > 1U)
                    {
                        break;
                    }
                }
            }
        }

        if (seen != 1U)
        {
            throw std::logic_error{"expected exactly one tile of that type, found " + std::to_string(seen)};
        }

        for (const Entity& entity : m_world.getEntities())
        {
            const Tile* const tile = entity.tryGetConstComponent<Tile>();
            if (tile != nullptr && tile->type == type)
            {
                return const_cast<Entity&>(entity);
            }
        }

        throw std::logic_error{"expected exactly one tile of that type"};
    }

    [[nodiscard]] Transform& tileTransform(Entity& tile) { return tile.getComponent<Transform>(); }
    [[nodiscard]] Tile& tileState(Entity& tile) { return tile.getComponent<Tile>(); }
    [[nodiscard]] Animation& tileAnimation(Entity& tile) { return tile.getComponent<Animation>(); }

    /// Whether the tile still has a collider, which is the whole of "is it still solid".
    [[nodiscard]] bool tileIsSolid(Entity& tile) const { return tile.hasComponent<Collider>(); }

    /// The id of the tile whose centre is exactly `(x, y)`, or throws.
    ///
    /// For naming *which* tile a report record refers to when the world has a dozen of the
    /// same kind. Taken from the transform rather than from an index, so it says "the tile I
    /// put at column three" rather than "the fourth one the loader happened to create".
    [[nodiscard]] EntityId tileIdAt(const float x, const float y) const
    {
        for (const Entity& entity : m_world.getEntities())
        {
            if (!entity.hasComponent<Tile>() || !entity.hasComponent<Transform>())
            {
                continue;
            }

            const Vec2 position = entity.getComponent<Transform>().position;
            if (position.x == x && position.y == y)
            {
                return entity.id();
            }
        }

        throw std::logic_error{"no tile at (" + std::to_string(x) + ", " + std::to_string(y) + ")"};
    }

    // -- Coins -----------------------------------------------------------------

    [[nodiscard]] std::size_t coinCount() const
    {
        return countWhere([](const Entity& entity) { return entity.tag() == engine::components::kCoinTag; });
    }

    [[nodiscard]] Entity& onlyCoin() const
    {
        std::size_t seen = 0;
        for (const Entity& entity : m_world.getEntities())
        {
            if (entity.tag() == engine::components::kCoinTag)
            {
                ++seen;
            }
        }

        if (seen != 1U)
        {
            throw std::logic_error{"expected exactly one coin, found " + std::to_string(seen)};
        }

        for (const Entity& entity : m_world.getEntities())
        {
            if (entity.tag() == engine::components::kCoinTag)
            {
                return const_cast<Entity&>(entity);
            }
        }

        throw std::logic_error{"expected exactly one coin"};
    }

    [[nodiscard]] Transform& coinTransform(Entity& coin) { return coin.getComponent<Transform>(); }

private:
    template <typename Predicate>
    [[nodiscard]] std::size_t countWhere(const Predicate& predicate) const
    {
        std::size_t count = 0;
        for (const Entity& entity : m_world.getEntities())
        {
            if (predicate(entity))
            {
                ++count;
            }
        }
        return count;
    }

    engine::assets::SfmlAssetManager m_assets;
    EntityManager m_world;
    SystemManager m_systems;
    const PhysicsSystem* m_physics = nullptr;
};

// ---------------------------------------------------------------------------
// The levels the groups play in.
// ---------------------------------------------------------------------------

/// A floor `tiles` cells wide, a player standing on it, and no other tile.
[[nodiscard]] Level floorAndPlayer(const float playerGridX = 0.0F, const float playerGridY = 1.0F,
                                   const int tiles = 12)
{
    Level level;
    for (int index = 0; index < tiles; ++index)
    {
        level.addTile(tileAt(static_cast<float>(index), 0.0F, "mario_ground_tile"));
    }
    level.setPlayer(playerAt(playerGridX, playerGridY));
    return level;
}

/// A floor, a player, and one more tile at `targetGridX, targetGridY`.
[[nodiscard]] Level floorPlayerAndTile(const float targetGridX, const float targetGridY,
                                       const std::string_view animationName, const float playerGridX = 0.0F)
{
    Level level = floorAndPlayer(playerGridX);
    level.addTile(tileAt(targetGridX, targetGridY, animationName));
    return level;
}

/// The player's column for the from-below groups.
///
/// Half a cell, so the player's 40-wide box clips the 64-wide block above it by eight
/// pixels rather than sitting squarely under it. See the file header: a block is resolved
/// along the axis of **least** penetration, so a wide horizontal overlap means the player is
/// nudged sideways for several frames before it ever gets a clean upward stop, and the phase
/// is about the upward stop.
constexpr float kUnderBlockColumn = 0.5F;

/// The player's column for the landing groups.
///
/// The full cell, so the player's box sits squarely under the block with a *large* horizontal
/// overlap. That is the opposite requirement to the one above, and for the same reason: a
/// player falling onto a block starts with almost no vertical penetration, so a small
/// horizontal overlap would be pushed out sideways before it ever landed.
constexpr float kUnderBlockColumnForLanding = 1.0F;

/// Settles the player onto the floor.
///
/// A freshly spawned player is one frame of falling away from rest, because the loader
/// honestly reports it as airborne. A few idle frames get it onto the floor at a known
/// state, so a group starts from standing rather than from spawn arithmetic.
void settle(CombatFixture& fixture, const int frames = 12)
{
    for (int frame = 0; frame < frames; ++frame)
    {
        fixture.step(ActionState{});
    }
}

/// Where a bullet's centre should be, given a stationary player at `playerX` facing
/// `facing`.
///
/// The offset *and* one frame of travel, because the bullet is created before the physics
/// step and therefore moves on the frame it is fired. Writing the two terms separately is the
/// point: a system that created bullets after the integration would land on the offset alone,
/// and one that used the wrong offset would land nowhere.
///
/// Only valid while the player is standing still, which every caller arranges - a moving
/// player would make the comparison two different frames.
[[nodiscard]] float expectedBulletX(const float playerX, const float facing) noexcept
{
    return playerX + (facing * kCourseBulletSpawnOffset) + (facing * kLevelLeftRightSpeed * kCourseBulletSpeedFactor * kFrame);
}

/// Whether the player is standing against the left face of a wall at `faceX`.
///
/// A tolerance rather than an equality, and the reason is the resolver rather than the
/// test: the player is walking right at 200 px/s and moves 3.33 pixels per frame, so the
/// position it is left in after a frame in which it moved and was pushed back can be anywhere
/// in a 3.33-pixel window against the surface. What the assertion is about is *arriving* and
/// *not getting through*, so the window is one frame of movement wide.
[[nodiscard]] bool blockedAgainstLeftFace(const CombatFixture& fixture, const float faceX, const float faceRight)
{
    const float right = fixture.playerX() + fixture.playerHalfWidth();
    return right >= faceX - 0.01F && right <= faceX + (kLevelLeftRightSpeed * kFrame) && fixture.playerX() < faceRight;
}

// ===========================================================================
// A. Shooting
// ===========================================================================

void testSpaceCreatesExactlyOneBullet()
{
    ActionDriver driver;
    CombatFixture fixture{floorAndPlayer()};
    settle(fixture);

    CHECK(fixture.bulletCount() == 0U);

    fixture.step(driver.pressedNow(Key::Space));

    CHECK(fixture.bulletCount() == 1U);

    // And it is an ECS entity with the components a moving body needs, not a special object
    // owned by the shoot system. A body is what makes the physics step integrate it.
    Entity& bullet = fixture.onlyBullet();
    CHECK(bullet.hasComponent<Transform>());
    CHECK(bullet.hasComponent<Animation>());
    CHECK(bullet.hasComponent<Collider>());
    CHECK(bullet.hasComponent<Body>());
    CHECK(bullet.hasComponent<Bullet>());
    CHECK(bullet.hasComponent<Lifetime>());
    CHECK(fixture.bulletBody(bullet).type == BodyType::Dynamic);
}

void testHoldingSpaceDoesNotCreateABulletEveryFrame()
{
    ActionDriver driver;
    CombatFixture fixture{floorAndPlayer()};
    settle(fixture);

    fixture.step(driver.pressedNow(Key::Space));
    CHECK(fixture.bulletCount() == 1U);

    // Thirty frames of the key still being physically down. The action is *active* on every
    // one of them, so a level-triggered system would fire sixty times a second.
    for (int frame = 0; frame < 30; ++frame)
    {
        fixture.step(driver.held());
    }

    // One. The first shot has also flown a long way by now, so the count is not one because
    // the others all died - it is one because twenty-nine were never made.
    CHECK(fixture.bulletCount() == 1U);

    // Releasing and pressing again is a second shot, which is what "once per button press"
    // means from the other side.
    fixture.step(driver.release(Key::Space));
    fixture.step(driver.pressedNow(Key::Space));
    CHECK(fixture.bulletCount() == 2U);
}

void testNoKeyAtAllCreatesNoBullet()
{
    ActionDriver driver;
    CombatFixture fixture{floorAndPlayer()};
    settle(fixture);

    for (int frame = 0; frame < 20; ++frame)
    {
        fixture.step(driver.idle());
    }

    CHECK(fixture.bulletCount() == 0U);
}

void testShootingRightPutsTheBulletToTheRightAndItGoesRight()
{
    ActionDriver driver;
    CombatFixture fixture{floorAndPlayer()};
    settle(fixture);

    // The player spawns facing right, which is the state Phase 16 established and the one a
    // freshly spawned player is in.
    CHECK_NEAR(fixture.transform().scale.x, 1.0F, 0.0001F);

    fixture.step(driver.pressedNow(Key::Space));

    Entity& bullet = fixture.onlyBullet();
    CHECK_NEAR(fixture.bulletTransform(bullet).position.x, expectedBulletX(fixture.playerX(), 1.0F), 0.01F);
    CHECK_NEAR(fixture.bulletTransform(bullet).position.y, fixture.playerY(), 0.0001F);
    CHECK(fixture.bulletTransform(bullet).velocity.x > 0.0F);
}

void testShootingLeftPutsTheBulletToTheLeftAndItGoesLeft()
{
    ActionDriver driver;
    CombatFixture fixture{floorAndPlayer()};
    settle(fixture);

    // Facing follows a *pressed direction*, so the player has to have walked left at least
    // once. This is the engine's own way of facing and the bullet reads it.
    fixture.step(driver.pressedNow(Key::A));
    CHECK_NEAR(fixture.transform().scale.x, -1.0F, 0.0001F);

    // Let go and stop, so the player is not still moving on the frame the shot is fired.
    // Otherwise the bullet's offset would be measured from where the player *started* the
    // frame and the assertion would be comparing two different moments.
    fixture.step(driver.release(Key::A));
    fixture.step(driver.idle());
    CHECK_NEAR(fixture.transform().velocity.x, 0.0F, 0.0001F);

    fixture.step(driver.pressedNow(Key::Space));

    Entity& bullet = fixture.onlyBullet();
    CHECK_NEAR(fixture.bulletTransform(bullet).position.x, expectedBulletX(fixture.playerX(), -1.0F), 0.01F);
    CHECK_NEAR(fixture.bulletTransform(bullet).position.y, fixture.playerY(), 0.0001F);
    CHECK(fixture.bulletTransform(bullet).velocity.x < 0.0F);
}

void testABulletKeepsTheDirectionItWasFiredIn()
{
    ActionDriver driver;
    CombatFixture fixture{floorAndPlayer()};
    settle(fixture);

    fixture.step(driver.pressedNow(Key::Space));
    Entity& bullet = fixture.onlyBullet();
    const float firedVelocity = fixture.bulletTransform(bullet).velocity.x;
    const float firedPosition = fixture.bulletTransform(bullet).position.x;
    CHECK(firedVelocity > 0.0F);

    // The player turns around and walks back towards the shot. A bullet is not a beam: its
    // direction belongs to the moment it was created, and nothing downstream reads the
    // player's facing again.
    for (int frame = 0; frame < 12; ++frame)
    {
        fixture.step(driver.pressedNow(Key::A));
    }

    CHECK_NEAR(fixture.transform().scale.x, -1.0F, 0.0001F);
    CHECK_NEAR(fixture.bulletTransform(bullet).velocity.x, firedVelocity, 0.0001F);
    CHECK(fixture.bulletTransform(bullet).position.x > firedPosition);
    CHECK(fixture.bulletTransform(bullet).position.x > fixture.playerX());
}

void testTheBulletSpeedIsTheLevelsSpeedTimesSix()
{
    ActionDriver driver;
    CombatFixture fixture{floorAndPlayer()};
    settle(fixture);

    fixture.step(driver.pressedNow(Key::Space));

    // Written as the level's own speed times the course's factor, so a bullet that ignored
    // [engine::components::PlayerConfig] and used a constant would fail here, and so would
    // one that used the player's speed without the factor.
    CHECK_NEAR(fixture.bulletTransform(fixture.onlyBullet()).velocity.x, kLevelLeftRightSpeed * kCourseBulletSpeedFactor,
               0.0001F);
    CHECK_NEAR(fixture.bulletTransform(fixture.onlyBullet()).velocity.y, 0.0F, 0.0001F);
}

void testTheBulletMovesInTheFrameItIsFired()
{
    ActionDriver driver;
    CombatFixture fixture{floorAndPlayer()};
    settle(fixture);

    fixture.step(driver.pressedNow(Key::Space));
    Entity& bullet = fixture.onlyBullet();
    const float born = fixture.bulletTransform(bullet).position.x;

    fixture.step(driver.held());

    // One frame of integration at the level's speed times the course's factor. Had the bullet
    // been created *after* the physics step, this difference would have been zero.
    CHECK_NEAR(fixture.bulletTransform(bullet).position.x - born, kLevelLeftRightSpeed * kCourseBulletSpeedFactor * kFrame,
               0.01F);
}

void testTheBulletBoxIsHalfItsAnimationFrame()
{
    ActionDriver driver;
    CombatFixture fixture{floorAndPlayer()};
    settle(fixture);

    fixture.step(driver.pressedNow(Key::Space));

    // The committed buster is 32x26 and the course halves it: `getSize() / 2`. Read from the
    // real asset manager rather than hard-coded, so this is a statement about the *rule* and
    // not about one number.
    const engine::assets::Animation& definition = fixture.assets().animation("megaman_megaBuster_shot");
    CHECK_NEAR(static_cast<float>(definition.frameWidth()), kBulletFrameWidth, 0.001F);
    CHECK_NEAR(static_cast<float>(definition.frameHeight()), kBulletFrameHeight, 0.001F);

    const Vec2 size = fixture.bulletCollider(fixture.onlyBullet()).size;
    CHECK_NEAR(size.x, kBulletFrameWidth * 0.5F, 0.0001F);
    CHECK_NEAR(size.y, kBulletFrameHeight * 0.5F, 0.0001F);
}

void testTheBulletIsDrawnWithTheLevelsBulletAnimationAndNeverEnds()
{
    ActionDriver driver;
    CombatFixture fixture{floorAndPlayer()};
    settle(fixture);

    fixture.step(driver.pressedNow(Key::Space));
    Entity& bullet = fixture.onlyBullet();

    CHECK(fixture.bulletAnimation(bullet).assetName == "megaman_megaBuster_shot");

    // A one-frame animation that does not repeat is *ended* after a single tick and
    // [engine::systems::AnimationSystem] would destroy the bullet on the frame it was made.
    // So a bullet's lifetime has to be [engine::components::Lifetime]'s business, and this is
    // where that is pinned rather than only documented.
    CHECK(fixture.bulletAnimation(bullet).repeat);

    for (int frame = 0; frame < 5; ++frame)
    {
        fixture.step(driver.held());
    }

    CHECK(fixture.bulletCount() == 1U);
}

// ===========================================================================
// B. Bullet collision
// ===========================================================================

void testABulletThatHitsASolidTileIsDestroyed()
{
    ActionDriver driver;
    CombatFixture fixture{floorPlayerAndTile(3.0F, 1.0F, "mario_ground_tile")};
    settle(fixture);

    // The tile is in grid row 1, which spans world y 896..960, and the player stands with its
    // centre at 930 - squarely on the bullet's path.
    CHECK_NEAR(rowCentreY(1.0F), kRowOneCentreY, 0.0001F);

    fixture.step(driver.pressedNow(Key::Space));
    CHECK(fixture.bulletCount() == 1U);

    fixture.stepUntil(driver.held(), [&fixture]() { return fixture.bulletCount() == 0U; }, 60U);

    CHECK(fixture.bulletCount() == 0U);

    // The tile itself is untouched: a bullet does not damage ordinary ground, it only stops
    // against it. Every tile in the world - the floor run and the wall - is still solid.
    for (const Entity& entity : fixture.world().getEntities(engine::level::kTileTag))
    {
        CHECK(entity.hasComponent<Collider>());
    }
}

void testTheBulletThatDiedIsOnePhysicsReported()
{
    ActionDriver driver;
    CombatFixture fixture{floorPlayerAndTile(3.0F, 1.0F, "mario_ground_tile")};
    settle(fixture);

    fixture.step(driver.pressedNow(Key::Space));
    const EntityId bulletId = fixture.onlyBullet().id();
    const EntityId tileId = fixture.tileIdAt((3.0F * kCell) + (kCell * 0.5F), kRowOneCentreY);

    // Every frame, was the bullet's death accompanied by a collision record naming both of
    // them? This is what "the bullet collision uses the report" means observably: the death
    // and the record happen on the same frame, and the record names the same two entities.
    // Nothing in the fixture ever looks for "a tile near the bullet".
    bool foundRecord = false;
    for (int frame = 0; frame < 60 && !foundRecord; ++frame)
    {
        fixture.step(driver.held());

        for (const Collision& collision : fixture.collisions().collisions())
        {
            if (engine::physics::isParticipant(collision, bulletId) &&
                engine::physics::partnerOf(collision, bulletId) == tileId)
            {
                foundRecord = true;
            }
        }
    }

    CHECK(foundRecord);
    CHECK(fixture.bulletCount() == 0U);
}

void testABulletCannotPassThroughASolidTile()
{
    ActionDriver driver;
    CombatFixture fixture{floorPlayerAndTile(3.0F, 1.0F, "mario_ground_tile")};
    settle(fixture);

    const float tileRight = (3.0F * kCell) + kCell;

    fixture.step(driver.pressedNow(Key::Space));

    // Track the bullet's own position every frame. It must never be beyond the tile's far
    // edge, which is worth more than "it eventually vanished": a bullet that reached the far
    // side and then disappeared would pass a test that only checked the count.
    float furthest = 0.0F;
    for (int frame = 0; frame < 60 && fixture.bulletCount() == 1U; ++frame)
    {
        fixture.step(driver.held());
        if (fixture.bulletCount() == 1U)
        {
            furthest = std::max(furthest, fixture.bulletTransform(fixture.onlyBullet()).position.x);
        }
    }

    CHECK(fixture.bulletCount() == 0U);
    CHECK(furthest < tileRight);
}

void testADecorationIsNeitherCollidableNorABulletTarget()
{
    ActionDriver driver;

    Level level = floorAndPlayer();
    // The buster's path: world y 930 is level y 94, and the bush is centred on it, halfway
    // along the level in x.
    level.addDecoration(DecorationRecord{"mario_BigBush_dec", 300.0F, kWorldHeight - 930.0F, 1U});

    CombatFixture fixture{level};
    settle(fixture);

    const EntityId decorationId = fixture.world().getEntities(engine::level::kDecorationTag).begin()->id();

    // A decoration is drawn, and nothing else. No box, no body - which is the whole of why it
    // cannot be collided with, and no flag anywhere saying "decoration".
    const Entity& decoration = entityOf(fixture.world(), decorationId);
    CHECK_FALSE(decoration.hasComponent<Collider>());
    CHECK_FALSE(decoration.hasComponent<Body>());
    CHECK(decoration.hasComponent<Transform>());
    CHECK(decoration.hasComponent<Animation>());
    CHECK_FALSE(decoration.hasComponent<Tile>());

    fixture.step(driver.pressedNow(Key::Space));

    // The strongest form of the claim: across forty frames of flight, the cloud is never named
    // in a collision record, and the bullet goes straight through where it was.
    bool namedInAReport = false;
    float furthest = 0.0F;
    for (int frame = 0; frame < 40; ++frame)
    {
        fixture.step(driver.held());

        for (const Collision& collision : fixture.collisions().collisions())
        {
            if (engine::physics::isParticipant(collision, decorationId))
            {
                namedInAReport = true;
            }
        }

        if (fixture.bulletCount() == 1U)
        {
            furthest = std::max(furthest, fixture.bulletTransform(fixture.onlyBullet()).position.x);
        }
    }

    CHECK_FALSE(namedInAReport);
    CHECK(fixture.bulletCount() == 1U);
    CHECK(furthest > 300.0F);
}

void testABulletThatMissesEverythingIsStillAliveAfterManyFrames()
{
    ActionDriver driver;
    CombatFixture fixture{floorAndPlayer()};
    settle(fixture);

    fixture.step(driver.pressedNow(Key::Space));
    const float born = fixture.bulletTransform(fixture.onlyBullet()).position.x;

    // Well past the point where it would have reached anything - the floor is the only tile in
    // range, and it is a cell below the bullet's path.
    for (int frame = 0; frame < 10; ++frame)
    {
        fixture.step(driver.held());
    }

    CHECK(fixture.bulletCount() == 1U);
    CHECK(fixture.bulletTransform(fixture.onlyBullet()).position.x > born + 100.0F);
}

void testABulletExpiresAfterItsFramesAndNotBefore()
{
    ActionDriver driver;
    CombatFixture fixture{floorAndPlayer()};
    settle(fixture);

    fixture.step(driver.pressedNow(Key::Space));

    // Alive well inside the life, so the group cannot pass by expiring immediately. The count
    // includes the frame it was created on, so one frame short of the end is still alive.
    for (std::uint32_t frame = 0; frame + 2U < kCourseBulletLifetimeFrames; ++frame)
    {
        fixture.step(driver.held());
    }
    CHECK(fixture.bulletCount() == 1U);

    // And gone by the end of it.
    fixture.stepUntil(driver.held(), [&fixture]() { return fixture.bulletCount() == 0U; }, 8U);
    CHECK(fixture.bulletCount() == 0U);
}

void testABulletLifetimeIsCountedInFramesNotSeconds()
{
    // The same number of frames at a different frame rate. A wall-clock lifespan - the
    // course's `sf::Clock` - would remove the bullet at a *different frame number* here,
    // because 109 frames of a fiftieth of a second is more than two seconds.
    ActionDriver driver;

    CombatFixture fixture{floorAndPlayer()};
    settle(fixture);
    fixture.step(driver.pressedNow(Key::Space));

    for (std::uint32_t frame = 0; frame + 2U < kCourseBulletLifetimeFrames; ++frame)
    {
        fixture.step(driver.held(), 1.0F / 20.0F);
    }

    // Still there at 20 FPS after the same number of frames it survives at 60, and removed on
    // the same frame number - which a wall-clock count cannot do.
    CHECK(fixture.bulletCount() == 1U);
    fixture.stepUntil(driver.held(), [&fixture]() { return fixture.bulletCount() == 0U; }, 8U, 1.0F / 20.0F);
    CHECK(fixture.bulletCount() == 0U);
}

// ===========================================================================
// C. Bricks
// ===========================================================================

/// A floor, a player half a cell in from the left, and a brick in grid row 3.
///
/// The brick is above the player, which is the arrangement that can be hit from underneath.
[[nodiscard]] Level floorPlayerAndHighBrick()
{
    Level level = floorAndPlayer(kUnderBlockColumn);
    level.addTile(tileAt(1.0F, 3.0F, "mario_Brick_tile"));
    return level;
}

void testABrickStartsWholeAndSolid()
{
    CombatFixture fixture{floorPlayerAndHighBrick()};

    Entity& brick = fixture.onlyTile(TileType::Brick);

    CHECK(fixture.tileIsSolid(brick));
    CHECK(fixture.tileAnimation(brick).assetName == "mario_Brick_tile");
    CHECK(fixture.tileAnimation(brick).repeat);
    CHECK_FALSE(fixture.tileState(brick).activated);

    // One grid cell, from the ground tile's own artwork - which is the course's `TexBrick`.
    const Vec2 size = brick.getComponent<Collider>().size;
    CHECK_NEAR(size.x, kCell, 0.0001F);
    CHECK_NEAR(size.y, kCell, 0.0001F);

    // And placed from its grid cell like any other tile.
    CHECK_NEAR(fixture.tileTransform(brick).position.y, kRowThreeCentreY, 0.0001F);
    CHECK_NEAR(fixture.tileTransform(brick).position.x, kCell + (kCell * 0.5F), 0.0001F);
}

void testTheCommittedLevelClassifiesItsBricks()
{
    // The shipped level, through a real [engine::scene::PlayScene]: the real loader over the
    // real asset table.
    engine::Application application;
    application.changeScene(engine::scene::SceneId::Play);
    application.update();

    const auto* play = dynamic_cast<const engine::scene::PlayScene*>(application.currentScene());
    CHECK(play != nullptr);
    if (play == nullptr)
    {
        return;
    }

    std::size_t bricks = 0;
    std::size_t solids = 0;
    std::size_t untype = 0;
    for (const Entity& entity : play->world().getEntities())
    {
        const Tile* const tile = entity.tryGetConstComponent<Tile>();
        if (tile == nullptr)
        {
            continue;
        }

        if (tile->type == TileType::Brick)
        {
            ++bricks;
            CHECK(entity.hasComponent<Collider>());
            CHECK(entity.getComponent<Animation>().assetName == "mario_Brick_tile");
            CHECK_FALSE(tile->activated);
        }
        else
        {
            ++solids;
        }
    }

    CHECK(bricks == 2U);
    CHECK(solids == 24U);

    // Every level tile is classified, and no decoration is. A decoration has no
    // [engine::components::Tile] at all, which is what keeps it out of gameplay without a
    // flag anywhere saying "decoration".
    for (const Entity& entity : play->world().getEntities(engine::level::kDecorationTag))
    {
        if (entity.hasComponent<Tile>())
        {
            ++untype;
        }
    }
    CHECK(untype == 0U);
}

void testABulletHittingABrickStartsTheExplosionAndDestroysTheBullet()
{
    ActionDriver driver;
    CombatFixture fixture{floorPlayerAndTile(3.0F, 1.0F, "mario_Brick_tile")};
    settle(fixture);

    Entity& brick = fixture.onlyTile(TileType::Brick);
    CHECK(fixture.tileIsSolid(brick));

    fixture.step(driver.pressedNow(Key::Space));
    CHECK(fixture.bulletCount() == 1U);

    fixture.stepUntil(driver.held(), [&fixture]() { return fixture.bulletCount() == 0U; }, 60U);

    CHECK(fixture.bulletCount() == 0U);
    CHECK(fixture.tileAnimation(brick).assetName == "animations_explosion_burst");
    CHECK_FALSE(fixture.tileAnimation(brick).repeat);
    CHECK(fixture.tileState(brick).activated);
    // Solid no more: the collider is the whole of "solid".
    CHECK_FALSE(fixture.tileIsSolid(brick));

    // The tile count is unchanged, so nothing was created and nothing was destroyed.
    CHECK(fixture.tileCount() == 13U);
}

void testThePlayerHittingABrickFromBelowStartsTheExplosion()
{
    ActionDriver driver;
    CombatFixture fixture{floorPlayerAndHighBrick()};
    settle(fixture);

    Entity& brick = fixture.onlyTile(TileType::Brick);

    fixture.step(driver.pressedNow(Key::W));
    fixture.stepUntil(driver.held(), [&fixture, &brick]() { return fixture.tileState(brick).activated; }, 120U);

    CHECK(fixture.tileState(brick).activated);
    CHECK(fixture.tileAnimation(brick).assetName == "animations_explosion_burst");
    CHECK_FALSE(fixture.tileIsSolid(brick));

    // And the player is not left inside it. The physics step resolved the player before the
    // tile system took the collider off, so the boxes are clear of one another.
    const Aabb brickBox{fixture.tileTransform(brick).position, Vec2{kCell, kCell}};
    CHECK_FALSE(fixture.playerBox().overlaps(brickBox));
}

void testLandingOnABrickDoesNotStartTheExplosion()
{
    ActionDriver driver;

    // The same brick, with the player dropped onto its **top** from grid row 5. A landing and
    // a ceiling hit are the same two boxes touching on the same frame; only the direction of
    // arrival separates them, and this is the case that must not fire.
    Level level = floorAndPlayer(kUnderBlockColumnForLanding, 5.0F);
    level.addTile(tileAt(1.0F, 3.0F, "mario_Brick_tile"));
    CombatFixture fixture{level};

    Entity& brick = fixture.onlyTile(TileType::Brick);

    for (int frame = 0; frame < 120; ++frame)
    {
        fixture.step(driver.idle());
    }

    // It landed: grounded, and standing on the brick's top edge rather than below it.
    CHECK(fixture.state().grounded);
    CHECK_NEAR(fixture.playerY() + (kPlayerHeight * 0.5F), kRowThreeCentreY - (kCell * 0.5F), 0.5F);
    CHECK_FALSE(fixture.tileState(brick).activated);
    CHECK(fixture.tileIsSolid(brick));
    CHECK(fixture.tileAnimation(brick).assetName == "mario_Brick_tile");
}

void testWalkingIntoTheSideOfABrickDoesNotStartTheExplosion()
{
    ActionDriver driver;
    CombatFixture fixture{floorPlayerAndTile(3.0F, 1.0F, "mario_Brick_tile")};
    settle(fixture);

    Entity& brick = fixture.onlyTile(TileType::Brick);
    const float brickCentre = fixture.tileTransform(brick).position.x;
    const float brickLeft = brickCentre - (kCell * 0.5F);

    // The player walks into the brick's left face. Same brick, same collision, arrived at
    // horizontally rather than from underneath.
    for (int frame = 0; frame < 60; ++frame)
    {
        fixture.step(driver.pressedNow(Key::D));
    }

    // The player really did reach it, or the group would pass for the wrong reason.
    CHECK(blockedAgainstLeftFace(fixture, brickLeft, brickCentre));
    CHECK_FALSE(fixture.tileState(brick).activated);
    CHECK(fixture.tileIsSolid(brick));
    CHECK(fixture.tileAnimation(brick).assetName == "mario_Brick_tile");
}

void testABrickKeepsItsExplosionRunningUntilTheAnimationEnds()
{
    ActionDriver driver;
    CombatFixture fixture{floorPlayerAndTile(3.0F, 1.0F, "mario_Brick_tile")};
    settle(fixture);

    const EntityId brickId = fixture.onlyTile(TileType::Brick).id();

    // Fire, then watch for the frame the explosion starts.
    fixture.step(driver.pressedNow(Key::Space));

    bool started = false;
    int afterStart = 0;
    for (int frame = 0; frame < 60 && !started; ++frame)
    {
        fixture.step(driver.held());
        if (entityAlive(fixture.world(), brickId) &&
            entityOf(fixture.world(), brickId).getComponent<Animation>().assetName == "animations_explosion_burst")
        {
            started = true;
            afterStart = frame;
        }
    }

    CHECK(started);
    if (!started)
    {
        return;
    }

    // Read the explosion's length from the asset rather than writing it down, so a change to
    // the strip shows up here instead of as a mysterious count. Twelve frames of eight.
    const engine::assets::Animation& explosion = fixture.assets().animation("animations_explosion_burst");
    CHECK(explosion.frameCount() == 12U);
    CHECK(explosion.speed() == 8U);
    const auto totalTicks = static_cast<int>(explosion.frameCount() * explosion.speed());

    // The frame the explosion started already spent one tick, because
    // [engine::systems::TileSystem] and [engine::systems::AnimationSystem] both ran during it.
    int steps = 0;
    std::uint32_t previousFrame = entityOf(fixture.world(), brickId).getComponent<Animation>().currentFrame;
    bool frameWentBackwards = false;

    while (entityAlive(fixture.world(), brickId) && steps < totalTicks + 8)
    {
        fixture.step(driver.held());
        ++steps;

        if (entityAlive(fixture.world(), brickId))
        {
            const std::uint32_t frame = entityOf(fixture.world(), brickId).getComponent<Animation>().currentFrame;
            // A looping animation wraps to zero; a non-repeating one walks to its last frame
            // and stops. Watching for a wrap is what makes "it does not repeat" an
            // observation rather than a flag.
            if (frame < previousFrame)
            {
                frameWentBackwards = true;
            }
            previousFrame = frame;
        }
    }

    CHECK_FALSE(frameWentBackwards);
    CHECK(steps == totalTicks - 1);
    CHECK_FALSE(entityAlive(fixture.world(), brickId));
    CHECK(afterStart >= 0);
}

void testABulletHittingABrickTwiceDoesNotRestartTheExplosion()
{
    ActionDriver driver;
    CombatFixture fixture{floorPlayerAndTile(3.0F, 1.0F, "mario_Brick_tile")};
    settle(fixture);

    const EntityId brickId = fixture.onlyTile(TileType::Brick).id();

    // The first shot breaks it, which takes the collider off.
    fixture.step(driver.pressedNow(Key::Space));
    fixture.stepUntil(driver.held(), [&fixture, brickId]() { return !fixture.tileIsSolid(entityOf(fixture.world(), brickId)); },
                      60U);
    CHECK(entityAlive(fixture.world(), brickId));
    CHECK_FALSE(fixture.tileIsSolid(entityOf(fixture.world(), brickId)));

    // A second shot at the same place, fired after. There is no collider left to hit, so it
    // flies on - and the explosion, which has already started, is not restarted and no second
    // explosion appears.
    fixture.step(driver.release(Key::Space));
    fixture.step(driver.pressedNow(Key::Space));
    CHECK(fixture.bulletCount() == 1U);

    for (int frame = 0; frame < 30; ++frame)
    {
        fixture.step(driver.held());
    }

    CHECK(fixture.bulletCount() == 1U);
    CHECK(entityAlive(fixture.world(), brickId));
    if (entityAlive(fixture.world(), brickId))
    {
        CHECK(entityOf(fixture.world(), brickId).getComponent<Animation>().assetName == "animations_explosion_burst");
    }

    // Exactly one brick ever existed and nothing new was created.
    CHECK(fixture.tileCount() == 13U);
}

void testABrickIsIdentifiedByItsTypeNotByItsArtwork()
{
    ActionDriver driver;

    // A brick drawn with the *ground* tile's artwork, at the bullet's height. A tile system
    // that compared animation names would leave this one alone and no brick would ever break.
    CombatFixture fixture{floorAndPlayer()};
    fixture.addHandBuiltTile(3.0F, 1.0F, "mario_ground_tile", TileType::Brick);
    settle(fixture);

    Entity& oddTile = fixture.onlyTile(TileType::Brick);
    CHECK(fixture.tileAnimation(oddTile).assetName == "mario_ground_tile");

    fixture.step(driver.pressedNow(Key::Space));
    fixture.stepUntil(driver.held(), [&fixture, &oddTile]() { return fixture.tileState(oddTile).activated; }, 60U);

    CHECK(fixture.tileState(oddTile).activated);
    CHECK(fixture.tileAnimation(oddTile).assetName == "animations_explosion_burst");
    CHECK_FALSE(fixture.tileIsSolid(oddTile));
}

void testADestroyedBrickStopsBlockingThePlayer()
{
    ActionDriver driver;
    CombatFixture fixture{floorPlayerAndTile(3.0F, 1.0F, "mario_Brick_tile")};
    settle(fixture);

    Entity& brick = fixture.onlyTile(TileType::Brick);
    const float brickCentreX = fixture.tileTransform(brick).position.x;

    // With the brick intact, walking right stops the player against its left face. Sixty
    // pixels of ground to cover and 200 px/s to cover it with, so eighty frames is generous.
    for (int frame = 0; frame < 80; ++frame)
    {
        fixture.step(driver.pressedNow(Key::D));
    }
    CHECK(blockedAgainstLeftFace(fixture, brickCentreX - (kCell * 0.5F), brickCentreX));

    // Break it. The player is standing against it, so the shot is created inside the brick's
    // box - which is the degenerate case, and the shot must still do its job.
    fixture.step(driver.release(Key::D));
    fixture.step(driver.pressedNow(Key::Space));
    fixture.stepUntil(driver.held(), [&fixture, &brick]() { return !fixture.tileIsSolid(brick); }, 60U);
    CHECK_FALSE(fixture.tileIsSolid(brick));
    CHECK(fixture.bulletCount() == 0U);

    // Now the player walks through where it was.
    for (int frame = 0; frame < 60; ++frame)
    {
        fixture.step(driver.pressedNow(Key::D));
    }

    CHECK(fixture.playerX() > brickCentreX);
}

// ===========================================================================
// D. The question block
// ===========================================================================

void testAQuestionBlockStartsUnused()
{
    CombatFixture fixture{floorAndPlayer()};
    Entity& block = fixture.addHandBuiltTile(1.0F, 3.0F, "mario_question_block", TileType::Question);

    CHECK_FALSE(fixture.tileState(block).activated);
    CHECK(fixture.tileAnimation(block).assetName == "mario_question_block");
    CHECK(fixture.tileAnimation(block).repeat);
    CHECK(fixture.tileIsSolid(block));
    CHECK(fixture.tileCount(TileType::Question2) == 0U);
    CHECK(fixture.coinCount() == 0U);
}

void testTheLoaderGivesTheQuestionBlockItsRealThreeHundredAndSixtyPixelBox()
{
    // The limitation, pinned.
    //
    // The committed question artwork is 360x360 and the course sizes a tile's box from its
    // animation, so the real loader produces a block five and a half cells across. That is
    // why `assets/levels/level1.txt` has no question block, and why the groups beside this one
    // build theirs by hand. If this ever stops being true - because the artwork was
    // re-authored at cell size, or because the loader learned to fit oversized artwork - this
    // group fails and says so, rather than the limitation quietly ceasing to apply.
    Level level;
    level.addTile(tileAt(0.0F, 0.0F, "mario_question_block"));
    level.setPlayer(playerAt(0.0F, 1.0F));

    CombatFixture fixture{level};

    Entity& block = fixture.onlyTile(TileType::Question);
    const Vec2 size = block.getComponent<Collider>().size;

    CHECK_NEAR(size.x, 360.0F, 0.0001F);
    CHECK_NEAR(size.y, 360.0F, 0.0001F);
}

void testThePlayerHittingAQuestionBlockFromBelowUsesIt()
{
    ActionDriver driver;
    CombatFixture fixture{floorAndPlayer(kUnderBlockColumn)};
    Entity& block = fixture.addHandBuiltTile(1.0F, 3.0F, "mario_question_block", TileType::Question);

    const EntityId blockId = block.id();
    const std::size_t tilesBefore = fixture.tileCount();

    // The jump has to be allowed from the ground, and the loader honestly reports a freshly
    // spawned player as airborne, so it is settled first. Every group below that jumps does
    // the same; the first version of this one did not, and it failed for the right reason.
    settle(fixture);

    fixture.step(driver.pressedNow(Key::W));
    fixture.stepUntil(driver.idle(), [&fixture, &block]() { return fixture.tileState(block).activated; }, 120U);

    // The same entity is now the used block: one entity through both states, not a
    // replacement.
    CHECK(entityAlive(fixture.world(), block.id()));
    CHECK(fixture.tileCount(TileType::Question2) == 1U);
    CHECK(fixture.tileCount(TileType::Question) == 0U);
    CHECK(fixture.tileCount() == tilesBefore);

    if (entityAlive(fixture.world(), blockId))
    {
        Entity& used = entityOf(fixture.world(), blockId);
        CHECK(fixture.tileState(used).activated);
        CHECK(fixture.tileAnimation(used).assetName == "mario_question2_block");
        // A used block is ordinary level geometry, so it loops like the ground does. A
        // non-repeating animation here would delete the block out of the level.
        CHECK(fixture.tileAnimation(used).repeat);
    }
}

void testTheUsedBlockKeepsItsPositionAndItsCollider()
{
    ActionDriver driver;
    CombatFixture fixture{floorAndPlayer(kUnderBlockColumn)};
    Entity& block = fixture.addHandBuiltTile(1.0F, 3.0F, "mario_question_block", TileType::Question);

    const EntityId blockId = block.id();
    const Vec2 before = fixture.tileTransform(block).position;
    const Vec2 sizeBefore = block.getComponent<Collider>().size;
    settle(fixture);

    fixture.step(driver.pressedNow(Key::W));
    fixture.stepUntil(driver.idle(), [&fixture]() { return fixture.coinCount() == 1U; }, 120U);

    // The block really was used. Without this the rest of the group - which is all about the
    // block *not* changing - would pass whether or not anything had happened, and it did the
    // first time it was written.
    CHECK(entityAlive(fixture.world(), blockId));
    CHECK(entityOf(fixture.world(), blockId).getComponent<Tile>().type == TileType::Question2);
    CHECK(entityOf(fixture.world(), blockId).getComponent<Tile>().activated);
    if (!entityAlive(fixture.world(), blockId))
    {
        return;
    }

    Entity& used = entityOf(fixture.world(), blockId);

    // The block does not move and does not change size. The used artwork is a different size
    // from the unused artwork - 160x160 against 360x360 - and resizing the collider to match
    // would mean a block that changed shape in the middle of a level the player is standing
    // in.
    CHECK_NEAR(fixture.tileTransform(used).position.x, before.x, 0.0001F);
    CHECK_NEAR(fixture.tileTransform(used).position.y, before.y, 0.0001F);
    CHECK(fixture.tileIsSolid(used));
    CHECK_NEAR(used.getComponent<Collider>().size.x, sizeBefore.x, 0.0001F);
    CHECK_NEAR(used.getComponent<Collider>().size.y, sizeBefore.y, 0.0001F);
}

void testLandingOnAQuestionBlockDoesNotUseIt()
{
    ActionDriver driver;

    CombatFixture fixture{floorAndPlayer(kUnderBlockColumnForLanding, 5.0F)};
    Entity& block = fixture.addHandBuiltTile(1.0F, 3.0F, "mario_question_block", TileType::Question);

    for (int frame = 0; frame < 120; ++frame)
    {
        fixture.step(driver.idle());
    }

    CHECK(fixture.state().grounded);
    CHECK_NEAR(fixture.playerY() + (kPlayerHeight * 0.5F), kRowThreeCentreY - (kCell * 0.5F), 1.0F);
    CHECK_FALSE(fixture.tileState(block).activated);
    CHECK(fixture.coinCount() == 0U);
    CHECK(fixture.tileAnimation(block).assetName == "mario_question_block");
}

void testWalkingIntoTheSideOfAQuestionBlockDoesNotUseIt()
{
    ActionDriver driver;

    CombatFixture fixture{floorAndPlayer()};
    Entity& block = fixture.addHandBuiltTile(3.0F, 1.0F, "mario_question_block", TileType::Question);
    settle(fixture);

    const float blockCentre = fixture.tileTransform(block).position.x;

    for (int frame = 0; frame < 60; ++frame)
    {
        fixture.step(driver.pressedNow(Key::D));
    }

    CHECK(blockedAgainstLeftFace(fixture, blockCentre - (kCell * 0.5F), blockCentre));
    CHECK_FALSE(fixture.tileState(block).activated);
    CHECK(fixture.coinCount() == 0U);
}

void testTheUsedBlockStaysUsed()
{
    ActionDriver driver;
    CombatFixture fixture{floorAndPlayer(kUnderBlockColumn)};
    Entity& block = fixture.addHandBuiltTile(1.0F, 3.0F, "mario_question_block", TileType::Question);

    const EntityId blockId = block.id();
    const std::size_t tilesBefore = fixture.tileCount();
    settle(fixture);

    fixture.step(driver.pressedNow(Key::W));
    fixture.stepUntil(driver.idle(), [&fixture]() { return fixture.coinCount() == 1U; }, 120U);
    CHECK(fixture.coinCount() == 1U);

    // Far longer than a jump lasts, with the player kept underneath the block the whole time
    // so that any repeatable activation would be pressed again and again.
    for (int frame = 0; frame < 180; ++frame)
    {
        fixture.step(driver.idle());
    }

    CHECK(entityAlive(fixture.world(), blockId));
    CHECK(fixture.tileCount(TileType::Question2) == 1U);
    // The one coin came and went; a second activation would have made another.
    CHECK(fixture.coinCount() == 0U);
    CHECK(fixture.tileCount() == tilesBefore);
}

void testABulletDoesNotUseAQuestionBlock()
{
    ActionDriver driver;

    // A grid-sized question block sitting on the buster's path.
    CombatFixture fixture{floorAndPlayer()};
    Entity& block = fixture.addHandBuiltTile(3.0F, 1.0F, "mario_question_block", TileType::Question);
    settle(fixture);

    fixture.step(driver.pressedNow(Key::Space));
    fixture.stepUntil(driver.held(), [&fixture]() { return fixture.bulletCount() == 0U; }, 60U);

    // The bullet stopped - that is the rule for every gameplay tile - and the block is
    // untouched. The course names the player's hit as the trigger, and "any hit works" would
    // be a rule nobody chose.
    CHECK(fixture.bulletCount() == 0U);
    CHECK_FALSE(fixture.tileState(block).activated);
    CHECK(fixture.tileCount(TileType::Question2) == 0U);
    CHECK(fixture.coinCount() == 0U);
    CHECK(fixture.tileAnimation(block).assetName == "mario_question_block");
}

// ===========================================================================
// E. The coin
// ===========================================================================

// A question block above the player, ready to be used.
void testUsingAQuestionBlockCreatesExactlyOneCoin()
{
    ActionDriver driver;
    CombatFixture fixture{floorAndPlayer(kUnderBlockColumn)};
    Entity& block = fixture.addHandBuiltTile(1.0F, 3.0F, "mario_question_block", TileType::Question);

    const Vec2 blockCentre = fixture.tileTransform(block).position;
    settle(fixture);

    fixture.step(driver.pressedNow(Key::W));
    fixture.stepUntil(driver.idle(), [&fixture]() { return fixture.coinCount() == 1U; }, 120U);

    CHECK(fixture.coinCount() == 1U);
    if (fixture.coinCount() != 1U)
    {
        return;
    }

    // Sixty-four pixels above the block, measured between centres - because that is what a
    // [engine::components::Transform]'s position is, and the course places entities by their
    // centre.
    const Vec2 coinPosition = fixture.coinTransform(fixture.onlyCoin()).position;
    CHECK_NEAR(coinPosition.x, blockCentre.x, 0.0001F);
    CHECK_NEAR(coinPosition.y, blockCentre.y - kCourseCoinSpawnHeight, 0.0001F);
}

void testTheCoinIsNotInsideTheQuestionBlock()
{
    ActionDriver driver;
    CombatFixture fixture{floorAndPlayer(kUnderBlockColumn)};
    Entity& block = fixture.addHandBuiltTile(1.0F, 3.0F, "mario_question_block", TileType::Question);

    const Vec2 blockCentre = fixture.tileTransform(block).position;
    const Vec2 blockSize = block.getComponent<Collider>().size;
    settle(fixture);

    fixture.step(driver.pressedNow(Key::W));
    fixture.stepUntil(driver.idle(), [&fixture]() { return fixture.coinCount() == 1U; }, 120U);
    CHECK(fixture.coinCount() == 1U);
    if (fixture.coinCount() != 1U)
    {
        return;
    }

    // Drawn with its longest side one grid cell - see [engine::systems::TileSystem] on why the
    // coin is scaled - and read from the real asset so the number is derived. The artwork is
    // 500x299, so fitting the *longest* side keeps its aspect and makes the coin 64 wide and
    // about 38 tall rather than a stretched square.
    const Vec2 scale = fixture.coinTransform(fixture.onlyCoin()).scale;
    const engine::assets::Animation& coinArt = fixture.assets().animation("mario_coin_pickup");
    const float drawnWidth = static_cast<float>(coinArt.frameWidth()) * scale.x;
    const float drawnHeight = static_cast<float>(coinArt.frameHeight()) * scale.y;

    CHECK_NEAR(std::max(drawnWidth, drawnHeight), kCell, 0.01F);
    // And the aspect ratio is kept rather than each axis fitted separately: the 500-pixel
    // side is the cell, so the 299-pixel side is not.
    CHECK(drawnHeight < drawnWidth);

    const Vec2 coinPosition = fixture.coinTransform(fixture.onlyCoin()).position;
    const Aabb coinBox{coinPosition, Vec2{drawnWidth, drawnHeight}};
    const Aabb blockBox{blockCentre, blockSize};

    // The direct statement of "not inside": the two boxes do not overlap at all.
    CHECK_FALSE(coinBox.overlaps(blockBox));

    // And it is *close* to the block rather than hovering above it. The course's sixty-four
    // pixels is a distance between centres; the gap this leaves is under half the coin's own
    // height, which is what "it came out of the top of the block" looks like. It is not zero,
    // and pretending it was would mean pretending the artwork is square.
    const float coinBottom = coinPosition.y + (drawnHeight * 0.5F);
    const float blockTop = blockCentre.y - (blockSize.y * 0.5F);
    CHECK(coinBottom <= blockTop + 0.01F);
    CHECK(blockTop - coinBottom < (drawnHeight * 0.5F));
}

void testTheCoinSurvivesItsWholeLifeAndThenGoes()
{
    ActionDriver driver;
    CombatFixture fixture{floorAndPlayer(kUnderBlockColumn)};
    static_cast<void>(fixture.addHandBuiltTile(1.0F, 3.0F, "mario_question_block", TileType::Question));
    settle(fixture);

    fixture.step(driver.pressedNow(Key::W));
    fixture.stepUntil(driver.idle(), [&fixture]() { return fixture.coinCount() == 1U; }, 120U);
    CHECK(fixture.coinCount() == 1U);
    if (fixture.coinCount() != 1U)
    {
        return;
    }

    // The frame it was created on is the first of its thirty, and the countdown runs before
    // the coin is created - so after twenty-nine more frames it is on its last one.
    for (std::uint32_t frame = 0; frame + 1U < kCourseCoinLifetimeFrames; ++frame)
    {
        fixture.step(driver.idle());
    }
    CHECK(fixture.coinCount() == 1U);

    // And the thirtieth removes it.
    fixture.step(driver.idle());
    CHECK(fixture.coinCount() == 0U);
}

void testTheCoinsLifetimeIsCountedInFramesNotSeconds()
{
    // Thirty frames at a fifth of the frame rate is six tenths of a second, and thirty frames
    // at sixty is half of one, so a wall-clock lifetime long enough to look right at sixty
    // would have removed this one early - and one short would have removed it early there.
    ActionDriver driver;
    CombatFixture fixture{floorAndPlayer(kUnderBlockColumn)};
    static_cast<void>(fixture.addHandBuiltTile(1.0F, 3.0F, "mario_question_block", TileType::Question));
    settle(fixture);

    fixture.step(driver.pressedNow(Key::W));
    fixture.stepUntil(driver.idle(), [&fixture]() { return fixture.coinCount() == 1U; }, 120U, 1.0F / 20.0F);
    CHECK(fixture.coinCount() == 1U);
    if (fixture.coinCount() != 1U)
    {
        return;
    }

    for (std::uint32_t frame = 0; frame + 1U < kCourseCoinLifetimeFrames; ++frame)
    {
        fixture.step(driver.idle(), 1.0F / 20.0F);
    }
    CHECK(fixture.coinCount() == 1U);

    fixture.step(driver.idle(), 1.0F / 20.0F);
    CHECK(fixture.coinCount() == 0U);
}

void testTheCoinIsNotCollidable()
{
    ActionDriver driver;
    CombatFixture fixture{floorAndPlayer(kUnderBlockColumn)};
    static_cast<void>(fixture.addHandBuiltTile(1.0F, 3.0F, "mario_question_block", TileType::Question));
    settle(fixture);

    fixture.step(driver.pressedNow(Key::W));
    fixture.stepUntil(driver.idle(), [&fixture]() { return fixture.coinCount() == 1U; }, 120U);
    CHECK(fixture.coinCount() == 1U);
    if (fixture.coinCount() != 1U)
    {
        return;
    }

    Entity& coin = fixture.onlyCoin();

    // The course says a coin appears when a block is hit, and nothing about what it then
    // does. So it has no box, it is not in the collision pass, and it cannot be collected,
    // scored or landed on by anything that does not already exist.
    CHECK_FALSE(coin.hasComponent<Collider>());
    CHECK_FALSE(coin.hasComponent<Body>());
    CHECK(coin.hasComponent<Lifetime>());
    CHECK(coin.hasComponent<Animation>());
    CHECK(coin.getComponent<Animation>().assetName == "mario_coin_pickup");
    CHECK(coin.getComponent<Animation>().repeat);
}

void testACoinIsNotDestroyedByTheAnimationSystem()
{
    ActionDriver driver;
    CombatFixture fixture{floorAndPlayer(kUnderBlockColumn)};
    static_cast<void>(fixture.addHandBuiltTile(1.0F, 3.0F, "mario_question_block", TileType::Question));
    settle(fixture);

    fixture.step(driver.pressedNow(Key::W));
    fixture.stepUntil(driver.idle(), [&fixture]() { return fixture.coinCount() == 1U; }, 120U);
    CHECK(fixture.coinCount() == 1U);

    // Five more frames, five times over - twenty frames, still inside the coin's thirty. A
    // one-frame animation that did not repeat would have been `ended` after one tick and the
    // coin would be gone by now; so would a non-repeating animation once its single frame had
    // been held for its period.
    for (int round = 0; round < 5; ++round)
    {
        for (int frame = 0; frame < 4; ++frame)
        {
            fixture.step(driver.idle());
        }
        CHECK(fixture.coinCount() == 1U);
    }
}

// ===========================================================================
// F. Entity lifetime, in bulk
// ===========================================================================

void testSeveralBulletsDyingOnTheSameFrameIsSafe()
{
    ActionDriver driver;
    CombatFixture fixture{floorPlayerAndTile(6.0F, 1.0F, "mario_ground_tile")};
    settle(fixture);

    // Three shots, one per frame. They are 20 pixels apart and travel the same distance each
    // frame, so all three reach the same wall on the same frame.
    for (int shot = 0; shot < 3; ++shot)
    {
        fixture.step(driver.pressedNow(Key::Space));
    }
    CHECK(fixture.bulletCount() == 3U);

    int frames = 0;
    while (fixture.bulletCount() != 0U && frames < 60)
    {
        fixture.step(driver.held());
        ++frames;
    }

    CHECK(fixture.bulletCount() == 0U);

    // And the world is still sound: the wall is standing, the player is standing, and the
    // systems can be run again without anything having been invalidated.
    CHECK(fixture.state().state == PlayerState::Stand);
    CHECK(fixture.tileCount(TileType::Solid) == 13U);
    for (const Entity& entity : fixture.world().getEntities(engine::level::kTileTag))
    {
        CHECK(entity.hasComponent<Collider>());
    }
    fixture.step(driver.idle());
    CHECK(fixture.world().aliveEntityCount() > 0U);
}

void testACoinExpiringWhileTheWorldMovesIsSafe()
{
    ActionDriver driver;
    CombatFixture fixture{floorAndPlayer(kUnderBlockColumn)};
    Entity& block = fixture.addHandBuiltTile(1.0F, 3.0F, "mario_question_block", TileType::Question);
    settle(fixture);

    fixture.step(driver.pressedNow(Key::W));
    fixture.stepUntil(driver.idle(), [&fixture]() { return fixture.coinCount() == 1U; }, 120U);
    CHECK(fixture.coinCount() == 1U);

    // The lifetime system runs before the coin is created, so the countdown and the physics
    // step that reads the world are both happening around it for thirty frames. Nothing may be
    // invalidated while the owner is iterating.
    for (int frame = 0; frame < 40; ++frame)
    {
        fixture.step(driver.pressedNow(Key::D));
    }

    CHECK(fixture.coinCount() == 0U);
    CHECK(fixture.tileIsSolid(block));
    CHECK(fixture.tileAnimation(block).assetName == "mario_question2_block");

    // The player is still standing in the world it started in.
    CHECK(fixture.playerX() > 0.0F);
}

void testABulletAndAnExplosionAndACoinInTheSameWorldAreAllSafe()
{
    ActionDriver driver;

    Level level = floorAndPlayer(kUnderBlockColumn);
    level.addTile(tileAt(3.0F, 1.0F, "mario_Brick_tile"));

    CombatFixture fixture{level};
    static_cast<void>(fixture.addHandBuiltTile(1.0F, 3.0F, "mario_question_block", TileType::Question));

    // Fire and jump in the same frame, then let everything run at once: a bullet breaks the
    // brick at the bullet's height, the jump uses the block above the player, a coin appears
    // and expires, and the explosion finishes. Every one of those destroys entities from
    // inside a walk.
    fixture.step(driver.pressedNow(Key::Space));
    for (int frame = 0; frame < 300; ++frame)
    {
        fixture.step((frame == 1) ? driver.pressedNow(Key::W) : driver.held());
    }

    // Nothing on a clock survived: no brick, no coin.
    CHECK(fixture.tileCount(TileType::Brick) == 0U);
    CHECK(fixture.coinCount() == 0U);
    CHECK(fixture.tileCount(TileType::Question2) == 1U);
    CHECK(fixture.tileIsSolid(fixture.onlyTile(TileType::Question2)));

    // The floor is untouched, and so is the player.
    CHECK(fixture.tileCount(TileType::Solid) == 12U);
    CHECK(fixture.state().grounded);
}

// ===========================================================================
// G. The committed level, end to end
// ===========================================================================

void testFiringFromTheCommittedLevelBreaksABrick()
{
    // The shipped level file, through the real loader over the real asset table, with the
    // production systems. The committed level puts a brick at cell (8, 1) - at the player's
    // own standing height - so a shot from the spawn point has somewhere to go.
    ActionDriver driver;

    CombatFixture fixture{engine::level::loadLevelFile(ENGINE_COMMITTED_LEVEL_FILE)};
    settle(fixture, 60U);

    // The player really is standing on the floor, with a brick a few hundred pixels to its
    // right at the height the bullet flies.
    CHECK(fixture.state().grounded);
    CHECK_NEAR(fixture.playerY() + (kPlayerHeight * 0.5F), kFloorTop, 0.5F);

    fixture.step(driver.pressedNow(Key::Space));
    CHECK(fixture.bulletCount() == 1U);
    CHECK_NEAR(fixture.bulletTransform(fixture.onlyBullet()).velocity.x, kCommittedBulletSpeed, 0.0001F);

    bool exploding = false;
    for (int frame = 0; frame < 200 && !exploding; ++frame)
    {
        fixture.step(driver.held());
        for (const Entity& entity : fixture.world().getEntities())
        {
            if (entity.hasComponent<Animation>() &&
                entity.getComponent<Animation>().assetName == "animations_explosion_burst")
            {
                exploding = true;
                break;
            }
        }
    }

    CHECK(exploding);
    CHECK(fixture.bulletCount() == 0U);
}

void testThePlaySceneRegistersTheSystemsThisPhaseNeeds()
{
    engine::Application application;
    application.changeScene(engine::scene::SceneId::Play);
    application.update();

    const auto* play = dynamic_cast<const engine::scene::PlayScene*>(application.currentScene());
    CHECK(play != nullptr);
    if (play == nullptr)
    {
        return;
    }

    // Read off a real scene rather than scanning a file, so a system that is merely absent is
    // a failure rather than a silent omission. `scene.lifecycle` pins the whole order by
    // index; this group states the three this phase added, because a reader of this file
    // should not have to know which suite owns the ordering.
    bool shoot = false;
    bool lifetime = false;
    bool tiles = false;
    for (std::size_t index = 0; index < play->systems().systemCount(); ++index)
    {
        const std::string name{play->systems().systemAt(index).name()};
        shoot = shoot || (name == "ShootSystem");
        lifetime = lifetime || (name == "LifetimeSystem");
        tiles = tiles || (name == "TileSystem");
    }

    CHECK(shoot);
    CHECK(lifetime);
    CHECK(tiles);
}

// ===========================================================================
// The id helpers
// ===========================================================================

[[nodiscard]] bool entityAlive(const EntityManager& world, const EntityId id)
{
    for (const Entity& entity : world.getEntities())
    {
        if (entity.id() == id)
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] Entity& entityOf(const EntityManager& world, const EntityId id)
{
    for (const Entity& entity : world.getEntities())
    {
        if (entity.id() == id)
        {
            return const_cast<Entity&>(entity);
        }
    }
    throw std::logic_error{"no entity with id " + std::to_string(id)};
}

} // namespace

int main()
{
    using TestCase = void (*)();
    const std::pair<const char*, TestCase> testCases[] = {
        // Shooting
        {"Space creates exactly one bullet", &testSpaceCreatesExactlyOneBullet},
        {"holding Space does not create a bullet every frame", &testHoldingSpaceDoesNotCreateABulletEveryFrame},
        {"no key at all creates no bullet", &testNoKeyAtAllCreatesNoBullet},
        {"shooting right puts the bullet to the right and it goes right",
         &testShootingRightPutsTheBulletToTheRightAndItGoesRight},
        {"shooting left puts the bullet to the left and it goes left",
         &testShootingLeftPutsTheBulletToTheLeftAndItGoesLeft},
        {"a bullet keeps the direction it was fired in", &testABulletKeepsTheDirectionItWasFiredIn},
        {"the bullet speed is the level's speed times six", &testTheBulletSpeedIsTheLevelsSpeedTimesSix},
        {"the bullet moves in the frame it is fired", &testTheBulletMovesInTheFrameItIsFired},
        {"the bullet box is half its animation frame", &testTheBulletBoxIsHalfItsAnimationFrame},
        {"the bullet is drawn with the level's bullet animation and never ends",
         &testTheBulletIsDrawnWithTheLevelsBulletAnimationAndNeverEnds},
        // Bullet collision
        {"a bullet that hits a solid tile is destroyed", &testABulletThatHitsASolidTileIsDestroyed},
        {"the bullet that died is one physics reported", &testTheBulletThatDiedIsOnePhysicsReported},
        {"a bullet cannot pass through a solid tile", &testABulletCannotPassThroughASolidTile},
        {"a decoration is neither collidable nor a bullet target",
         &testADecorationIsNeitherCollidableNorABulletTarget},
        {"a bullet that misses everything is still alive after many frames",
         &testABulletThatMissesEverythingIsStillAliveAfterManyFrames},
        {"a bullet expires after its frames and not before", &testABulletExpiresAfterItsFramesAndNotBefore},
        {"a bullet lifetime is counted in frames not seconds",
         &testABulletLifetimeIsCountedInFramesNotSeconds},
        // Bricks
        {"a brick starts whole and solid", &testABrickStartsWholeAndSolid},
        {"the committed level classifies its bricks", &testTheCommittedLevelClassifiesItsBricks},
        {"a bullet hitting a brick starts the explosion and destroys the bullet",
         &testABulletHittingABrickStartsTheExplosionAndDestroysTheBullet},
        {"the player hitting a brick from below starts the explosion",
         &testThePlayerHittingABrickFromBelowStartsTheExplosion},
        {"landing on a brick does not start the explosion", &testLandingOnABrickDoesNotStartTheExplosion},
        {"walking into the side of a brick does not start the explosion",
         &testWalkingIntoTheSideOfABrickDoesNotStartTheExplosion},
        {"a brick keeps its explosion running until the animation ends",
         &testABrickKeepsItsExplosionRunningUntilTheAnimationEnds},
        {"a bullet hitting a brick twice does not restart the explosion",
         &testABulletHittingABrickTwiceDoesNotRestartTheExplosion},
        {"a brick is identified by its type not by its artwork",
         &testABrickIsIdentifiedByItsTypeNotByItsArtwork},
        {"a destroyed brick stops blocking the player", &testADestroyedBrickStopsBlockingThePlayer},
        // The question block
        {"a question block starts unused", &testAQuestionBlockStartsUnused},
        {"the loader gives the question block its real 360 pixel box",
         &testTheLoaderGivesTheQuestionBlockItsRealThreeHundredAndSixtyPixelBox},
        {"the player hitting a question block from below uses it",
         &testThePlayerHittingAQuestionBlockFromBelowUsesIt},
        {"the used block keeps its position and its collider", &testTheUsedBlockKeepsItsPositionAndItsCollider},
        {"landing on a question block does not use it", &testLandingOnAQuestionBlockDoesNotUseIt},
        {"walking into the side of a question block does not use it",
         &testWalkingIntoTheSideOfAQuestionBlockDoesNotUseIt},
        {"the used block stays used", &testTheUsedBlockStaysUsed},
        {"a bullet does not use a question block", &testABulletDoesNotUseAQuestionBlock},
        // The coin
        {"using a question block creates exactly one coin",
         &testUsingAQuestionBlockCreatesExactlyOneCoin},
        {"the coin is not inside the question block", &testTheCoinIsNotInsideTheQuestionBlock},
        {"the coin survives its whole life and then goes", &testTheCoinSurvivesItsWholeLifeAndThenGoes},
        {"the coin's lifetime is counted in frames not seconds",
         &testTheCoinsLifetimeIsCountedInFramesNotSeconds},
        {"the coin is not collidable", &testTheCoinIsNotCollidable},
        {"a coin is not destroyed by the animation system", &testACoinIsNotDestroyedByTheAnimationSystem},
        // Entity lifetime, in bulk
        {"several bullets dying on the same frame is safe", &testSeveralBulletsDyingOnTheSameFrameIsSafe},
        {"a coin expiring while the world moves is safe", &testACoinExpiringWhileTheWorldMovesIsSafe},
        {"a bullet and an explosion and a coin in the same world are all safe",
         &testABulletAndAnExplosionAndACoinInTheSameWorldAreAllSafe},
        // The committed level, end to end
        {"firing from the committed level breaks a brick", &testFiringFromTheCommittedLevelBreaksABrick},
        {"the play scene registers the systems this phase needs",
         &testThePlaySceneRegistersTheSystemsThisPhaseNeeds},
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

    std::cout << groupCount << " combat and tile behaviour test groups passed\n";
    return EXIT_SUCCESS;
}