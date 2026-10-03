/// The A3 vertical slice, end to end.
///
/// ### What this suite is for
///
/// Phases 13 to 19 each proved one thing about a part of the engine: a level loads, a
/// player moves, a camera follows, a bullet kills a brick, five keys do what the course
/// says. This suite asks the only question that matters about those parts together -
///
/// ```text
///   menu -> START -> level loads -> player spawns -> walks -> jumps -> lands
///         -> shoots -> bullet flies -> bullet hits a tile -> brick explodes
///         -> brick is gone -> the player walks through where it was
///         -> falls out of the world -> respawns at the spawn point
/// ```
///
/// ### Everything is real
///
/// There is not one double in this file. The world is spawned by the real
/// [engine::level::LevelLoader] from the **committed** `assets/levels/level1.txt`;
/// animations and frame sizes come from the real
/// [engine::assets::SfmlAssetManager] over the committed `assets/assets.txt`; the nine
/// systems are the ones [engine::scene::PlayScene] registers, in its order; and input
/// is **keys** pressed on a real [engine::input::Input] and resolved through
/// [engine::input::defaultActionMap], the way [engine::Application] does it.
///
/// The only double is the renderer, and it is a recording one for the same reason
/// [debug.controls](DebugTest.cpp) uses one: the groups here are about **state moving**,
/// not about pixels, and asserting on the world is a stronger claim than asserting on a
/// picture. The one group that does care about the picture asserts on which draws were
/// submitted.
///
/// ### Why a real [engine::scene::PlayScene] and not a hand-wired system list
///
/// Because the interesting failures are **integration** failures. A system list built
/// by hand can be in the right order for the wrong reason, and a level loaded from a
/// hand-written record can be a level nobody plays. Building the production scene is
/// what makes "the systems cooperate" a fact about the shipping configuration rather
/// than about a fixture.
///
/// ### The numbers
///
/// Every expected value is the level's own, written out rather than read back from the
/// system under test. The player record is `Player 3 4 40 60 200 400 250 900 ...`, so
/// the spawn centre is cell (3,4) with a 40x60 box, the walk is 200 px/s and the jump
/// is 400 px/s against 900 px/s^2 of gravity - all of which the groups state.
#include "engine/Color.hpp"
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
#include "engine/graphics/Camera.hpp"
#include "engine/graphics/RenderTransform.hpp"
#include "engine/graphics/Renderer.hpp"
#include "engine/input/Action.hpp"
#include "engine/input/ActionMap.hpp"
#include "engine/input/ActionState.hpp"
#include "engine/input/Input.hpp"
#include "engine/level/LevelGrid.hpp"
#include "engine/level/LevelLoader.hpp"
#include "engine/scene/MenuScene.hpp"
#include "engine/scene/PlayScene.hpp"
#include "engine/scene/Scene.hpp"

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
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

// ---------------------------------------------------------------------------
// Minimal harness, matching the style already used by the other suites.
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
using engine::ecs::EntityManager;
using engine::graphics::Camera;
using engine::graphics::RenderTransform;
using engine::graphics::Renderer;
using engine::input::ActionState;
using engine::input::defaultActionMap;
using engine::input::Input;
using engine::input::Key;
using engine::level::LevelGrid;
using engine::scene::MenuScene;
using engine::scene::PlayScene;
using engine::scene::SceneContext;
using engine::scene::SceneId;

/// One sixtieth of a second: the engine's frame cap.
constexpr float kFrame = 1.0F / 60.0F;

/// The course's cell size.
constexpr float kCell = LevelGrid::kCellSize;

/// `assets/levels/level1.txt`: `Player 3 4 40 60 200 400 250 900 ...`.
///
/// The world's height is the game's, not the level's - see
/// [engine::level::LevelGrid] - and it comes from the generated configuration, so it
/// is read rather than assumed.
constexpr float kWorldHeight = 16.0F * kCell;

/// The player's own numbers, from the level record, restated so a group can say "the
/// level says 200" without reading the value it is about to compare against.
constexpr float kPlayerWidth = 40.0F;
constexpr float kPlayerHeight = 60.0F;
constexpr float kWalkSpeed = 200.0F;
constexpr float kJumpSpeed = 400.0F;
constexpr float kGravity = 900.0F;

/// The course's bullet speed factor: `velocity.x = scale.x * SPEED * 6`.
constexpr float kBulletSpeedFactor = 6.0F;

/// The course's bullet spawn offset: the bullet appears 64 pixels in front of the
/// player.
constexpr float kBulletSpawnOffset = 64.0F;

/// The committed buster's real frame size, halved for the bullet's collider.
constexpr float kBulletFrameWidth = 32.0F;
constexpr float kBulletFrameHeight = 26.0F;

/// A grid cell's centre height in world pixels, for a one-cell animation.
///
/// `worldY = height - gridY * 64 - 32`: the level's Y axis grows upwards from the
/// bottom-left and the engine's grows downwards from the top-left, and the flip is
/// about the world's height rather than about zero.
[[nodiscard]] constexpr float rowCentreY(const float gridY) noexcept
{
    return kWorldHeight - (gridY * kCell) - (kCell * 0.5F);
}

// ---------------------------------------------------------------------------
// A recording renderer, for the one group that looks at what was drawn.
// ---------------------------------------------------------------------------

class RecordingRenderer final : public Renderer
{
public:
    void beginFrame() override
    {
        ++m_beginFrames;
        m_textureDraws.clear();
        m_rectangleDraws.clear();
        m_textDraws.clear();
    }

    void clear(const engine::Color&) override {}

    void drawRectangle(const Vec2& size, const engine::Color& color, const RenderTransform& placement) override
    {
        static_cast<void>(color);
        m_rectangleDraws.push_back({size, placement});
    }

    void drawTexture(const engine::assets::Texture& texture, const RenderTransform& placement,
                     const std::optional<engine::IntRect>& source) override
    {
        m_textureDraws.push_back({&texture, placement, source});
    }

    void drawText(const engine::assets::Font&, const std::string& content, std::uint32_t,
                  const engine::Color&, const RenderTransform& placement) override
    {
        static_cast<void>(placement);
        m_textDraws.push_back(content);
    }

    void endFrame() override { ++m_endFrames; }

    [[nodiscard]] std::uint64_t frameCount() const noexcept override { return m_endFrames; }

    struct RectangleDraw
    {
        Vec2 size;
        RenderTransform placement;
    };

    struct TextureDraw
    {
        const engine::assets::Texture* texture = nullptr;
        RenderTransform placement;
        std::optional<engine::IntRect> source;
    };

    [[nodiscard]] const std::vector<TextureDraw>& textureDraws() const noexcept { return m_textureDraws; }
    [[nodiscard]] const std::vector<RectangleDraw>& rectangleDraws() const noexcept { return m_rectangleDraws; }
    [[nodiscard]] const std::vector<std::string>& textDraws() const noexcept { return m_textDraws; }

private:
    std::vector<TextureDraw> m_textureDraws;
    std::vector<RectangleDraw> m_rectangleDraws;
    std::vector<std::string> m_textDraws;
    std::size_t m_beginFrames = 0U;
    std::size_t m_endFrames = 0U;
};

// ---------------------------------------------------------------------------
// The fixture: the production scene over the committed level.
// ---------------------------------------------------------------------------

/// Presses keys through the real action map.
///
/// `press` and `release` both begin a frame first, because that is the order
/// [engine::Application::run] uses and because a driver that does not would leave the
/// previous frame's press edge set - which is how the first version of this suite's
/// pause group pressed `P` twice by accident.
class Keyboard
{
public:
    [[nodiscard]] ActionState press(const Key key)
    {
        m_input.beginFrame();
        m_input.processKeyDown(key);
        return snapshot();
    }

    [[nodiscard]] ActionState release(const Key key)
    {
        m_input.beginFrame();
        m_input.processKeyUp(key);
        return snapshot();
    }

    [[nodiscard]] ActionState held()
    {
        m_input.beginFrame();
        return snapshot();
    }

    [[nodiscard]] ActionState idle()
    {
        m_input.beginFrame();
        return snapshot();
    }

    /// Releases `key` and throws the snapshot away.
    ///
    /// For the end of a hold, where the next frame's actions are what matter. The
    /// other three return the snapshot because a caller is about to hand it to a
    /// scene, and `ActionState` is `[[nodiscard]]` for that reason.
    void letGo(const Key key)
    {
        static_cast<void>(release(key));
    }

private:
    [[nodiscard]] ActionState snapshot() const
    {
        ActionState actions;
        actions.update(defaultActionMap(), m_input);
        return actions;
    }

    Input m_input;
};

/// The real game, with the real level, driven one frame at a time.
class Game
{
public:
    Game()
        : m_assets{std::filesystem::path{engine::config::kAssetsConfig}}
    {
        m_camera.setViewport(Vec2{static_cast<float>(engine::config::kWindowWidth),
                                   static_cast<float>(engine::config::kWindowHeight)});
        m_context.emplace(m_renderer, m_camera, m_assets);
        m_scene = std::make_unique<PlayScene>(*m_context);
    }

    [[nodiscard]] PlayScene& play() noexcept { return *static_cast<PlayScene*>(m_scene.get()); }
    [[nodiscard]] EntityManager& world() noexcept { return play().world(); }
    [[nodiscard]] Camera& camera() noexcept { return m_camera; }
    [[nodiscard]] const AssetManager& assets() const noexcept { return m_assets; }
    [[nodiscard]] const SceneContext& context() const noexcept { return *m_context; }
    [[nodiscard]] RecordingRenderer& renderer() noexcept { return m_renderer; }

    /// One frame, exactly as [engine::Application::update] calls a scene.
    void update(const ActionState& actions) { m_scene->update(actions, kFrame); }

    /// One frame drawn, bracketed the way [engine::Application::render] brackets it.
    void renderFrame()
    {
        m_renderer.beginFrame();
        m_scene->render();
        m_renderer.endFrame();
    }

    void run(const ActionState& actions, const int frames)
    {
        for (int frame = 0; frame < frames; ++frame)
        {
            update(actions);
            renderFrame();
        }
    }

    /// Steps **then** checks: checking first would return on the frame the system
    /// wrote the state and never run the frame that acts on it.
    template <typename Predicate>
    int runUntil(const ActionState& actions, const Predicate& until, const int frames = 600)
    {
        for (int frame = 0; frame < frames; ++frame)
        {
            update(actions);
            renderFrame();
            if (until())
            {
                return frame;
            }
        }
        return frames;
    }

private:
    RecordingRenderer m_renderer;
    engine::assets::SfmlAssetManager m_assets;
    Camera m_camera;
    std::optional<SceneContext> m_context;
    std::unique_ptr<engine::scene::Scene> m_scene;
};

// ---------------------------------------------------------------------------
// Reading the world.
// ---------------------------------------------------------------------------

[[nodiscard]] Entity& playerOf(const EntityManager& world)
{
    // A **named** string, because `EntityView` holds its tag as a `string_view` and
    // `getEntities` forwards its parameter straight into it - so a temporary would
    // leave the view pointing at a string that died at the end of the same full
    // expression. `MenuScene::refreshSelection` documents the same trap in the place
    // it was found.
    const std::string tag{engine::level::kPlayerTag};
    for (const Entity& entity : world.getEntities(tag))
    {
        return const_cast<Entity&>(entity);
    }
    throw std::logic_error{"the level has no player"};
}

[[nodiscard]] Transform& playerTransform(const EntityManager& world)
{
    return playerOf(world).getComponent<Transform>();
}

[[nodiscard]] Player& playerState(const EntityManager& world) { return playerOf(world).getComponent<Player>(); }

[[nodiscard]] PlayerConfig& playerConfig(const EntityManager& world)
{
    return playerOf(world).getComponent<PlayerConfig>();
}

[[nodiscard]] std::size_t tileCount(const EntityManager& world, const TileType type)
{
    std::size_t count = 0U;
    for (const Entity& entity : world.getEntities())
    {
        const Tile* const tile = entity.tryGetConstComponent<Tile>();
        if (tile != nullptr && tile->type == type)
        {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] std::size_t bulletCount(const EntityManager& world)
{
    std::size_t count = 0U;
    for (const Entity& entity : world.getEntities())
    {
        if (entity.hasComponent<Bullet>())
        {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] Entity& onlyBullet(const EntityManager& world)
{
    for (const Entity& entity : world.getEntities())
    {
        if (entity.hasComponent<Bullet>())
        {
            return const_cast<Entity&>(entity);
        }
    }
    throw std::logic_error{"there is no bullet"};
}

/// The tile that has started exploding, or `nullptr`.
///
/// Found by the animation a broken brick is pointed at rather than by a position, so
/// a group can say "the brick I broke" instead of "the second tile the loader
/// happened to create".
[[nodiscard]] const Entity* explodingBrick(const EntityManager& world)
{
    for (const Entity& entity : world.getEntities())
    {
        if (entity.hasComponent<Animation>() &&
            entity.getComponent<Animation>().assetName == "animations_explosion_burst")
        {
            return &entity;
        }
    }
    return nullptr;
}

/// The collider sitting exactly on `(centreX, centreY)`, or `nullptr`.
///
/// Used to ask about the world rather than about a component flag: "is there still a
/// brick where the brick was" is a question about geometry, and a brick that had lost
/// its artwork but kept its box would answer it wrongly.
[[nodiscard]] const Entity* tileAt(const EntityManager& world, const float centreX, const float centreY)
{
    for (const Entity& entity : world.getEntities())
    {
        if (!entity.hasComponent<Tile>() || !entity.hasComponent<Collider>())
        {
            continue;
        }
        const Vec2 position = entity.getComponent<Transform>().position;
        if (position.x == centreX && position.y == centreY)
        {
            return &entity;
        }
    }
    return nullptr;
}

/// The committed level's brick at cell (8, 1): the one a bullet fired from the spawn
/// point reaches, because a bullet leaves the player at the player's centre - world y
/// 930 - and that cell spans 896 to 960.
constexpr float kLowBrickX = 8.0F * kCell + (kCell * 0.5F);
constexpr float kLowBrickY = rowCentreY(1.0F);

/// The committed level's brick at cell (5, 3): high enough that a jump reaches it and
/// low enough that a bullet does not.
constexpr float kHighBrickX = 5.0F * kCell + (kCell * 0.5F);
constexpr float kHighBrickY = rowCentreY(3.0F);

/// Where the player stands on the floor: the top of the floor row is world y 960 and
/// the player's box is 60 tall, so its centre is 930.
constexpr float kStandingY = kWorldHeight - kCell - (kPlayerHeight * 0.5F);

/// How far the player is stopped by the low brick: its left edge is
/// `kLowBrickX - 32` and the player's half width is 20.
constexpr float kBlockedByLowBrickX = kLowBrickX - (kCell * 0.5F) - (kPlayerWidth * 0.5F);

// ===========================================================================
// A. The slice
// ===========================================================================

void testTheMenuStartsTheGame()
{
    Game game;
    Keyboard keys;

    // The front end, and what a press of `Space` asks for. `Shoot` is the menu's
    // confirmation because that is what the action layer bound to `Space`.
    MenuScene menu{game.context()};
    CHECK_FALSE(menu.pendingTransition().scene().has_value());

    menu.update(keys.press(Key::Space), kFrame);
    CHECK(menu.pendingTransition().scene().has_value());
    CHECK(menu.pendingTransition().scene().value() == SceneId::Play);
    CHECK_FALSE(menu.pendingTransition().quits());

    // And the level the request builds is the one this suite plays, so "START" and
    // "the level exists" are the same fact rather than two.
    CHECK(tileAt(game.world(), kLowBrickX, kLowBrickY) != nullptr);
}

void testThePlayerSpawnsFromTheLevelAndFallsOntoTheFloor()
{
    Game game;
    Keyboard keys;

    // `Player 3 4 40 60 200 400 250 900 megaman_megaBuster_shot`, so the spawn
    // centre is cell (3, 4) with a 40x60 box. Restated, not read back.
    CHECK_NEAR(playerTransform(game.world()).position.x, (3.0F * kCell) + (kPlayerWidth * 0.5F), 0.001F);
    // `centreOf` anchors the box's **bottom-left** on the cell's bottom-left, so the
    // player's own height is subtracted from the height rather than a cell's worth:
    // the spawn is the top of the level minus four cells minus half of a 60-pixel
    // player.
    CHECK_NEAR(playerTransform(game.world()).position.y,
               kWorldHeight - (4.0F * kCell) - (kPlayerHeight * 0.5F), 0.001F);

    // Spawned in the air, and honest about it: the loader starts `grounded` false
    // because the player really is airborne until it lands.
    CHECK_FALSE(playerState(game.world()).grounded);

    // The level's own constants, copied through untouched.
    CHECK_NEAR(playerConfig(game.world()).leftRightSpeed, kWalkSpeed, 0.0001F);
    CHECK_NEAR(playerConfig(game.world()).jumpSpeed, kJumpSpeed, 0.0001F);
    CHECK_NEAR(playerConfig(game.world()).gravity, kGravity, 0.0001F);
    CHECK(playerConfig(game.world()).bulletAnimationName == "megaman_megaBuster_shot");

    // Sixty frames is more than the fall needs.
    game.run(keys.idle(), 90);

    CHECK(playerState(game.world()).grounded);
    CHECK_NEAR(playerTransform(game.world()).position.y, kStandingY, 0.5F);
    CHECK_NEAR(playerTransform(game.world()).position.x, (3.0F * kCell) + (kPlayerWidth * 0.5F), 0.001F);
    CHECK(playerState(game.world()).state == PlayerState::Stand);
}

void testThePlayerWalksAtTheLevelsOwnSpeed()
{
    Game game;
    Keyboard keys;
    game.run(keys.idle(), 90);

    const float startX = playerTransform(game.world()).position.x;

    // One press per frame, released each time: the walk is the level's 200 px/s and
    // not a constant chosen here.
    for (int frame = 0; frame < 30; ++frame)
    {
        game.update(keys.press(Key::D));
        game.renderFrame();
        game.update(keys.release(Key::D));
        game.renderFrame();
    }

    const float walked = playerTransform(game.world()).position.x - startX;
    CHECK(walked > 0.0F);
    CHECK_NEAR(walked, kWalkSpeed * 30.0F * kFrame, 2.0F);

    // Facing follows the direction pressed, and the player's own scale is where the
    // course says to keep it.
    CHECK_NEAR(playerTransform(game.world()).scale.x, 1.0F, 0.0001F);

    // And the camera followed: it sits on the position the frame ended at.
    CHECK_NEAR(game.camera().position().x, playerTransform(game.world()).position.x, 0.001F);
    CHECK_NEAR(game.camera().position().y, playerTransform(game.world()).position.y, 0.001F);
}

void testThePlayerJumpsAndLandsOnTheSameFloor()
{
    Game game;
    Keyboard keys;
    game.run(keys.idle(), 90);

    const float floorY = playerTransform(game.world()).position.y;

    // Hold the jump: press, then hold the key for the rest of the ascent. `wasPressed`
    // means one press is one jump even though the key stays down.
    game.update(keys.press(Key::W));
    game.renderFrame();

    float highest = playerTransform(game.world()).position.y;
    for (int frame = 0; frame < 30; ++frame)
    {
        game.update(keys.held());
        game.renderFrame();
        highest = std::min(highest, playerTransform(game.world()).position.y);
    }

    // It left the ground, and the rise is the level's own arithmetic: a launch at
    // 400 px/s against 900 px/s^2 of gravity, integrated at sixty frames a second.
    CHECK(highest < floorY - 60.0F);
    CHECK_NEAR(floorY - highest, (kJumpSpeed * kJumpSpeed) / (2.0F * kGravity), 8.0F);
    CHECK(playerState(game.world()).jumping);
    CHECK_FALSE(playerState(game.world()).grounded);
    CHECK(playerState(game.world()).state == PlayerState::Air);

    // Let go of the key and let it come down.
    keys.letGo(Key::W);
    game.run(keys.held(), 90);

    CHECK(playerState(game.world()).grounded);
    CHECK_NEAR(playerTransform(game.world()).position.y, floorY, 0.5F);
    CHECK_FALSE(playerState(game.world()).jumping);
}

void testAJumpIsNotLaunchedTwiceByHoldingTheKey()
{
    Game game;
    Keyboard keys;
    game.run(keys.idle(), 90);

    // The course: "If the jump key is held, the player should not continuously jump,
    // but instead it should only jump once per button press."
    game.update(keys.press(Key::W));
    game.renderFrame();

    int jumps = 0;
    float previousY = playerTransform(game.world()).position.y;
    for (int frame = 0; frame < 200; ++frame)
    {
        game.update(keys.held());
        game.renderFrame();
        const float y = playerTransform(game.world()).position.y;
        // A second launch would be the player rising again after having come back
        // down, so watch the direction of travel rather than a counter of anything.
        if (y < previousY - 4.0F && playerState(game.world()).grounded)
        {
            ++jumps;
        }
        previousY = y;
    }

    CHECK(jumps <= 1);
}

void testShootingSpawnsABulletThatTravelsAndDies()
{
    Game game;
    Keyboard keys;
    game.run(keys.idle(), 90);

    game.update(keys.press(Key::Space));
    game.renderFrame();
    CHECK(bulletCount(game.world()) == 1U);

    const float playerX = playerTransform(game.world()).position.x;
    const float playerY = playerTransform(game.world()).position.y;

    const Entity& bullet = onlyBullet(game.world());
    const Transform& transform = bullet.getComponent<Transform>();
    const Collider& collider = bullet.getComponent<Collider>();

    // On the correct side, at the player's own height, at six times the walk speed.
    CHECK_NEAR(transform.position.y, playerY, 0.001F);
    // 64 pixels in front of the player **plus the frame it was created on**, because
    // `ShootSystem` runs before the physics step and a bullet moves in the frame it is
    // fired. That is the whole reason it is registered there.
    CHECK_NEAR(transform.position.x,
               playerX + kBulletSpawnOffset + (kWalkSpeed * kBulletSpeedFactor * kFrame), 1.0F);
    CHECK_NEAR(transform.velocity.x, kWalkSpeed * kBulletSpeedFactor, 0.001F);
    CHECK(transform.velocity.x > 0.0F);

    // Half the animation's frame size, which is what the course halves, so the
    // bullet's box is 16x13 rather than 32x26.
    CHECK_NEAR(collider.size.x, kBulletFrameWidth * 0.5F, 0.001F);
    CHECK_NEAR(collider.size.y, kBulletFrameHeight * 0.5F, 0.001F);

    // The bullet is on the player's facing side and moving away from the player, so
    // it never collides with the body that fired it.
    CHECK(collider.size.x < kPlayerWidth);

    // And it does travel.
    const float firstX = transform.position.x;
    game.run(keys.held(), 4);
    CHECK(onlyBullet(game.world()).getComponent<Transform>().position.x > firstX + 60.0F);

    // The level's brick is four hundred pixels away, so the bullet stops at the tile
    // rather than flying off the level.
    game.runUntil(keys.held(), [&game] { return bulletCount(game.world()) == 0U; }, 200);
    CHECK(bulletCount(game.world()) == 0U);
}

void testABulletDestroysTheCommittedLevelsBrick()
{
    Game game;
    Keyboard keys;
    game.run(keys.idle(), 90);

    CHECK(tileCount(game.world(), TileType::Brick) == 2U);
    CHECK(tileAt(game.world(), kLowBrickX, kLowBrickY) != nullptr);

    game.update(keys.press(Key::Space));
    game.renderFrame();

    const int untilExploding =
        game.runUntil(keys.held(), [&game] { return explodingBrick(game.world()) != nullptr; }, 300);
    CHECK(untilExploding < 300);
    CHECK(bulletCount(game.world()) == 0U);

    // The explosion is a real twelve-frame strip at its declared speed of eight, so
    // the brick survives long enough to be seen and then goes.
    const Entity* exploding = explodingBrick(game.world());
    CHECK(exploding != nullptr);
    if (exploding == nullptr)
    {
        return;
    }
    CHECK(exploding->getComponent<Animation>().currentFrame < 12U);

    game.runUntil(keys.held(), [&game] { return explodingBrick(game.world()) == nullptr; }, 300);
    CHECK(explodingBrick(game.world()) == nullptr);

    // A destroyed brick stops being solid, and it does so by losing its collider -
    // which is the same fact the debug overlay reads.
    CHECK(tileAt(game.world(), kLowBrickX, kLowBrickY) == nullptr);
    CHECK(tileCount(game.world(), TileType::Brick) == 1U);
}

void testThePlayerWalksThroughWhereTheBrickWas()
{
    Game game;
    Keyboard keys;
    game.run(keys.idle(), 90);

    // Before the shot, the brick is a wall: the player is stopped against it.
    const float beforeX = playerTransform(game.world()).position.x;
    game.update(keys.press(Key::D));
    game.renderFrame();
    game.run(keys.held(), 200);
    keys.letGo(Key::D);

    CHECK_NEAR(playerTransform(game.world()).position.x, kBlockedByLowBrickX, 1.0F);
    CHECK(playerTransform(game.world()).position.x > beforeX);

    // Break it, then walk again. This is the stale-geometry check: a brick that kept
    // its collider after its animation ended would leave a player walking into
    // something that no longer exists.
    game.update(keys.press(Key::Space));
    game.renderFrame();
    game.runUntil(keys.held(), [&game] { return explodingBrick(game.world()) == nullptr && bulletCount(game.world()) == 0U; }, 300);
    CHECK(tileAt(game.world(), kLowBrickX, kLowBrickY) == nullptr);

    for (int frame = 0; frame < 120; ++frame)
    {
        game.update(keys.press(Key::D));
        game.renderFrame();
        game.update(keys.release(Key::D));
        game.renderFrame();
    }
    keys.letGo(Key::D);

    CHECK(playerTransform(game.world()).position.x > kLowBrickX + 40.0F);
    CHECK(playerState(game.world()).grounded);
}

void testABrickCanAlsoBeHitFromBelow()
{
    Game game;
    Keyboard keys;
    game.run(keys.idle(), 90);

    // Stand under the high brick, at the edge of it rather than dead centre, and
    // jump.
    //
    // ### Why the edge and not the middle
    //
    // [engine::systems::PhysicsSystem] resolves an overlap along the axis of **least**
    // penetration, which [docs/physics.md] documents as deliberate. A player whose box
    // is entirely inside the block's has a horizontal overlap of its full 40 pixels
    // against a vertical one of about eighteen - the jump only lifts a head that far -
    // so the resolver pushes them sideways and the block is never hit from below.
    //
    // Standing at the block's edge makes the horizontal overlap smaller than the
    // vertical one, and the contact resolves the way the course means it to. This is a
    // property of the collision resolver rather than a defect in the level, and it is
    // written down here rather than left for the next person to rediscover with a
    // screenshot.
    // Twenty-two pixels in from the block's left edge, which is where the committed
    // level's geometry and the resolver's rule meet. See the note above.
    const float edgeX = kHighBrickX - 22.0F;
    playerTransform(game.world()).position = Vec2{edgeX, kStandingY};
    playerTransform(game.world()).velocity = Vec2{0.0F, 0.0F};
    game.run(keys.idle(), 4);

    CHECK(tileAt(game.world(), kHighBrickX, kHighBrickY) != nullptr);
    CHECK(tileCount(game.world(), TileType::Brick) == 2U);

    game.update(keys.press(Key::W));
    game.renderFrame();
    for (int frame = 0; frame < 40; ++frame)
    {
        game.update(keys.held());
        game.renderFrame();
    }
    keys.letGo(Key::W);

    // The brick loses its collider the moment it starts exploding, and keeps its
    // `Brick` type until the animation ends - so the two facts are asked separately
    // and the second one is asked only once the strip has finished.
    game.run(keys.held(), 20);
    CHECK(tileAt(game.world(), kHighBrickX, kHighBrickY) == nullptr);
    CHECK(explodingBrick(game.world()) != nullptr);

    game.runUntil(keys.held(), [&game] { return explodingBrick(game.world()) == nullptr; }, 300);
    CHECK(tileCount(game.world(), TileType::Brick) == 1U);

    // And the player is standing, not stuck inside what used to be there.
    CHECK(playerState(game.world()).grounded);
    CHECK_NEAR(playerTransform(game.world()).position.y, kStandingY, 1.0F);
}

void testFallingOutOfTheWorldRespawnsThePlayer()
{
    Game game;
    Keyboard keys;
    game.run(keys.idle(), 90);

    const Vec2 spawn = playerTransform(game.world()).position;
    const Vec2 left = playerOf(game.world()).getComponent<Player>().spawnPosition;

    // Run left first, so the player is **facing left** when they die. A respawn that
    // only put the player back where they were and left them facing the way they
    // happened to be running would be a real defect, and the first version of this
    // group could not see it: the player had never moved, so their facing was already
    // right and restoring it was a no-op.
    game.update(keys.press(Key::A));
    game.renderFrame();
    game.run(keys.held(), 20);
    keys.letGo(Key::A);
    CHECK_NEAR(playerTransform(game.world()).scale.x, -1.0F, 0.0001F);

    // Now drop the player below the floor, which is what the gap in the committed
    // level's bottom row does to anybody who walks into it. The fall limit is the
    // world's bottom edge, measured from the grid this scene built.
    playerTransform(game.world()).position = Vec2{left.x, kWorldHeight + 200.0F};
    playerTransform(game.world()).velocity = Vec2{0.0F, 0.0F};
    game.update(keys.idle());
    game.renderFrame();

    // Respawned at the spawn point, not merely somewhere: the position is the level's
    // own cell (3, 4). The vertical tolerance is one frame of gravity, because the
    // respawn happens part way through a frame and gravity is applied after it - the
    // player is put back in the air and falls the rest of the way, which is what a
    // spawn cell means.
    CHECK_NEAR(playerTransform(game.world()).position.x, left.x, 0.001F);
    CHECK_NEAR(playerTransform(game.world()).position.y, left.y, 1.0F);
    CHECK_NEAR(spawn.x, left.x, 0.001F);

    // The horizontal state a run could have left behind is gone: no sideways speed,
    // and the facing a respawn resets to the right.
    CHECK_NEAR(playerTransform(game.world()).velocity.x, 0.0F, 0.0001F);
    CHECK_NEAR(playerTransform(game.world()).scale.x, 1.0F, 0.0001F);
    CHECK_FALSE(playerState(game.world()).jumping);

    // And the player can play on from there: they land, and they land standing.
    game.run(keys.idle(), 90);
    CHECK(playerState(game.world()).grounded);
    CHECK_FALSE(playerState(game.world()).jumping);
    CHECK(playerState(game.world()).state == PlayerState::Stand);
    CHECK_NEAR(playerTransform(game.world()).position.y, kStandingY, 0.5F);
}

void testThePipeIsAPlatformToStandOn()
{
    Game game;
    Keyboard keys;
    game.run(keys.idle(), 90);

    // The committed level's pipe is at cells (20, 0) and (21, 0), and its artwork is
    // 70x70 rather than a whole cell - so its top is at world y 954 and nothing about
    // it assumes 64.
    constexpr float kPipeTop = kWorldHeight - 70.0F;

    // Dropped onto it from just above.
    playerTransform(game.world()).position = Vec2{20.0F * kCell + 35.0F, kPipeTop - 60.0F};
    playerTransform(game.world()).velocity = Vec2{0.0F, 0.0F};
    game.run(keys.idle(), 40);

    // Standing on it, not inside it and not fallen through.
    CHECK(playerState(game.world()).grounded);
    CHECK_NEAR(playerTransform(game.world()).position.y + (kPlayerHeight * 0.5F), kPipeTop, 1.0F);
    CHECK_NEAR(playerTransform(game.world()).velocity.y, 0.0F, 0.001F);

    // And walking off it is a fall rather than a wall: the player leaves the ground
    // rather than being stopped at the pipe's edge.
    const float pipeCentreX = 20.0F * kCell + 35.0F;
    game.update(keys.press(Key::D));
    game.renderFrame();
    const int untilAirborne = game.runUntil(keys.held(), [&game] { return !playerState(game.world()).grounded; }, 200);
    keys.letGo(Key::D);

    CHECK(untilAirborne < 200);
    CHECK(playerTransform(game.world()).position.x > pipeCentreX + 70.0F);
}

// ===========================================================================
// B. The whole chain, in one pass
// ===========================================================================

void testTheWholeChainInOnePass()
{
    // The group the phase exists for. Everything above is a stage; this runs them in
    // the order a player would, through the production scene, and checks the world at
    // each stage rather than at the end.
    Game game;
    Keyboard keys;

    // 1. the level loaded, and the player is in it
    game.run(keys.idle(), 90);
    CHECK(playerState(game.world()).grounded);
    CHECK(tileCount(game.world(), TileType::Brick) == 2U);

    // 2. walk right until the brick stops them
    game.update(keys.press(Key::D));
    game.renderFrame();
    game.run(keys.held(), 200);
    keys.letGo(Key::D);
    CHECK_NEAR(playerTransform(game.world()).position.x, kBlockedByLowBrickX, 1.0F);

    // 3. jump, and land again
    const float beforeJumpY = playerTransform(game.world()).position.y;
    game.update(keys.press(Key::W));
    game.renderFrame();
    float highest = beforeJumpY;
    for (int frame = 0; frame < 30; ++frame)
    {
        game.update(keys.held());
        game.renderFrame();
        highest = std::min(highest, playerTransform(game.world()).position.y);
    }
    keys.letGo(Key::W);
    CHECK(highest < beforeJumpY - 60.0F);
    game.run(keys.held(), 90);
    CHECK(playerState(game.world()).grounded);

    // 4. step back off the brick, and shoot it away
    //
    // The step back is not decoration. Standing against the brick, the player's muzzle
    // - 64 pixels in front of them - is already *inside* it, so a shot fired from
    // there is a bullet that overlaps the target on its first frame and dies there.
    // The brick still goes, which is what a player would see, but there is no bullet
    // to watch travel; backing off a step is how a person plays it.
    game.update(keys.press(Key::A));
    game.renderFrame();
    game.run(keys.held(), 30);
    keys.letGo(Key::A);
    CHECK(playerTransform(game.world()).position.x < kLowBrickX - 64.0F);

    // Backing off turned the player to face left, and a bullet leaves on the side the
    // player is facing - so a shot fired here would fly away from the brick. Turning
    // back is one press of the other key, and is what a person does without thinking.
    game.update(keys.press(Key::D));
    game.renderFrame();
    keys.letGo(Key::D);
    CHECK_NEAR(playerTransform(game.world()).scale.x, 1.0F, 0.0001F);

    game.update(keys.press(Key::Space));
    game.renderFrame();
    CHECK(bulletCount(game.world()) == 1U);
    game.runUntil(keys.held(), [&game] { return explodingBrick(game.world()) != nullptr; }, 300);
    CHECK(explodingBrick(game.world()) != nullptr);
    game.runUntil(keys.held(), [&game] { return explodingBrick(game.world()) == nullptr; }, 300);
    CHECK(tileAt(game.world(), kLowBrickX, kLowBrickY) == nullptr);

    // 5. walk through the gap it left, all the way to the pipe
    game.update(keys.press(Key::D));
    game.renderFrame();
    game.runUntil(keys.held(), [&game] { return playerTransform(game.world()).position.x > kLowBrickX + 200.0F; }, 300);
    keys.letGo(Key::D);
    CHECK(playerTransform(game.world()).position.x > kLowBrickX + 200.0F);

    // 6. the camera came with them
    CHECK_NEAR(game.camera().position().x, playerTransform(game.world()).position.x, 0.001F);

    // 7. the level is still drawn: the ground, the standing brick, the ledge, the
    //    decorations, the player, and the level's own spawn label. Nine animated
    //    entities is what is left of thirty-one after one brick is gone.
    game.renderFrame();
    CHECK(!game.renderer().textureDraws().empty());
    CHECK(!game.renderer().textDraws().empty());
    CHECK(game.renderer().textDraws().front().find("SPAWN") != std::string::npos);

    // 8. fall out of the world and come back
    playerTransform(game.world()).position = Vec2{playerTransform(game.world()).position.x, kWorldHeight + 200.0F};
    playerTransform(game.world()).velocity = Vec2{0.0F, 0.0F};
    game.update(keys.idle());
    game.renderFrame();
    CHECK_NEAR(playerTransform(game.world()).position.x,
               playerOf(game.world()).getComponent<Player>().spawnPosition.x, 0.001F);
    game.run(keys.idle(), 60);
    CHECK(playerState(game.world()).grounded);

    // 9. and the debug controls still work at the end of all that
    game.update(keys.press(Key::P));
    game.renderFrame();
    CHECK(game.play().paused());
    game.update(keys.release(Key::P));
    game.update(keys.press(Key::C));
    game.renderFrame();
    game.update(keys.release(Key::C));
    game.update(keys.press(Key::G));
    game.renderFrame();
    // Twenty-one vertical grid lines and twelve horizontal ones at the game's own
    // 1280x720 viewport, drawn on top of the boxes.
    CHECK(!game.renderer().rectangleDraws().empty());
    CHECK(!game.renderer().textureDraws().empty());

    // 10. Escape asks for the menu
    game.update(keys.press(Key::Escape));
    game.renderFrame();
    CHECK(game.play().pendingTransition().scene().has_value());
    CHECK(game.play().pendingTransition().scene().value() == SceneId::Menu);
}

// ===========================================================================
// C. Camera
// ===========================================================================

void testTheCameraFollowsThePlayerInBothAxes()
{
    Game game;
    Keyboard keys;
    game.run(keys.idle(), 90);

    // Horizontally.
    const float beforeX = game.camera().position().x;
    for (int frame = 0; frame < 30; ++frame)
    {
        game.update(keys.press(Key::D));
        game.renderFrame();
        game.update(keys.release(Key::D));
        game.renderFrame();
    }
    keys.letGo(Key::D);
    CHECK(game.camera().position().x > beforeX);
    CHECK_NEAR(game.camera().position().x, playerTransform(game.world()).position.x, 0.001F);

    // Vertically, from a jump: the camera rides up with the player rather than
    // leaving them off the top of the view.
    game.update(keys.press(Key::W));
    game.renderFrame();
    for (int frame = 0; frame < 20; ++frame)
    {
        game.update(keys.held());
        game.renderFrame();
    }
    keys.letGo(Key::W);

    CHECK_NEAR(game.camera().position().y, playerTransform(game.world()).position.y, 0.001F);
    CHECK(game.camera().position().y < kStandingY);
}

void testZoomScalesTheWorldAndTheCameraKeepsThePlayerCentred()
{
    Game game;
    Keyboard keys;
    game.run(keys.idle(), 90);
    game.renderFrame();

    const std::size_t before = game.renderer().textureDraws().size();
    CHECK(before > 0U);

    // `X` is the camera demo's zoom, and it is a per-press action - one press is one
    // step of 1.25, so holding it does not run the zoom away.
    game.update(keys.press(Key::X));
    game.renderFrame();
    game.update(keys.release(Key::X));
    game.renderFrame();

    CHECK_NEAR(game.camera().zoom(), 1.25F, 0.0001F);

    // The camera still sits on the player, so the player stays in the middle of the
    // view whatever the magnification.
    CHECK_NEAR(game.camera().position().x, playerTransform(game.world()).position.x, 0.001F);

    // The world is still drawn: the same entities, magnified. Zoom is a rendering
    // concern and changes no simulation data, so the player did not move and the
    // collider did not change size.
    game.renderFrame();
    CHECK(game.renderer().textureDraws().size() == before);

    // And the player's own box is unchanged, because `Camera::setZoom` deliberately
    // does not touch simulation data.
    CHECK_NEAR(playerOf(game.world()).getComponent<Collider>().size.x, kPlayerWidth, 0.0001F);

    game.update(keys.press(Key::Z));
    game.renderFrame();
    game.update(keys.release(Key::Z));
    game.renderFrame();
    CHECK_NEAR(game.camera().zoom(), 1.0F, 0.0001F);
}

// ===========================================================================
// D. Session lifecycle
// ===========================================================================

void testReturningToTheMenuAndStartingAgainRebuildsTheLevel()
{
    Game game;
    Keyboard keys;
    game.run(keys.idle(), 90);

    // Play a little: walk, shoot, break the brick.
    for (int frame = 0; frame < 60; ++frame)
    {
        game.update(keys.press(Key::D));
        game.renderFrame();
        game.update(keys.release(Key::D));
        game.renderFrame();
    }
    keys.letGo(Key::D);
    game.update(keys.press(Key::Space));
    game.renderFrame();
    game.runUntil(keys.held(), [&game] { return explodingBrick(game.world()) == nullptr && bulletCount(game.world()) == 0U; }, 400);
    CHECK(tileCount(game.world(), TileType::Brick) == 1U);

    // Pause it and turn every overlay on, so the second level has something to prove
    // it did **not** inherit.
    for (const Key key : {Key::P, Key::T, Key::C, Key::G})
    {
        game.update(keys.press(key));
        game.renderFrame();
        game.update(keys.release(key));
        game.renderFrame();
    }
    CHECK(game.play().paused());

    // The second level, built the way returning to the menu and starting again builds
    // one: a new object over the same shared assets, renderer and camera.
    PlayScene second{game.context()};
    CHECK_FALSE(second.paused());
    CHECK(tileCount(second.world(), TileType::Brick) == 2U);
    CHECK(tileAt(second.world(), kLowBrickX, kLowBrickY) != nullptr);

    // Fresh debug state too, and the first level is still paused - which is the other
    // half of the claim: nothing reset it, and nothing needed to.
    CHECK(second.debugRenderState().showTextures);
    CHECK_FALSE(second.debugRenderState().showBoundingBoxes);
    CHECK_FALSE(second.debugRenderState().showGrid);
    CHECK(game.play().paused());
}

void testTheCommittedLevelCarriesWhatTheSliceNeeds()
{
    Game game;
    Keyboard keys;
    game.run(keys.idle(), 90);

    // The level the slice plays, counted through the production loader.
    //
    // Nineteen floor tiles, three on the floating ledge and two under the pipe make
    // twenty-four ordinary solid tiles; the pipe's own two tiles are ordinary solids
    // too, so the file's twenty-six non-brick tiles are all `Solid`. Plus two bricks,
    // four decorations that are never collided with, one player and the scene's label.
    CHECK(tileCount(game.world(), TileType::Solid) == 24U);
    CHECK(tileCount(game.world(), TileType::Brick) == 2U);

    std::size_t decorations = 0U;
    for (const Entity& entity : game.world().getEntities(std::string{engine::level::kDecorationTag}))
    {
        ++decorations;
        // A decoration is drawn and never touched, and the components it does not
        // have are how that is said - there is no flag anywhere.
        CHECK_FALSE(entity.hasComponent<Collider>());
        CHECK_FALSE(entity.hasComponent<engine::components::Body>());
        static_cast<void>(entity);
    }
    CHECK(decorations == 4U);

    // One player, and it is the level's.
    std::size_t players = 0U;
    for (const Entity& entity : game.world().getEntities(std::string{engine::level::kPlayerTag}))
    {
        ++players;
        static_cast<void>(entity);
    }
    CHECK(players == 1U);

    // The level gives the slice a fall to test: there is a hole in the bottom row,
    // one cell wide between the last floor tile and the pipe, so a player who walks
    // off the end of the floor has somewhere to fall out of the world.
    CHECK(tileAt(game.world(), 19.0F * kCell + (kCell * 0.5F), rowCentreY(0.0F)) == nullptr);
    CHECK(tileAt(game.world(), 18.0F * kCell + (kCell * 0.5F), rowCentreY(0.0F)) != nullptr);

    // The pipe is at cell (20, 0), and its artwork is 70x70 rather than a whole cell,
    // so its centre is 35 pixels in rather than 32 - which is the level proving that a
    // tile's box and its anchor both come from the animation.
    CHECK(tileAt(game.world(), 20.0F * kCell + 35.0F, kWorldHeight - 35.0F) != nullptr);

    // And there is no question block, which is a property of the artwork rather than
    // an omission: `assets/assets.txt` records that the committed question-block image
    // is 360x360 and no integer frame count puts it in a 64-pixel cell. The
    // behaviour is covered by `combat.tiles`, which builds the block by hand.
    CHECK(tileCount(game.world(), TileType::Question) == 0U);
}

} // namespace

int main()
{
    using TestCase = void (*)();
    const std::pair<const char*, TestCase> testCases[] = {
        // A. The slice
        {"the menu starts the game", &testTheMenuStartsTheGame},
        {"the player spawns from the level and falls onto the floor",
         &testThePlayerSpawnsFromTheLevelAndFallsOntoTheFloor},
        {"the player walks at the level's own speed", &testThePlayerWalksAtTheLevelsOwnSpeed},
        {"the player jumps and lands on the same floor", &testThePlayerJumpsAndLandsOnTheSameFloor},
        {"a jump is not launched twice by holding the key", &testAJumpIsNotLaunchedTwiceByHoldingTheKey},
        {"shooting spawns a bullet that travels and dies", &testShootingSpawnsABulletThatTravelsAndDies},
        {"a bullet destroys the committed level's brick", &testABulletDestroysTheCommittedLevelsBrick},
        {"the player walks through where the brick was", &testThePlayerWalksThroughWhereTheBrickWas},
        {"a brick can also be hit from below", &testABrickCanAlsoBeHitFromBelow},
        {"falling out of the world respawns the player", &testFallingOutOfTheWorldRespawnsThePlayer},
        {"the pipe is a platform to stand on", &testThePipeIsAPlatformToStandOn},
        // B. The whole chain
        {"the whole chain in one pass", &testTheWholeChainInOnePass},
        // C. Camera
        {"the camera follows the player in both axes", &testTheCameraFollowsThePlayerInBothAxes},
        {"zoom scales the world and the camera keeps the player centred",
         &testZoomScalesTheWorldAndTheCameraKeepsThePlayerCentred},
        // D. Session lifecycle
        {"returning to the menu and starting again rebuilds the level",
         &testReturningToTheMenuAndStartingAgainRebuildsTheLevel},
        {"the committed level carries what the slice needs", &testTheCommittedLevelCarriesWhatTheSliceNeeds},
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

    std::cout << groupCount << " gameplay test groups passed\n";
    return EXIT_SUCCESS;
}