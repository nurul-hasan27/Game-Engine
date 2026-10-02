/// Debug controls: pause, the three rendering toggles, and Escape.
///
/// ### What is driven and what is observed
///
/// Every behavioural group builds a **real** [engine::scene::PlayScene] over the real
/// committed level, with the real [engine::assets::SfmlAssetManager], and presses
/// **keys** on a real [engine::input::Input] resolved through the real default action
/// map - the same path [engine::Application] takes. Then it reads what came out:
///
/// - **Pause** is read off the world, not off a flag. A group asserts that the
///   player's position, velocity, jump state, bullet position, bullet lifetime,
///   explosion frame and camera all held still for sixty frames while `P` was down,
///   and that every one of them moved again afterwards. `paused()` is asserted too,
///   but only beside those: the flag says the control fired, the world says what it
///   did.
/// - **Textures, boxes and the grid** are read off the **draw calls**. The suite
///   drives a recording [engine::graphics::Renderer] and asserts what was submitted -
///   one rectangle at the collider's own size for every collider and nothing else,
///   one 1-pixel line per cell boundary at a world position that is an exact multiple
///   of 64 - rather than asserting a bool. A flag test would pass against a
///   renderer that ignored it, and this phase's whole claim is that the renderer
///   does not.
///
/// The renderer is a recording double rather than a real window because the claim is
/// about *which draws were submitted*. A screenshot of a magenta box would assert
/// far less than "exactly the colliders, at their own sizes".
///
/// ### What is a source check, and why only that
///
/// Three things have no run-time observable, and they are checked in the source and
/// nowhere else, each with the reason written next to it:
///
/// - **No scene or debug renderer names a physical key.** A system handed an
///   [engine::input::ActionState] *cannot* ask, which is a type-level guarantee for
///   anything taking actions - but a scene holds no keyboard at all, so only the
///   source can say whether one reached for `sf::Keyboard`.
/// - **No gameplay system knows pause or a debug flag exists.** That is a claim about
///   six files, none of which is passed a pause flag, so there is nothing to observe
///   from outside them except that they do not mention it.
/// - **The debug layer holds no static storage.** A `static DebugRenderState` would
///   behave identically in every test that exists in this file, and identically in
///   the game - which is exactly why it has to be checked by reading.
///
/// ### What is deliberately not asserted
///
/// No screenshot, no image hash and no "the pixel at (x, y) is magenta". The
/// repository's own rule is in [docs/rendering.md](docs/rendering.md) §8: readback
/// on this platform returns correct values *before* `display()` and black after, so
/// a pixel test can prove a draw was correct and never prove the frame was presented.
/// The stronger assertion available here is the draw call itself, and that is what
/// these groups use.
#include "engine/Color.hpp"
#include "engine/EngineConfig.hpp"
#include "engine/assets/Animation.hpp"
#include "engine/assets/AssetManager.hpp"
#include "engine/assets/Font.hpp"
#include "engine/assets/SfmlAssetManager.hpp"
#include "engine/assets/Texture.hpp"
#include "engine/components/Animation.hpp"
#include "engine/components/Bullet.hpp"
#include "engine/components/Collider.hpp"
#include "engine/components/Lifetime.hpp"
#include "engine/components/Player.hpp"
#include "engine/components/PlayerConfig.hpp"
#include "engine/components/Rectangle.hpp"
#include "engine/components/Text.hpp"
#include "engine/components/Tile.hpp"
#include "engine/components/Transform.hpp"
#include "engine/debug/DebugRenderState.hpp"
#include "engine/ecs/EntityManager.hpp"
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
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
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

using engine::Color;
using engine::IntRect;
using engine::Vec2;
using engine::assets::AssetManager;
using engine::components::Animation;
using engine::components::Bullet;
using engine::components::Collider;
using engine::components::Lifetime;
using engine::components::Player;
using engine::components::PlayerState;
using engine::components::Tile;
using engine::components::TileType;
using engine::components::Transform;
using engine::debug::DebugRenderState;
using engine::ecs::Entity;
using engine::ecs::EntityId;
using engine::ecs::EntityManager;
using engine::graphics::Camera;
using engine::graphics::RenderTransform;
using engine::graphics::Renderer;
using engine::input::Action;
using engine::input::ActionState;
using engine::input::defaultActionMap;
using engine::input::Input;
using engine::input::Key;
using engine::level::LevelGrid;
using engine::scene::MenuScene;
using engine::scene::PlayScene;
using engine::scene::SceneContext;
using engine::scene::SceneId;

/// One sixtieth of a second: the engine's frame cap, so a group stepping with another
/// number would be testing another game.
constexpr float kFrame = 1.0F / 60.0F;

/// The course's cell size, restated so a group can say "the grid is 64 pixels" without
/// reading the value it is about to compare against.
constexpr float kCell = LevelGrid::kCellSize;

/// The window the game opens, in pixels. The grid overlay's line count is a function of
/// the viewport, so it is written down here rather than derived from the one being
/// tested.
constexpr float kWindowWidth = static_cast<float>(engine::config::kWindowWidth);
constexpr float kWindowHeight = static_cast<float>(engine::config::kWindowHeight);

/// How thick a grid line is in world pixels, as [engine::systems::DebugRenderSystem]
/// declares it. Written down rather than read so the group says what it expects.
constexpr float kGridLineThickness = 1.0F;

/// How long "while paused" is tested for: three seconds of frames, which is longer than
/// anything in the level can survive if it is not actually stopped.
constexpr int kPausedFrames = 180;

// ---------------------------------------------------------------------------
// Source reading, for the three checks with no run-time observable.
// ---------------------------------------------------------------------------

[[nodiscard]] std::string stripComments(std::string code)
{
    std::string result;
    result.reserve(code.size());

    enum class State
    {
        Code,
        LineComment,
        BlockComment,
        String
    };

    State state = State::Code;

    for (std::size_t index = 0U; index < code.size(); ++index)
    {
        const char c = code[index];
        const char next = (index + 1U < code.size()) ? code[index + 1U] : '\0';

        switch (state)
        {
            case State::Code:
                if (c == '/' && next == '/')
                {
                    state = State::LineComment;
                    result += "  ";
                    ++index;
                }
                else if (c == '/' && next == '*')
                {
                    state = State::BlockComment;
                    result += "  ";
                    ++index;
                }
                else
                {
                    if (c == '"')
                    {
                        state = State::String;
                    }
                    result += c;
                }
                break;

            case State::String:
                result += c;
                if (c == '"')
                {
                    state = State::Code;
                }
                break;

            case State::LineComment:
                if (c == '\n')
                {
                    // The newline is kept, so a stripped file still has as many lines
                    // as the original and a line-numbered diagnostic still means
                    // something.
                    result += c;
                    state = State::Code;
                }
                else
                {
                    result += ' ';
                }
                break;

            case State::BlockComment:
                if (c == '*' && next == '/')
                {
                    result += "  ";
                    ++index;
                    state = State::Code;
                }
                else
                {
                    result += (c == '\n') ? '\n' : ' ';
                }
                break;
        }
    }

    return result;
}

[[nodiscard]] std::string codeOf(const char* const path)
{
    std::ifstream file{path, std::ios::binary};
    if (!file.is_open())
    {
        return {};
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

[[nodiscard]] std::string codeWithoutComments(const char* const path) { return stripComments(codeOf(path)); }

[[nodiscard]] bool has(const std::string& haystack, const std::string_view needle)
{
    return haystack.find(needle) != std::string::npos;
}

[[nodiscard]] std::size_t countOf(const std::string& haystack, const std::string_view needle)
{
    std::size_t count = 0U;
    std::size_t position = haystack.find(needle);
    while (position != std::string::npos)
    {
        ++count;
        position = haystack.find(needle, position + needle.size());
    }
    return count;
}

[[nodiscard]] bool isWordChar(const char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/// True when `code` declares static **storage**: a static data member or a file-scope
/// static variable.
///
/// Not a substring scan, and the reasons are the three false positives a substring
/// scan produces in this engine: `static_cast` and `static_assert` are single tokens,
/// `constexpr` objects have the same value in every translation unit and cannot be
/// written to, and a static *member function* is not storage. A check that cries wolf
/// is worse than none, because its reader learns to skip it - so this looks at what
/// follows the keyword and at the shape of the declaration, exactly as `scene.lifecycle`
/// does for the scene layer.
[[nodiscard]] bool declaresStaticStorage(const std::string& code)
{
    std::size_t at = code.find("static");

    while (at != std::string::npos)
    {
        const std::size_t after = at + 6U;
        const bool rightOk = (after >= code.size()) || !isWordChar(code[after]);
        const bool leftOk = (at == 0U) || !isWordChar(code[at - 1U]);

        if (rightOk && leftOk && after < code.size() && (code[after] == ' ' || code[after] == '\t'))
        {
            const std::size_t paren = code.find('(', after);
            const std::size_t equals = code.find('=', after);
            const std::size_t semi = code.find(';', after);

            const std::size_t firstEnd = std::min(equals, semi);
            const bool isFunction =
                (paren != std::string::npos) && (firstEnd == std::string::npos || paren < firstEnd);

            if (!isFunction)
            {
                const std::size_t name = code.find_first_not_of(" \t", after);
                const std::string_view head{code};
                const bool isConstant = name != std::string::npos && head.substr(name, 10U) == "constexpr ";
                if (!isConstant)
                {
                    return true;
                }
            }
        }

        at = code.find("static", at + 1U);
    }

    return false;
}

// ---------------------------------------------------------------------------
// The recording renderer: what a render pass actually submitted.
// ---------------------------------------------------------------------------

struct DrawCall
{
    Vec2 size;
    Color color;
    RenderTransform placement;
};

struct TextureDrawCall
{
    const engine::assets::Texture* texture = nullptr;
    RenderTransform placement;
    std::optional<IntRect> source;
};

struct TextDrawCall
{
    std::string content;
    RenderTransform placement;
};

/// Records every draw, in order, and draws nothing.
///
/// The point of the phase is which draws were **submitted**, so that is exactly what
/// this records: the size, the colour and the screen placement of every rectangle, and
/// the source region of every image. A window would show that some ink appeared; this
/// shows that the *collider's own size* was submitted at the *collider's own
/// position*, which is the claim being tested.
class RecordingRenderer final : public Renderer
{
public:
    void beginFrame() override
    {
        ++m_beginFrames;
        m_draws.clear();
        m_textureDraws.clear();
        m_textDraws.clear();
    }

    void clear(const Color&) override {}

    void drawRectangle(const Vec2& size, const Color& color, const RenderTransform& placement) override
    {
        m_draws.push_back(DrawCall{size, color, placement});
    }

    void drawTexture(const engine::assets::Texture& texture, const RenderTransform& placement,
                     const std::optional<IntRect>& source) override
    {
        m_textureDraws.push_back(TextureDrawCall{&texture, placement, source});
    }

    void drawText(const engine::assets::Font&, const std::string& content, std::uint32_t, const Color&,
                  const RenderTransform& placement) override
    {
        m_textDraws.push_back(TextDrawCall{content, placement});
    }

    void endFrame() override { ++m_endFrames; }

    [[nodiscard]] std::uint64_t frameCount() const noexcept override { return m_endFrames; }

    [[nodiscard]] const std::vector<DrawCall>& draws() const noexcept { return m_draws; }
    [[nodiscard]] const std::vector<TextureDrawCall>& textureDraws() const noexcept { return m_textureDraws; }
    [[nodiscard]] const std::vector<TextDrawCall>& textDraws() const noexcept { return m_textDraws; }

private:
    std::vector<DrawCall> m_draws;
    std::vector<TextureDrawCall> m_textureDraws;
    std::vector<TextDrawCall> m_textDraws;
    std::size_t m_beginFrames = 0U;
    std::size_t m_endFrames = 0U;
};

// ---------------------------------------------------------------------------
// Driving the real action map with real key presses.
// ---------------------------------------------------------------------------

class ActionDriver
{
public:
    /// A fresh press: active **and** pressed this frame.
    ///
    /// `beginFrame()` runs first, which is the order [engine::Application::run] uses:
    /// clear the frame-local edges, *then* record this frame's events. Without it the
    /// previous call's press edge would still be set, and a group that pressed `P`
    /// and then `C` would silently press `P` twice - which is exactly what happened
    /// to the first version of the pause group, and is why this comment exists.
    [[nodiscard]] ActionState pressedNow(const Key key)
    {
        m_input.beginFrame();
        m_input.processKeyDown(key);
        return snapshot();
    }

    /// The frames after a press, where the key is still physically down but is no
    /// longer a new press. This is the entire distinction between an edge-triggered
    /// control and a level-triggered one, and most of these groups depend on it.
    [[nodiscard]] ActionState held()
    {
        m_input.beginFrame();
        return snapshot();
    }

    /// One frame in which `key` has come up.
    [[nodiscard]] ActionState released(const Key key)
    {
        m_input.beginFrame();
        m_input.processKeyUp(key);
        return snapshot();
    }

    /// Nothing held, and no edge.
    [[nodiscard]] ActionState idle()
    {
        m_input.beginFrame();
        return snapshot();
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

// ---------------------------------------------------------------------------
// Looking into a level.
// ---------------------------------------------------------------------------

[[nodiscard]] Entity& playerOf(const EntityManager& world)
{
    // The tag is a **named** `std::string`, and that is load bearing rather than
    // style. `EntityView` holds its tag as a `std::string_view` and `getEntities`
    // forwards its parameter straight into it, so a temporary would leave the view
    // pointing at a string that died at the end of the same full expression. The
    // Debug build happens to work; the Release build does not. `MenuScene::refreshSelection`
    // documents the same trap in the place it was found.
    const std::string tag{engine::level::kPlayerTag};
    for (const Entity& entity : world.getEntities(tag))
    {
        return const_cast<Entity&>(entity);
    }
    throw std::logic_error{"the world has no player"};
}

[[nodiscard]] Transform& playerTransform(const EntityManager& world)
{
    return playerOf(world).getComponent<Transform>();
}

[[nodiscard]] Player& playerState(const EntityManager& world) { return playerOf(world).getComponent<Player>(); }

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
    throw std::logic_error{"the world has no bullet"};
}

/// How many entities carry both a transform and a collider, which is exactly the set
/// the bounding-box overlay is allowed to draw.
[[nodiscard]] std::size_t colliderCount(const EntityManager& world)
{
    std::size_t count = 0U;
    for (const Entity& entity : world.getEntities())
    {
        if (entity.hasComponent<Transform>() && entity.hasComponent<Collider>())
        {
            ++count;
        }
    }
    return count;
}

/// The brick that has started exploding, or `nullptr` if none has.
///
/// Found by the animation the tile system points a broken brick at rather than by a
/// position, so the group says "the brick I broke" rather than "the fourth tile the
/// loader happened to create".
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

/// Every collider size in the level, sorted, so a group can say "the pipe's own 70x70
/// is among the boxes drawn" without caring what order the ECS produced.
[[nodiscard]] std::vector<Vec2> colliderSizes(const EntityManager& world)
{
    std::vector<Vec2> sizes;
    for (const Entity& entity : world.getEntities())
    {
        if (entity.hasComponent<Collider>())
        {
            sizes.push_back(entity.getComponent<Collider>().size);
        }
    }
    std::sort(sizes.begin(), sizes.end(), [](const Vec2 a, const Vec2 b) { return a.y < b.y || (a.y == b.y && a.x < b.x); });
    return sizes;
}

// ---------------------------------------------------------------------------
// The fixture: a real scene over the real level, drawing through a recording renderer.
// ---------------------------------------------------------------------------

/// The whole point of a *recording* renderer rather than a window: the assertions below
/// are about which draws were submitted, so the fixture hands out what was submitted.
///
/// No window at all, which means this suite needs no GPU and no display - a property
/// worth having for the phase whose claim is "the debug controls do not disturb
/// anything", because it means nothing here can be passing because of what the platform
/// happened to render.
class DebugFixture
{
public:
    DebugFixture()
        : m_assets{std::filesystem::path{engine::config::kAssetsConfig}}
    {
        // The game's own viewport, so the grid's line count is exercised at a
        // realistic size rather than at the degenerate zero a default camera has.
        m_camera.setViewport(Vec2{kWindowWidth, kWindowHeight});
        m_context.emplace(m_renderer, m_camera, m_assets);
        m_play = std::make_unique<PlayScene>(*m_context);
    }

    [[nodiscard]] PlayScene& play() noexcept { return *m_play; }
    [[nodiscard]] const EntityManager& world() const noexcept { return m_play->world(); }
    [[nodiscard]] EntityManager& world() noexcept { return m_play->world(); }
    [[nodiscard]] const AssetManager& assets() const noexcept { return m_assets; }
    [[nodiscard]] Camera& camera() noexcept { return m_camera; }
    [[nodiscard]] const Camera& camera() const noexcept { return m_camera; }
    [[nodiscard]] RecordingRenderer& renderer() noexcept { return m_renderer; }
    [[nodiscard]] const RecordingRenderer& renderer() const noexcept { return m_renderer; }
    [[nodiscard]] const SceneContext& context() const noexcept { return *m_context; }

    /// One update of the scene, exactly as `Application::update` calls it.
    void update(const ActionState& actions) { m_play->update(actions, kFrame); }

    /// One render pass, bracketed the way [engine::Application::render] brackets it.
    /// The scene submits draws; it does not know about frames.
    void renderFrame()
    {
        m_renderer.beginFrame();
        m_renderer.clear(Color{0.0F, 0.0F, 0.0F, 1.0F});
        m_play->render();
        m_renderer.endFrame();
    }

    /// Runs `count` frames of `actions` and renders each one.
    void run(const ActionState& actions, const int count)
    {
        for (int frame = 0; frame < count; ++frame)
        {
            update(actions);
            renderFrame();
        }
    }

    /// Steps until `until` is true, rendering each frame. Steps **then** checks:
    /// checking first would return on the frame the system wrote the state and never
    /// run the frame that acts on it.
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

    /// How long the player takes to land on the floor from the spawn point.
    void settle(ActionDriver& driver) { run(driver.idle(), 90); }

private:
    RecordingRenderer m_renderer;
    engine::assets::SfmlAssetManager m_assets;
    Camera m_camera;
    std::optional<SceneContext> m_context;
    std::unique_ptr<PlayScene> m_play;
};

// ---------------------------------------------------------------------------
// Reading the grid's lines back out of the recorded draws.
// ---------------------------------------------------------------------------

/// Every rectangle the grid pass submitted, split by orientation.
struct GridLines
{
    std::vector<DrawCall> vertical;
    std::vector<DrawCall> horizontal;
};

/// Reads the recorded rectangles as grid lines.
///
/// The two orientations are told apart by which axis is one world pixel wide, which is
/// how the engine draws them; anything else counts as neither, so a group asserting
/// "21 vertical and 12 horizontal" cannot be satisfied by 33 rectangles of the wrong
/// shape.
[[nodiscard]] GridLines gridLinesOf(const DebugFixture& fixture)
{
    GridLines lines;
    for (const DrawCall& draw : fixture.renderer().draws())
    {
        const bool thinX = draw.size.x == kGridLineThickness && draw.size.y != kGridLineThickness;
        const bool thinY = draw.size.y == kGridLineThickness && draw.size.x != kGridLineThickness;

        if (thinX)
        {
            lines.vertical.push_back(draw);
        }
        else if (thinY)
        {
            lines.horizontal.push_back(draw);
        }
    }
    return lines;
}

// ===========================================================================
// A. Pause
// ===========================================================================

void testPStopsThePlayerDead()
{
    ActionDriver driver;
    DebugFixture fixture;
    fixture.settle(driver);

    // Walking first, so "the position stopped changing" cannot pass because nothing
    // was moving anyway.
    fixture.run(driver.pressedNow(Key::D), 10);
    const Vec2 walking = playerTransform(fixture.world()).position;
    CHECK_NEAR(walking.x, 212.0F + (200.0F * 10.0F * kFrame), 0.5F);

    // Pause, on the press edge.
    fixture.update(driver.pressedNow(Key::P));
    CHECK(fixture.play().paused());

    // Every input that would change something, held for the whole of the pause: walk
    // right, jump, shoot. Three seconds of frames.
    const Vec2 frozenPosition = playerTransform(fixture.world()).position;
    const Vec2 frozenVelocity = playerTransform(fixture.world()).velocity;
    const Player frozenPlayer = playerState(fixture.world());
    const Camera frozenCamera = fixture.camera();

    for (int frame = 0; frame < kPausedFrames; ++frame)
    {
        fixture.update(driver.held());
    }

    const Transform& after = playerTransform(fixture.world());
    CHECK_NEAR(after.position.x, frozenPosition.x, 0.0001F);
    CHECK_NEAR(after.position.y, frozenPosition.y, 0.0001F);
    CHECK_NEAR(after.velocity.x, frozenVelocity.x, 0.0001F);
    CHECK_NEAR(after.velocity.y, frozenVelocity.y, 0.0001F);

    // The player's own state machine is frozen too, including the jump gate: a jump
    // pressed while paused must not leave `jumping` set, because a player who
    // unpaused mid-jump would be launched by a key they let go three seconds ago.
    const Player& stillFrozen = playerState(fixture.world());
    CHECK(stillFrozen.state == frozenPlayer.state);
    CHECK(stillFrozen.grounded == frozenPlayer.grounded);
    CHECK_FALSE(stillFrozen.jumping);

    // And the camera, which is a system like any other and would otherwise drift.
    CHECK_NEAR(fixture.camera().position().x, frozenCamera.position().x, 0.0001F);
    CHECK_NEAR(fixture.camera().position().y, frozenCamera.position().y, 0.0001F);

    // No respawn either: the player is well above the fall limit and stayed put.
    CHECK_NEAR(after.position.y, frozenPosition.y, 0.0001F);

    // Unpause, and the world moves again. Released first, so the second press is a
    // genuine edge rather than a key that never came up.
    fixture.update(driver.released(Key::P));
    fixture.update(driver.pressedNow(Key::P));
    CHECK_FALSE(fixture.play().paused());

    fixture.run(driver.held(), 10);
    CHECK(playerTransform(fixture.world()).position.x > frozenPosition.x + 10.0F);
}

void testHoldingPDoesNotTogglePauseEveryFrame()
{
    ActionDriver driver;
    DebugFixture fixture;

    // One press pauses.
    fixture.update(driver.pressedNow(Key::P));
    CHECK(fixture.play().paused());

    // Three seconds with the key still physically down. This is the mistake the
    // course's own sentence warns about for the jump key, wearing a pause costume: a
    // level-triggered toggle would flip sixty times a second and the game would
    // appear to stutter, while a paused game would appear to flicker.
    for (int frame = 0; frame < kPausedFrames; ++frame)
    {
        fixture.update(driver.held());
        CHECK(fixture.play().paused());
    }

    // Release and press again: one more edge, one more toggle.
    fixture.update(driver.released(Key::P));
    fixture.update(driver.held());
    CHECK(fixture.play().paused());

    fixture.update(driver.pressedNow(Key::P));
    CHECK_FALSE(fixture.play().paused());

    // And a held key does not resume it either.
    for (int frame = 0; frame < 30; ++frame)
    {
        fixture.update(driver.held());
        CHECK_FALSE(fixture.play().paused());
    }
}

void testAPausedGameStillDrawsTheSamePicture()
{
    ActionDriver driver;
    DebugFixture fixture;
    fixture.settle(driver);

    fixture.renderFrame();
    const std::size_t texturesBefore = fixture.renderer().textureDraws().size();
    const std::size_t textBefore = fixture.renderer().textDraws().size();
    CHECK(texturesBefore > 0U);

    // Pause, and draw a hundred frames while stopped.
    fixture.update(driver.pressedNow(Key::P));
    for (int frame = 0; frame < 100; ++frame)
    {
        fixture.update(driver.held());
        fixture.renderFrame();
    }

    // The frame is still being produced, with the same content: pause stops the
    // simulation, not the render loop. A pause that went blank would leave the player
    // unable to see what they paused to look at.
    CHECK(fixture.renderer().textureDraws().size() == texturesBefore);
    CHECK(fixture.renderer().textDraws().size() == textBefore);
    CHECK(fixture.renderer().frameCount() > 0U);
}

void testABulletStopsAndKeepsItsLifetimeAndThenResumes()
{
    ActionDriver driver;
    DebugFixture fixture;
    fixture.settle(driver);

    // Fire, and let the bullet travel so it is demonstrably in flight rather than
    // sitting on the player: one frame of travel in the shot's own frame, plus three.
    fixture.update(driver.pressedNow(Key::Space));
    CHECK(bulletCount(fixture.world()) == 1U);

    fixture.run(driver.held(), 3);

    // The course's own arithmetic, written out: the player spawns at cell (3, 4)
    // with a 40-wide box, so its centre is at world x 212; a bullet is created 64
    // pixels in front of that and travels at six times the level's 200 px/s.
    constexpr float kSpawnX = 212.0F + 64.0F;
    constexpr float kBulletSpeed = 200.0F * 6.0F;
    const int travelFrames = 4;

    const Transform flying = onlyBullet(fixture.world()).getComponent<Transform>();
    const Lifetime alive = onlyBullet(fixture.world()).getComponent<Lifetime>();
    const Animation playing = onlyBullet(fixture.world()).getComponent<Animation>();
    CHECK_NEAR(flying.position.x, kSpawnX + (kBulletSpeed * static_cast<float>(travelFrames) * kFrame), 0.5F);
    CHECK(alive.remainingFrames > 100U);

    // Pause for three seconds of frames.
    fixture.update(driver.pressedNow(Key::P));
    for (int frame = 0; frame < kPausedFrames; ++frame)
    {
        fixture.update(driver.held());
    }

    // Still alive, and unchanged in all three ways a bullet can change: where it is,
    // how long it has left, and which frame of its animation is showing.
    //
    // The lifetime is the one a naive pause gets wrong. `LifetimeSystem` counts frames,
    // and a "pause" that only skipped physics would let a hundred and nine frames of
    // life evaporate while the player watched - so the bullet would expire in mid-air
    // the instant they unpaused.
    CHECK(bulletCount(fixture.world()) == 1U);
    const Entity& bullet = onlyBullet(fixture.world());
    CHECK_NEAR(bullet.getComponent<Transform>().position.x, flying.position.x, 0.0001F);
    CHECK_NEAR(bullet.getComponent<Transform>().position.y, flying.position.y, 0.0001F);
    CHECK(bullet.getComponent<Lifetime>().remainingFrames == alive.remainingFrames);
    CHECK(bullet.getComponent<Animation>().currentFrame == playing.currentFrame);
    CHECK(bullet.getComponent<Animation>().ticksOnFrame == playing.ticksOnFrame);

    // Unpause and it carries on from exactly there.
    //
    // **Four** frames of travel, not three, and the reason is a property of the
    // design rather than an off-by-one: the frame `P` is pressed on is itself the
    // first *live* frame, exactly as the frame `P` is pressed on to pause is the
    // first dead one. Both take effect immediately. Three frames are asked for here
    // and the unpause frame is the fourth.
    //
    // Three rather than ten, because the committed level's brick at cell (8, 1)
    // spans world x 512 to 576: a longer run would put the bullet inside it and the
    // assertion below would be about a bullet that had correctly died.
    fixture.update(driver.released(Key::P));
    fixture.update(driver.pressedNow(Key::P));
    fixture.run(driver.held(), 3);

    const Entity& moved = onlyBullet(fixture.world());
    CHECK_NEAR(moved.getComponent<Transform>().position.x,
               flying.position.x + (kBulletSpeed * 4.0F * kFrame), 1.0F);
    CHECK(moved.getComponent<Lifetime>().remainingFrames < alive.remainingFrames);
}

void testAnExplosionStopsWithTheWorldAndFinishesWhenItResumes()
{
    ActionDriver driver;
    DebugFixture fixture;
    fixture.settle(driver);

    // Break the committed level's brick at cell (8, 1) with a shot from the spawn.
    fixture.update(driver.pressedNow(Key::Space));
    const int untilExploding =
        fixture.runUntil(driver.held(), [&fixture] { return explodingBrick(fixture.world()) != nullptr; }, 300);

    CHECK(untilExploding < 300);
    const Entity* brick = explodingBrick(fixture.world());
    CHECK(brick != nullptr);
    if (brick == nullptr)
    {
        return;
    }

    const Transform brokenPosition = brick->getComponent<Transform>();
    const Animation playing = brick->getComponent<Animation>();
    // A brick that has exploded has lost its collider, which is what makes it no
    // longer solid. Recorded so the pause can be shown not to give it back.
    CHECK_FALSE(brick->hasComponent<Collider>());

    // Pause for three seconds - far longer than the twelve-frame strip at its declared
    // speed of eight, which is ninety-six frames. A pause that let the animation
    // through would finish the explosion and delete the brick while the game was
    // supposedly stopped, which is the failure mode this group exists to catch.
    fixture.update(driver.pressedNow(Key::P));
    for (int frame = 0; frame < kPausedFrames; ++frame)
    {
        fixture.update(driver.held());
    }

    CHECK(explodingBrick(fixture.world()) != nullptr);
    const Entity& frozen = *explodingBrick(fixture.world());
    CHECK_NEAR(frozen.getComponent<Transform>().position.x, brokenPosition.position.x, 0.0001F);
    CHECK_NEAR(frozen.getComponent<Transform>().position.y, brokenPosition.position.y, 0.0001F);
    CHECK(frozen.getComponent<Animation>().currentFrame == playing.currentFrame);
    CHECK(frozen.getComponent<Animation>().ticksOnFrame == playing.ticksOnFrame);
    CHECK_FALSE(frozen.hasComponent<Collider>());

    // Unpause, and the explosion runs out and takes the brick with it.
    fixture.update(driver.released(Key::P));
    fixture.update(driver.pressedNow(Key::P));
    const int untilGone = fixture.runUntil(driver.held(), [&fixture] { return explodingBrick(fixture.world()) == nullptr; }, 400);

    CHECK(untilGone < 400);
    CHECK(explodingBrick(fixture.world()) == nullptr);
    CHECK(bulletCount(fixture.world()) == 0U);
}

void testPauseIsPerSceneAndNotShared()
{
    ActionDriver driver;
    DebugFixture fixture;

    PlayScene other{fixture.context()};

    fixture.play().update(driver.pressedNow(Key::P), kFrame);
    CHECK(fixture.play().paused());
    CHECK_FALSE(other.paused());

    // And the frozen one really is frozen while the other runs, which is the
    // observation a `static bool s_paused` could not survive: the flag would be shared
    // and both scenes would report it.
    other.update(driver.held(), kFrame);
    CHECK(fixture.play().paused());
    CHECK_FALSE(other.paused());

    const Vec2 frozen = playerTransform(fixture.world()).position;
    for (int frame = 0; frame < 30; ++frame)
    {
        fixture.play().update(driver.held(), kFrame);
    }
    CHECK_NEAR(playerTransform(fixture.world()).position.x, frozen.x, 0.0001F);
    CHECK_NEAR(playerTransform(fixture.world()).position.y, frozen.y, 0.0001F);
}

void testADebugControlStillWorksInAPausedGame()
{
    ActionDriver driver;
    DebugFixture fixture;
    fixture.settle(driver);

    fixture.update(driver.pressedNow(Key::P));
    CHECK(fixture.play().paused());

    // Three of the four controls, all pressed and all released while the game is
    // stopped. A debugging control that only worked while running would be barely a
    // debugging control - and "pause, then look at the collision boxes" is the exact
    // sequence this exists for.
    fixture.update(driver.pressedNow(Key::C));
    fixture.renderFrame();
    CHECK(fixture.renderer().draws().size() == colliderCount(fixture.world()));
    CHECK(fixture.renderer().draws().size() > 0U);

    // The boxes off and the grid on: 21 vertical lines and 12 horizontal ones at the
    // game's own viewport, which is the same arithmetic the grid group writes out.
    //
    // The camera is placed deliberately, because `CameraSystem` followed the player
    // during the settle and a grid drawn around wherever the player happens to be
    // would make the line count a function of the fixture rather than of the overlay.
    fixture.update(driver.released(Key::C));
    fixture.update(driver.pressedNow(Key::G));
    fixture.camera().setPosition(Vec2{0.0F, 0.0F});
    fixture.camera().setZoom(1.0F);
    fixture.renderFrame();
    CHECK(gridLinesOf(fixture).vertical.size() == 21U);
    CHECK(gridLinesOf(fixture).horizontal.size() == 12U);

    fixture.update(driver.released(Key::G));
    fixture.update(driver.pressedNow(Key::T));
    fixture.renderFrame();
    CHECK(fixture.renderer().textureDraws().empty());

    // Still paused through all of it, and still frozen: none of the three is a way to
    // resume the game by accident.
    CHECK(fixture.play().paused());
    const Vec2 frozen = playerTransform(fixture.world()).position;
    fixture.run(driver.held(), 30);
    CHECK(fixture.play().paused());
    CHECK_NEAR(playerTransform(fixture.world()).position.y, frozen.y, 0.0001F);
}

// ===========================================================================
// B. Textures
// ===========================================================================

void testTRemovesEveryTextureDrawAndPutsThemBack()
{
    ActionDriver driver;
    DebugFixture fixture;
    fixture.settle(driver);

    // The game's own look, first: every animated entity in the level - the player,
    // every tile, every decoration - is an image, so this is not a small number.
    fixture.renderFrame();
    const std::size_t before = fixture.renderer().textureDraws().size();
    const std::size_t textBefore = fixture.renderer().textDraws().size();
    CHECK(before > 20U);
    CHECK(textBefore == 1U);

    // `T` off. Not "the flag changed": the renderer received no image at all.
    fixture.update(driver.pressedNow(Key::T));
    CHECK_FALSE(fixture.play().debugRenderState().showTextures);
    fixture.renderFrame();
    CHECK(fixture.renderer().textureDraws().empty());

    // And the level's own spawn label is still there, because a label is not an
    // entity's texture. Hiding every image in the game should not hide the one piece
    // of text that says where the level put the player - that is precisely when it is
    // useful.
    CHECK(fixture.renderer().textDraws().size() == textBefore);

    // `T` on again, and exactly as many draws as before: reversible, and neither
    // cumulatively applied nor cumulative in the other direction.
    fixture.update(driver.released(Key::T));
    fixture.update(driver.pressedNow(Key::T));
    CHECK(fixture.play().debugRenderState().showTextures);
    fixture.renderFrame();
    CHECK(fixture.renderer().textureDraws().size() == before);
}

void testHoldingTDoesNotFlickerTheTextures()
{
    ActionDriver driver;
    DebugFixture fixture;

    fixture.update(driver.pressedNow(Key::T));
    for (int frame = 0; frame < kPausedFrames; ++frame)
    {
        fixture.update(driver.held());
        fixture.renderFrame();
        CHECK_FALSE(fixture.play().debugRenderState().showTextures);
        CHECK(fixture.renderer().textureDraws().empty());
    }

    fixture.update(driver.released(Key::T));
    fixture.update(driver.pressedNow(Key::T));
    fixture.renderFrame();
    CHECK(fixture.play().debugRenderState().showTextures);
    CHECK(!fixture.renderer().textureDraws().empty());
}

void testHidingTexturesDoesNotStopTheAnimationClock()
{
    ActionDriver driver;
    DebugFixture fixture;
    fixture.settle(driver);

    // A real animation with real length: break the brick at cell (8, 1) and watch the
    // explosion strip advance. Twelve frames at its declared speed of eight, so the
    // frame index is observable rather than a one-frame placeholder.
    fixture.update(driver.pressedNow(Key::Space));
    const int untilExploding =
        fixture.runUntil(driver.held(), [&fixture] { return explodingBrick(fixture.world()) != nullptr; }, 300);
    CHECK(untilExploding < 300);
    if (explodingBrick(fixture.world()) == nullptr)
    {
        return;
    }

    const std::uint32_t startedAt = explodingBrick(fixture.world())->getComponent<Animation>().currentFrame;

    // Textures off, then sixteen frames: two full animation steps. Rendered once first,
    // because the claim is about what the renderer received rather than about a flag.
    fixture.update(driver.released(Key::Space));
    fixture.update(driver.pressedNow(Key::T));
    fixture.renderFrame();
    CHECK_FALSE(fixture.play().debugRenderState().showTextures);
    CHECK(fixture.renderer().textureDraws().empty());
    fixture.run(driver.held(), 16);

    const Animation& advanced = explodingBrick(fixture.world())->getComponent<Animation>();
    CHECK(advanced.currentFrame == startedAt + 2U);

    // Textures back on, and the frame that is submitted is the frame the animation has
    // actually reached - not a reset to zero, and not the frame it started at. This is
    // the whole of "the animation state keeps advancing while the artwork is hidden":
    // the render pass reads a state it did not write.
    fixture.update(driver.released(Key::T));
    fixture.update(driver.pressedNow(Key::T));
    fixture.renderFrame();

    const engine::assets::Animation& definition =
        fixture.assets().animation("animations_explosion_burst");
    const IntRect expected = definition.frameRect(advanced.currentFrame);

    bool sawTheFrame = false;
    for (const TextureDrawCall& call : fixture.renderer().textureDraws())
    {
        if (call.source.has_value() && call.source->left == expected.left && call.source->top == expected.top &&
            call.source->width == expected.width && call.source->height == expected.height)
        {
            sawTheFrame = true;
        }
    }
    CHECK(sawTheFrame);
}

void testTexturesOffDoesNotChangeTheGameAtAll()
{
    // Two identical levels, one of them drawing no artwork, run side by side for three
    // seconds with the same keys. If the toggle touched anything but the render pass,
    // this is the group that would notice - and it is a stronger claim than "the
    // player did not move", because it covers every system at once.
    ActionDriver first;
    ActionDriver second;
    DebugFixture withTextures;
    DebugFixture withoutTextures;

    for (const Key key : {Key::D, Key::W, Key::Space})
    {
        withTextures.update(first.pressedNow(key));
        withoutTextures.update(second.pressedNow(key));
    }

    const int frames = 180;
    withTextures.run(first.held(), frames);
    withoutTextures.run(second.held(), frames);

    const Transform& a = playerTransform(withTextures.world());
    const Transform& b = playerTransform(withoutTextures.world());

    CHECK_NEAR(a.position.x, b.position.x, 0.0001F);
    CHECK_NEAR(a.position.y, b.position.y, 0.0001F);
    CHECK_NEAR(a.velocity.x, b.velocity.x, 0.0001F);
    CHECK_NEAR(a.velocity.y, b.velocity.y, 0.0001F);
    CHECK(bulletCount(withTextures.world()) == bulletCount(withoutTextures.world()));
    CHECK(colliderCount(withTextures.world()) == colliderCount(withoutTextures.world()));
}

// ===========================================================================
// C. Bounding boxes
// ===========================================================================

void testCDrawsOneBoxPerColliderAtTheCollidersOwnSize()
{
    ActionDriver driver;
    DebugFixture fixture;

    // Off by default: the game is meant to look like the game.
    CHECK_FALSE(fixture.play().debugRenderState().showBoundingBoxes);
    fixture.renderFrame();
    CHECK(fixture.renderer().draws().empty());

    fixture.update(driver.pressedNow(Key::C));
    CHECK(fixture.play().debugRenderState().showBoundingBoxes);
    fixture.renderFrame();

    const std::vector<DrawCall>& boxes = fixture.renderer().draws();

    // Exactly the colliders, and nothing else. The committed level puts no
    // `components::Rectangle` anywhere, so every rectangle the renderer received came
    // from the overlay - which is what lets this be an equality rather than a lower
    // bound.
    CHECK(boxes.size() == colliderCount(fixture.world()));
    CHECK(boxes.size() > 20U);

    // Every box is a collider's own size at a collider's own world position. Checked
    // through the camera, so this is a statement about the geometry the overlay drew,
    // not about the numbers it happened to be handed. The positions are compared with
    // a tolerance because the round trip is `worldToScreen` and then `screenToWorld`,
    // which is not the identity in floating point.
    std::size_t matched = 0U;
    for (const DrawCall& box : boxes)
    {
        const Vec2 world = fixture.camera().screenToWorld(box.placement.position);
        for (const Entity& entity : fixture.world().getEntities())
        {
            if (!entity.hasComponent<Transform>() || !entity.hasComponent<Collider>())
            {
                continue;
            }

            const Transform& transform = entity.getComponent<Transform>();
            const Collider& collider = entity.getComponent<Collider>();
            if (collider.size.x == box.size.x && collider.size.y == box.size.y &&
                std::fabs(transform.position.x - world.x) < 0.01F &&
                std::fabs(transform.position.y - world.y) < 0.01F)
            {
                ++matched;
                break;
            }
        }
    }
    CHECK(matched == boxes.size());

    // And the sizes are the *level's*, not a constant the overlay chose. The
    // committed level carries a pipe whose artwork is 70x70 and a player whose box is
    // 40x60, so a hard-coded 64 would produce neither.
    const std::vector<Vec2> sizes = colliderSizes(fixture.world());
    bool sawThePipe = false;
    bool sawThePlayer = false;
    bool sawTheTile = false;
    for (const Vec2& size : sizes)
    {
        sawThePipe = sawThePipe || (size.x == 70.0F && size.y == 70.0F);
        sawThePlayer = sawThePlayer || (size.x == 40.0F && size.y == 60.0F);
        sawTheTile = sawTheTile || (size.x == kCell && size.y == kCell);
    }
    CHECK(sawThePipe);
    CHECK(sawThePlayer);
    CHECK(sawTheTile);

    // Off again, and the boxes go: the toggle is reversible, not a one-way switch.
    fixture.update(driver.released(Key::C));
    fixture.update(driver.pressedNow(Key::C));
    fixture.renderFrame();
    CHECK(fixture.renderer().draws().empty());
}

void testAnEntityWithNoColliderGetsNoBox()
{
    ActionDriver driver;
    DebugFixture fixture;

    // Added through the scene's own mutable world accessor, because spawning into a
    // level is a thing a game does and the scene is where it happens. It carries a
    // transform and an animation, so the renderer *does* submit an image for it - and
    // no collider, so the overlay must not invent one.
    Entity& ghost = fixture.world().addEntity("debug.no.collider");
    ghost.addComponent<Transform>(Transform{Vec2{-4096.0F, -4096.0F}, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
    Animation animation;
    animation.assetName = "mario_ground_tile";
    animation.repeat = true;
    ghost.addComponent<Animation>(animation);

    fixture.update(driver.pressedNow(Key::C));
    fixture.renderFrame();

    // It is in the world and being drawn...
    CHECK(colliderCount(fixture.world()) >= 20U);
    bool drewIt = false;
    for (const TextureDrawCall& call : fixture.renderer().textureDraws())
    {
        const Vec2 world = fixture.camera().screenToWorld(call.placement.position);
        if (world.x == -4096.0F && world.y == -4096.0F)
        {
            drewIt = true;
        }
    }
    CHECK(drewIt);

    // ...and it produced no box. The count is the assertion that matters: a fake box
    // would make the overlay's output disagree with the world's geometry, and a fake
    // box at a default size is exactly what "draw a box for everything" produces.
    CHECK(fixture.renderer().draws().size() == colliderCount(fixture.world()));
}

void testADestroyedBricksBoxDisappearsWithItsCollider()
{
    ActionDriver driver;
    DebugFixture fixture;
    fixture.settle(driver);

    fixture.update(driver.pressedNow(Key::C));
    fixture.renderFrame();
    const std::size_t withTheBrick = fixture.renderer().draws().size();
    CHECK(withTheBrick == colliderCount(fixture.world()));

    // The centre of the committed level's brick at cell (8, 1): `8 * 64 + 32` across
    // and `1024 - 64 - 32` up from the top of a sixteen-cell world.
    const Vec2 brickCentre{544.0F, 928.0F};

    // Break it with a shot, and check the box for the brick's own cell is gone from the
    // very next frame - because "no collider" is the whole of what makes it no longer
    // solid, and the overlay reads the same component physics does rather than keeping
    // its own list. `C` is released, not pressed again: the boxes stay on throughout,
    // which is the whole point of measuring a disappearance.
    fixture.update(driver.released(Key::C));
    fixture.update(driver.pressedNow(Key::Space));
    const int frames = fixture.runUntil(driver.held(), [&fixture] { return explodingBrick(fixture.world()) != nullptr; }, 300);
    CHECK(frames < 300);

    fixture.renderFrame();
    CHECK(fixture.renderer().draws().size() + 1U == withTheBrick);
    CHECK(fixture.renderer().draws().size() == colliderCount(fixture.world()));

    bool stillBoxed = false;
    for (const DrawCall& box : fixture.renderer().draws())
    {
        const Vec2 world = fixture.camera().screenToWorld(box.placement.position);
        if (std::fabs(world.x - brickCentre.x) < 0.01F && std::fabs(world.y - brickCentre.y) < 0.01F)
        {
            stillBoxed = true;
        }
    }
    CHECK_FALSE(stillBoxed);
}

// ===========================================================================
// D. The grid
// ===========================================================================

void testGDrawsTheLevelGridSnappedToCellBoundaries()
{
    ActionDriver driver;
    DebugFixture fixture;

    // Off by default, like every other overlay.
    CHECK_FALSE(fixture.play().debugRenderState().showGrid);
    fixture.renderFrame();
    CHECK(fixture.renderer().draws().empty());

    // Pressed **before** the camera is placed, so that no simulation runs in between
    // and `CameraSystem` cannot move the view out from under the arithmetic below.
    // At the world origin at 1:1, `screenToWorld(0)` is `-screenCentre`, which is
    // `-640, -360`; the visible world rectangle is therefore `x in [-640, 640]` and
    // `y in [-360, 360]`.
    fixture.update(driver.pressedNow(Key::G));
    CHECK(fixture.play().debugRenderState().showGrid);

    fixture.camera().setPosition(Vec2{0.0F, 0.0F});
    fixture.camera().setZoom(1.0F);
    fixture.renderFrame();

    const GridLines lines = gridLinesOf(fixture);

    // 1280 / 64 = 20 cells across, so 21 boundaries from -640 to 640 inclusive.
    // 720 / 64 = 11.25 cells down, so the first boundary is the one at or below -360
    // (-384) and the last at or above 360 (320): 12 of them.
    CHECK(lines.vertical.size() == 21U);
    CHECK(lines.horizontal.size() == 12U);

    // Each one is an exact cell boundary in **world** space, which is the claim that
    // there is one grid rather than two nearby ones.
    for (const DrawCall& line : lines.vertical)
    {
        const Vec2 world = fixture.camera().screenToWorld(line.placement.position);
        CHECK_NEAR(std::fmod(world.x, kCell), 0.0F, 0.001F);
        // Each line spans the visible height plus a cell at each end, so the grid has
        // no gap at the edge of the view.
        CHECK_NEAR(line.size.y, kWindowHeight + (2.0F * kCell), 0.001F);
        CHECK_NEAR(line.placement.scale.x, 1.0F, 0.0001F);
    }

    for (const DrawCall& line : lines.horizontal)
    {
        const Vec2 world = fixture.camera().screenToWorld(line.placement.position);
        CHECK_NEAR(std::fmod(world.y, kCell), 0.0F, 0.001F);
        CHECK_NEAR(line.size.x, kWindowWidth + (2.0F * kCell), 0.001F);
    }

    // The spacing is the cell size, measured on screen, so "a grid of 64-pixel cells"
    // is a fact about the picture and not only about the arithmetic.
    for (std::size_t index = 1U; index < lines.vertical.size(); ++index)
    {
        CHECK_NEAR(lines.vertical[index].placement.position.x - lines.vertical[index - 1U].placement.position.x,
                   kCell, 0.001F);
    }

    // Off again.
    fixture.update(driver.released(Key::G));
    fixture.update(driver.pressedNow(Key::G));
    fixture.renderFrame();
    CHECK(fixture.renderer().draws().empty());
}

/// The screen position of the grid line at world `worldX`, if it is on screen.
///
/// Comparing the *same world line* in two views is the only comparison that says
/// anything about the camera. Comparing the first line in each view compares two
/// different lines, and because the grid is periodic and the move below is a whole
/// number of cells, that comparison produces two identical screen positions and reads
/// as "the grid did not move" - which is what the first version of this group
/// concluded, and was wrong about the code and right about the test.
[[nodiscard]] bool screenXOfWorldLine(const Camera& view, const GridLines& lines, const float worldX,
                                      float& screenX)
{
    for (const DrawCall& line : lines.vertical)
    {
        const Vec2 world = view.screenToWorld(line.placement.position);
        if (std::fabs(world.x - worldX) < 0.01F)
        {
            screenX = line.placement.position.x;
            return true;
        }
    }
    return false;
}

/// The camera the game opens with, as a value a group can read world positions back
/// through after the live camera has moved on.
[[nodiscard]] Camera gameView()
{
    Camera camera;
    camera.setViewport(Vec2{kWindowWidth, kWindowHeight});
    camera.setZoom(1.0F);
    return camera;
}

void testTheGridFollowsTheCamera()
{
    ActionDriver driver;
    DebugFixture fixture;

    fixture.update(driver.pressedNow(Key::G));

    // At the origin: 21 vertical boundaries, from -640 to 640.
    Camera originView = gameView();
    fixture.camera().setPosition(originView.position());
    fixture.camera().setZoom(originView.zoom());
    fixture.renderFrame();
    const GridLines atOrigin = gridLinesOf(fixture);
    CHECK(atOrigin.vertical.size() == 21U);

    // Move the camera 320 world pixels right - five whole cells.
    Camera movedView = originView;
    movedView.setPosition(Vec2{320.0F, 0.0F});
    fixture.camera().setPosition(movedView.position());
    fixture.renderFrame();
    const GridLines moved = gridLinesOf(fixture);

    CHECK(moved.vertical.size() == atOrigin.vertical.size());
    CHECK(moved.horizontal.size() == atOrigin.horizontal.size());

    // Every line is still a cell boundary, in both views.
    for (const DrawCall& line : moved.vertical)
    {
        const Vec2 world = movedView.screenToWorld(line.placement.position);
        CHECK_NEAR(std::fmod(world.x, kCell), 0.0F, 0.001F);
    }
    for (const DrawCall& line : moved.horizontal)
    {
        const Vec2 world = movedView.screenToWorld(line.placement.position);
        CHECK_NEAR(std::fmod(world.y, kCell), 0.0F, 0.001F);
    }

    // And the line the world says is at x = 0 - visible in both views, since the first
// sees `x in [-640, 640]` and the second `x in [-320, 960]` - has moved **left** on
    // screen by exactly the camera's 320 pixels. That is the same shift every other
    // renderable in the engine gets, and the opposite of what a screen-space overlay
    // would do.
    float originX = 0.0F;
    float movedX = 0.0F;
    CHECK(screenXOfWorldLine(originView, atOrigin, 0.0F, originX));
    CHECK(screenXOfWorldLine(movedView, moved, 0.0F, movedX));
    CHECK_NEAR(movedX, originX - 320.0F, 0.001F);

    // The horizontal lines did not move at all, because the camera did not move
    // vertically. A grid that slid along with the camera on one axis only would be a
    // second coordinate system wearing a disguise.
    CHECK_NEAR(moved.horizontal.front().placement.position.y, atOrigin.horizontal.front().placement.position.y,
               0.001F);

    // And a diagonal move moves both axes, which is the case a screen-space overlay
    // gets right in one direction only.
    fixture.camera().setPosition(Vec2{-1280.0F, 640.0F});
    fixture.renderFrame();
    const GridLines diagonal = gridLinesOf(fixture);
    for (const DrawCall& line : diagonal.vertical)
    {
        const Vec2 world = fixture.camera().screenToWorld(line.placement.position);
        CHECK_NEAR(std::fmod(world.x, kCell), 0.0F, 0.001F);
    }
    for (const DrawCall& line : diagonal.horizontal)
    {
        const Vec2 world = fixture.camera().screenToWorld(line.placement.position);
        CHECK_NEAR(std::fmod(world.y, kCell), 0.0F, 0.001F);
    }
}

void testTheGridScalesWithTheZoom()
{
    ActionDriver driver;
    DebugFixture fixture;

    fixture.update(driver.pressedNow(Key::G));

    // Zoomed in to two. The visible world rectangle halves, so half as many cell
    // boundaries are on screen: `x in [-320, 320]` is 640 pixels, which is ten cells
    // and therefore eleven lines.
    fixture.camera().setPosition(Vec2{0.0F, 0.0F});
    fixture.camera().setZoom(2.0F);
    fixture.renderFrame();

    const GridLines zoomed = gridLinesOf(fixture);
    CHECK(zoomed.vertical.size() == 11U);

    // Twice as far apart on screen, because the grid is world space and the world is
    // magnified. The line's own thickness is still one **world** pixel - which is
    // exactly the distinction a screen-space grid gets wrong, since it would have to
    // undo the camera before choosing a size.
    CHECK_NEAR(zoomed.vertical[1].placement.position.x - zoomed.vertical[0].placement.position.x, 2.0F * kCell, 0.001F);
    CHECK_NEAR(zoomed.vertical[0].size.x, kGridLineThickness, 0.001F);
    CHECK_NEAR(zoomed.vertical[0].placement.scale.x, 2.0F, 0.0001F);
    CHECK_NEAR(zoomed.vertical[0].size.x * zoomed.vertical[0].placement.scale.x, 2.0F * kGridLineThickness, 0.001F);

    // And every line is still a cell boundary in world space, which is the whole claim:
    // magnifying the view magnified the *grid*, it did not invent a finer one.
    for (const DrawCall& line : zoomed.vertical)
    {
        const Vec2 world = fixture.camera().screenToWorld(line.placement.position);
        CHECK_NEAR(std::fmod(world.x, kCell), 0.0F, 0.001F);
    }

    // Zoomed out below 1: still the same grid, fewer lines on screen.
    fixture.camera().setZoom(0.5F);
    fixture.renderFrame();
    const GridLines out = gridLinesOf(fixture);
    for (const DrawCall& line : out.vertical)
    {
        const Vec2 world = fixture.camera().screenToWorld(line.placement.position);
        CHECK_NEAR(std::fmod(world.x, kCell), 0.0F, 0.001F);
    }
    CHECK(out.vertical.size() > zoomed.vertical.size());
}

void testTheOverlaysChangeNothingAboutTheGame()
{
    // Every overlay on, run against every overlay off, with the same keys and the same
    // number of frames. The grid and the boxes are pictures; if either of them were
    // anything else - a collider invented from a grid line, an entity added to the
    // world so the grid could be drawn - this is the group that would say so.
    ActionDriver withDriver;
    ActionDriver withoutDriver;
    DebugFixture withOverlays;
    DebugFixture withoutOverlays;

    for (const Key key : {Key::C, Key::G, Key::T, Key::D, Key::Space, Key::W})
    {
        withOverlays.update(withDriver.pressedNow(key));
        withoutOverlays.update(withoutDriver.pressedNow(key));
    }

    const int frames = 240;
    withOverlays.run(withDriver.held(), frames);
    withoutOverlays.run(withoutDriver.held(), frames);

    const Transform& a = playerTransform(withOverlays.world());
    const Transform& b = playerTransform(withoutOverlays.world());

    CHECK_NEAR(a.position.x, b.position.x, 0.0001F);
    CHECK_NEAR(a.position.y, b.position.y, 0.0001F);
    CHECK_NEAR(a.velocity.x, b.velocity.x, 0.0001F);
    CHECK_NEAR(a.velocity.y, b.velocity.y, 0.0001F);
    CHECK(bulletCount(withOverlays.world()) == bulletCount(withoutOverlays.world()));
    CHECK(colliderCount(withOverlays.world()) == colliderCount(withoutOverlays.world()));

    // The grid is not drawn by putting entities into the level: the two worlds hold
    // the same number of things, one of which is showing a grid.
    CHECK(withOverlays.world().aliveEntityCount() == withoutOverlays.world().aliveEntityCount());
}

// ===========================================================================
// E. Escape
// ===========================================================================

void testEscapeInALevelAsksForTheMenu()
{
    ActionDriver driver;
    DebugFixture fixture;

    CHECK_FALSE(fixture.play().pendingTransition().scene().has_value());

    fixture.update(driver.pressedNow(Key::Escape));

    CHECK(fixture.play().pendingTransition().scene().has_value());
    CHECK(fixture.play().pendingTransition().scene().value() == SceneId::Menu);
    CHECK_FALSE(fixture.play().pendingTransition().quits());

    // And it is a **request**, not an act: the scene is still the play scene and still
    // in one piece, because the owner applies a transition at a frame boundary and
    // never from inside a scene's own update.
    CHECK(fixture.play().id() == SceneId::Play);
}

void testEscapeWorksInAPausedLevel()
{
    ActionDriver driver;
    DebugFixture fixture;

    fixture.update(driver.pressedNow(Key::P));
    CHECK(fixture.play().paused());

    // "Go back" has to work in a stopped game. A player who pauses to look at
    // something and then wants out would otherwise have to unpause first, and the one
    // control that must never be swallowed by the pause is the one that leaves.
    fixture.update(driver.released(Key::P));
    fixture.update(driver.pressedNow(Key::Escape));

    CHECK(fixture.play().pendingTransition().scene().has_value());
    CHECK(fixture.play().pendingTransition().scene().value() == SceneId::Menu);
}

void testEscapeOnTheMainMenuAsksTheApplicationToQuit()
{
    ActionDriver driver;
    DebugFixture fixture;
    MenuScene menu{fixture.context()};

    // Nothing asked for on a menu nobody has touched.
    CHECK_FALSE(menu.pendingTransition().quits());
    CHECK_FALSE(menu.pendingTransition().scene().has_value());

    // The course's sentence: "go 'back' to the Main Menu, or quit if on the Main
    // Menu". The menu has nowhere to go back to, so the same action means "leave".
    menu.update(driver.pressedNow(Key::Escape), kFrame);

    CHECK(menu.pendingTransition().quits());
    CHECK_FALSE(menu.pendingTransition().scene().has_value());

    // And it is a quit rather than a scene change, so the application stops instead of
    // building a menu that is already there.
    MenuScene other{fixture.context()};
    other.update(driver.held(), kFrame);
    other.update(driver.released(Key::Escape), kFrame);
    other.update(driver.pressedNow(Key::Escape), kFrame);
    CHECK(other.pendingTransition().quits());
}

// ===========================================================================
// F. Scene lifetime
// ===========================================================================

void testANewLevelStartsUnpausedWithTheGamesOwnLook()
{
    ActionDriver driver;
    DebugFixture fixture;

    // Turn everything on in this level, and pause it.
    for (const Key key : {Key::T, Key::C, Key::G, Key::P})
    {
        fixture.update(driver.pressedNow(key));
        fixture.update(driver.released(key));
    }
    CHECK(fixture.play().paused());
    CHECK_FALSE(fixture.play().debugRenderState().showTextures);
    CHECK(fixture.play().debugRenderState().showBoundingBoxes);
    CHECK(fixture.play().debugRenderState().showGrid);

    // A second level, built the way returning to the menu and starting again builds
    // one: a new object over the same shared infrastructure. [engine::Application]
    // destroys the old scene on every transition, so "the flags reset" is not a rule
    // anybody implemented - it is what a member of a destroyed object does.
    PlayScene fresh{fixture.context()};

    CHECK_FALSE(fresh.paused());
    CHECK(fresh.debugRenderState().showTextures);
    CHECK_FALSE(fresh.debugRenderState().showBoundingBoxes);
    CHECK_FALSE(fresh.debugRenderState().showGrid);

    // And the old one is still paused, which is the other half of the claim: nothing
    // reset it, and nothing needed to.
    CHECK(fixture.play().paused());

    // The fresh level draws the game, not the overlays.
    fixture.renderer().beginFrame();
    fresh.render();
    fixture.renderer().endFrame();
    CHECK(!fixture.renderer().textureDraws().empty());
    CHECK(fixture.renderer().draws().empty());
}

void testTheDebugStateIsNotSharedBetweenTwoScenes()
{
    ActionDriver driver;
    DebugFixture fixture;

    PlayScene other{fixture.context()};

    fixture.play().update(driver.pressedNow(Key::C), kFrame);
    fixture.play().update(driver.released(Key::C), kFrame);

    CHECK(fixture.play().debugRenderState().showBoundingBoxes);
    CHECK_FALSE(other.debugRenderState().showBoundingBoxes);

    // Both scenes' render systems are independent objects, so the two answers cannot
    // be one shared value that happens to agree. A `static DebugRenderState` would make
    // these two lines equal and this group would fail - which is the point of it.
    fixture.renderer().beginFrame();
    fixture.play().render();
    fixture.renderer().endFrame();
    const std::size_t firstBoxes = fixture.renderer().draws().size();
    CHECK(firstBoxes > 0U);

    fixture.renderer().beginFrame();
    other.render();
    fixture.renderer().endFrame();
    CHECK(fixture.renderer().draws().empty());
}

// ===========================================================================
// G. What cannot be observed at run time
// ===========================================================================

void testTheScenesAndTheDebugRendererNameNoKeyAndNoGraphicsType()
{
    // A scene holds no keyboard at all - it is handed an [engine::input::ActionState]
    // - and the debug renderer is handed neither, so nothing at run time can say
    // whether either went round the action layer. Hence the source.
    const char* const files[] = {ENGINE_PLAY_SCENE_HEADER, ENGINE_PLAY_SCENE_SOURCE, ENGINE_MENU_SCENE_SOURCE,
                                ENGINE_DEBUG_RENDER_SYSTEM_HEADER, ENGINE_DEBUG_RENDER_SYSTEM_SOURCE};

    for (const char* const path : files)
    {
        const std::string code = codeWithoutComments(path);
        CHECK_FALSE(code.empty());

        CHECK_FALSE(has(code, "sf::Keyboard"));
        CHECK_FALSE(has(code, "Keyboard::"));
        CHECK_FALSE(has(code, "isKeyPressed"));
        CHECK_FALSE(has(code, "input::Input"));
        CHECK_FALSE(has(code, "sf::"));
        CHECK_FALSE(has(code, "<SFML/"));
    }

    // And the five controls are read as the actions the course's keys are already
    // bound to, rather than as five new names invented for the purpose.
    const std::string play = codeWithoutComments(ENGINE_PLAY_SCENE_SOURCE);
    CHECK(has(play, "input::Action::Pause"));
    CHECK(has(play, "input::Action::ToggleTextures"));
    CHECK(has(play, "input::Action::ToggleBoundingBoxes"));
    CHECK(has(play, "input::Action::ToggleGrid"));
    CHECK(has(play, "input::Action::Quit"));

    const std::string menu = codeWithoutComments(ENGINE_MENU_SCENE_SOURCE);
    CHECK(has(menu, "input::Action::Quit"));

    // Every one of them on the press edge. A level-triggered toggle is the single most
    // likely mistake in this phase and the least visible one at run time.
    //
    // **Seven**, not five: the file's local `ZoomKeysSystem` reads two more, and
    // leaving them out of the count would make this group wrong the moment somebody
    // added a sixth control to the same function. What is being asserted is that
    // *every* action the scene reads is read through the press edge.
    CHECK(countOf(play, "wasPressed(") == 7U);
    CHECK(countOf(menu, "wasPressed(") == 4U);
}

void testNoGameplaySystemKnowsAboutPauseOrADebugFlag()
{
    // Pause is one branch in the scene, not a flag in six systems. The behavioural
    // groups prove that pausing stops the world; this one proves that the world did
    // not stop *because six systems asked*, which would mean every future system would
    // have to ask too.
    const char* const systems[] = {
        ENGINE_PLAYER_SYSTEM_SOURCE, ENGINE_PHYSICS_SYSTEM_SOURCE, ENGINE_LIFETIME_SYSTEM_SOURCE,
        ENGINE_ANIMATION_SYSTEM_SOURCE, ENGINE_TILE_SYSTEM_SOURCE, ENGINE_SHOOT_SYSTEM_SOURCE};

    for (const char* const path : systems)
    {
        const std::string code = codeWithoutComments(path);
        CHECK_FALSE(code.empty());

        for (const std::string_view word : {std::string_view{"paused"}, std::string_view{"DebugRenderState"},
                                           std::string_view{"showTextures"}, std::string_view{"showBoundingBoxes"},
                                           std::string_view{"showGrid"}, std::string_view{"debugRenderState"}})
        {
            if (has(code, word))
            {
                std::cerr << "    " << path << " mentions " << word << '\n';
            }
            CHECK_FALSE(has(code, word));
        }
    }

    // And the render system's own copy is read in exactly one place per query - the
    // two image queries - which is what makes "T hides the player's sprites too" a
    // property of the pass rather than of luck.
    const std::string render = codeWithoutComments(ENGINE_RENDER_SYSTEM_SOURCE);
    CHECK(countOf(render, "m_debug.showTextures") == 2U);
}

void testThePauseFlagIsReadInExactlyOnePlace()
{
    // The count is the architecture. `m_paused` appears once to toggle it and once to
    // test it: a fourth appearance would be a second gate, and a scene with two gates
    // has an order between them that nobody chose.
    const std::string play = codeWithoutComments(ENGINE_PLAY_SCENE_SOURCE);
    CHECK(countOf(play, "m_paused") == 3U);

    // And the member is declared once, in the scene, rather than in the base class a
    // menu or a future scene would inherit it into. Two mentions in the header: the
    // declaration and the accessor that reports it.
    const std::string playHeader = codeWithoutComments(ENGINE_PLAY_SCENE_HEADER);
    CHECK(countOf(playHeader, "m_paused") == 2U);
    CHECK(has(playHeader, "bool m_paused = false"));
}

void testTheDebugRenderPassIsNotASimulationSystem()
{
    DebugFixture fixture;

    // The overlay pass is driven by the scene's render, not by the system list, for
    // the reason the ordinary render pass is not in it either: a render pass has to be
    // bracketed by beginFrame/endFrame and has to run after every simulation system.
    for (std::size_t index = 0U; index < fixture.play().systems().systemCount(); ++index)
    {
        CHECK(std::string{fixture.play().systems().systemAt(index).name()} != "DebugRenderSystem");
    }
    CHECK(fixture.play().systems().systemCount() == 9U);

    const std::string play = codeWithoutComments(ENGINE_PLAY_SCENE_SOURCE);
    CHECK_FALSE(has(play, "m_systems.add<engine::systems::DebugRenderSystem>"));

    // And the overlay pass runs *after* the ordinary one, which is the difference
    // between an overlay and something hidden underneath the sprites.
    const std::size_t renderCall = play.find("m_renderSystem.update(");
    const std::size_t debugCall = play.find("m_debugRenderSystem.update(");
    CHECK(renderCall != std::string::npos);
    CHECK(debugCall != std::string::npos);
    CHECK(debugCall > renderCall);

    // Two update calls in the frame: the simulation list and the deferred-destruction
    // flush. A third would be a system list that pause forgot about.
    CHECK(countOf(play, "m_systems.update(") == 1U);
    CHECK(countOf(play, "m_world.update()") == 1U);

    // And the pause gate is between the debug controls and the simulation call, so a
    // paused frame still reads its controls and still draws.
    const std::size_t pauseToggle = play.find("m_paused = !m_paused;");
    const std::size_t gate = play.find("if (m_paused)");
    CHECK(pauseToggle != std::string::npos);
    CHECK(gate != std::string::npos);
    CHECK(pauseToggle < gate);
    CHECK(gate < play.find("m_systems.update("));
}

void testTheDebugLayerIsThreeBoolsAndNoStaticStorage()
{
    // Three flags, an aggregate, and nothing else. Checked by the compiler rather than
    // by a comment: a fourth field is a decision somebody made, and this is where it
    // would be noticed.
    static_assert(std::is_aggregate_v<DebugRenderState>, "the debug render state must be an aggregate of flags");
    static_assert(std::is_trivially_copyable_v<DebugRenderState>, "three bools are freely copyable");
    static_assert(std::is_standard_layout_v<DebugRenderState>, "and it is a plain struct");
    static_assert(std::is_empty_v<DebugRenderState> == false, "and it carries the three flags");

    // The defaults are the game's own look: textures on, no overlays. A build that has
    // just started must look like the game.
    const DebugRenderState fresh;
    CHECK(fresh.showTextures);
    CHECK_FALSE(fresh.showBoundingBoxes);
    CHECK_FALSE(fresh.showGrid);

    // No static storage anywhere in the debug layer, and none anywhere pause lives. A
    // `static bool s_paused` or a `static DebugRenderState s_state` would behave
    // identically in every test in this file and identically in the game - which is
    // exactly the property that makes it worth checking by reading.
    const char* const files[] = {ENGINE_DEBUG_STATE_HEADER, ENGINE_DEBUG_RENDER_SYSTEM_HEADER,
                                ENGINE_DEBUG_RENDER_SYSTEM_SOURCE, ENGINE_PLAY_SCENE_HEADER,
                                ENGINE_PLAY_SCENE_SOURCE, ENGINE_RENDER_SYSTEM_HEADER, ENGINE_RENDER_SYSTEM_SOURCE};

    for (const char* const path : files)
    {
        const std::string code = codeWithoutComments(path);
        CHECK_FALSE(code.empty());

        if (declaresStaticStorage(code))
        {
            std::cerr << "    " << path << " declares static storage\n";
        }
        CHECK_FALSE(declaresStaticStorage(code));
        CHECK_FALSE(has(code, "thread_local"));
    }

    // Pause is a member of the scene and not of anything below it.
    const std::string playHeader = codeWithoutComments(ENGINE_PLAY_SCENE_HEADER);
    CHECK(has(playHeader, "bool m_paused"));
}

void testTheDebugRenderStateIsAnAggregateOwnedByTheRenderSystem()
{
    // One object answers "are the overlays on this frame", so the ordinary render pass
    // and the overlay pass cannot disagree. That is a claim about the wiring, and the
    // wiring is what this reads.
    const std::string renderHeader = codeWithoutComments(ENGINE_RENDER_SYSTEM_HEADER);
    CHECK(has(renderHeader, "debug::DebugRenderState m_debug"));
    CHECK(has(renderHeader, "debugRenderState()"));

    const std::string debugHeader = codeWithoutComments(ENGINE_DEBUG_RENDER_SYSTEM_HEADER);
    CHECK(has(debugHeader, "const debug::DebugRenderState& m_state"));
    CHECK_FALSE(has(debugHeader, "debug::DebugRenderState m_state"));
    CHECK_FALSE(has(debugHeader, "debug::DebugRenderState m_debug"));

    // And the scene borrows it rather than holding a second copy.
    const std::string playHeader = codeWithoutComments(ENGINE_PLAY_SCENE_HEADER);
    CHECK_FALSE(has(playHeader, "DebugRenderState m_"));
    CHECK(has(playHeader, "m_renderSystem.debugRenderState()"));

    // Declared after the render system it borrows from, so the state exists before the
    // borrower is built and outlives it on the way out.
    const std::size_t renderSystem = playHeader.find("engine::systems::RenderSystem m_renderSystem;");
    const std::size_t debugSystem = playHeader.find("engine::systems::DebugRenderSystem m_debugRenderSystem;");
    CHECK(renderSystem != std::string::npos);
    CHECK(debugSystem != std::string::npos);
    CHECK(debugSystem > renderSystem);
}

} // namespace

int main()
{
    using TestCase = void (*)();
    const std::pair<const char*, TestCase> testCases[] = {
        // A. Pause
        {"P stops the player dead", &testPStopsThePlayerDead},
        {"holding P does not toggle pause every frame", &testHoldingPDoesNotTogglePauseEveryFrame},
        {"a paused game still draws the same picture", &testAPausedGameStillDrawsTheSamePicture},
        {"a bullet stops and keeps its lifetime and then resumes",
         &testABulletStopsAndKeepsItsLifetimeAndThenResumes},
        {"an explosion stops with the world and finishes when it resumes",
         &testAnExplosionStopsWithTheWorldAndFinishesWhenItResumes},
        {"pause is per scene and not shared", &testPauseIsPerSceneAndNotShared},
        {"a debug control still works in a paused game", &testADebugControlStillWorksInAPausedGame},
        // B. Textures
        {"T removes every texture draw and puts them back", &testTRemovesEveryTextureDrawAndPutsThemBack},
        {"holding T does not flicker the textures", &testHoldingTDoesNotFlickerTheTextures},
        {"hiding textures does not stop the animation clock",
         &testHidingTexturesDoesNotStopTheAnimationClock},
        {"textures off does not change the game at all", &testTexturesOffDoesNotChangeTheGameAtAll},
        // C. Bounding boxes
        {"C draws one box per collider at the collider's own size",
         &testCDrawsOneBoxPerColliderAtTheCollidersOwnSize},
        {"an entity with no collider gets no box", &testAnEntityWithNoColliderGetsNoBox},
        {"a destroyed brick's box disappears with its collider",
         &testADestroyedBricksBoxDisappearsWithItsCollider},
        // D. The grid
        {"G draws the level grid snapped to cell boundaries",
         &testGDrawsTheLevelGridSnappedToCellBoundaries},
        {"the grid follows the camera", &testTheGridFollowsTheCamera},
        {"the grid scales with the zoom", &testTheGridScalesWithTheZoom},
        {"the overlays change nothing about the game", &testTheOverlaysChangeNothingAboutTheGame},
        // E. Escape
        {"escape in a level asks for the menu", &testEscapeInALevelAsksForTheMenu},
        {"escape works in a paused level", &testEscapeWorksInAPausedLevel},
        {"escape on the main menu asks the application to quit",
         &testEscapeOnTheMainMenuAsksTheApplicationToQuit},
        // F. Scene lifetime
        {"a new level starts unpaused with the game's own look",
         &testANewLevelStartsUnpausedWithTheGamesOwnLook},
        {"the debug state is not shared between two scenes", &testTheDebugStateIsNotSharedBetweenTwoScenes},
        // G. Source checks
        {"the scenes and the debug renderer name no key and no graphics type",
         &testTheScenesAndTheDebugRendererNameNoKeyAndNoGraphicsType},
        {"no gameplay system knows about pause or a debug flag",
         &testNoGameplaySystemKnowsAboutPauseOrADebugFlag},
        {"the pause flag is read in exactly one place", &testThePauseFlagIsReadInExactlyOnePlace},
        {"the debug render pass is not a simulation system", &testTheDebugRenderPassIsNotASimulationSystem},
        {"the debug layer is three bools and no static storage",
         &testTheDebugLayerIsThreeBoolsAndNoStaticStorage},
        {"the debug render state is an aggregate owned by the render system",
         &testTheDebugRenderStateIsAnAggregateOwnedByTheRenderSystem},
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

    std::cout << groupCount << " debug test groups passed\n";
    return EXIT_SUCCESS;
}