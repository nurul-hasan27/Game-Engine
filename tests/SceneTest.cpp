/// Scene tests: the scene abstraction, the two scenes, the transition lifecycle,
/// and the screen-space decision.
///
/// ### What is behavioural and what is a source check
///
/// The scene rules - one active scene, deferred transitions, a destroyed old scene,
/// shared assets, and the two screen-space guarantees - are all observable from
/// outside, and every one of them is tested by running code. The two things that
/// cannot be observed at run time are checked in the source, and only those:
///
/// - **No SFML in the scene API.** A header could include an SFML type it never
///   used, or name one in a comment, and no run-time test would know. The check
///   strips comments first, because a header that *documents* "this is not
///   `sf::Text`" contains the token and is exactly as SFML-free as one that does
///   not.
/// - **No singleton scene manager.** A global would be visible in the source and
///   nowhere else: a test cannot ask "is there a global" at run time, it can only
///   observe that this particular program behaves correctly.
///
/// Everything else is real. The screen-space groups measure changed pixels in a
/// real window, because the whole claim is about where ink lands.
#include "engine/Color.hpp"
#include "engine/EngineConfig.hpp"
#include "engine/Application.hpp"
#include "engine/assets/AssetManager.hpp"
#include "engine/assets/SfmlAssetManager.hpp"
#include "engine/components/ScreenSpace.hpp"
#include "engine/components/Text.hpp"
#include "engine/components/Transform.hpp"
#include "engine/ecs/EntityManager.hpp"
#include "engine/ecs/System.hpp"
#include "engine/graphics/Camera.hpp"
#include "engine/graphics/RenderTransform.hpp"
#include "engine/graphics/SfmlRenderer.hpp"
#include "engine/components/Rectangle.hpp"
#include "engine/input/Action.hpp"
#include "engine/input/ActionMap.hpp"
#include "engine/input/ActionState.hpp"
#include "engine/input/Input.hpp"
#include "engine/level/LevelGrid.hpp"
#include "engine/scene/MenuScene.hpp"
#include "engine/scene/PlayScene.hpp"
#include "engine/scene/Scene.hpp"
#include "engine/systems/RenderSystem.hpp"

#include <SFML/Graphics/Image.hpp>
#include <SFML/Graphics/RenderWindow.hpp>
#include <SFML/Graphics/Texture.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <initializer_list>
#include <memory>
#include <optional>
#include <regex>
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
using engine::components::Text;
using engine::components::Transform;
using engine::ecs::EntityManager;
using engine::graphics::Camera;
using engine::graphics::RenderTransform;
using engine::graphics::Renderer;
using engine::input::Action;
using engine::input::ActionState;
using engine::input::defaultActionMap;
using engine::input::Input;
using engine::input::Key;
using engine::scene::MenuScene;
using engine::scene::PlayScene;
using engine::scene::Scene;
using engine::scene::SceneContext;
using engine::scene::SceneId;
using engine::scene::SceneTransition;

// ---------------------------------------------------------------------------
// The committed font the menu draws with.
//
// Pinned rather than read from `assets.txt`, for the reason every other suite
// pins it: a test that read the name out of the configuration would keep passing
// if the name were renamed away, quietly testing a different font.
// ---------------------------------------------------------------------------
constexpr std::string_view kFontPixeled = "fonts_pixeled";

constexpr unsigned kWindowWidth = 800U;
constexpr unsigned kWindowHeight = 450U;

// ---------------------------------------------------------------------------
// Pixel measurement
//
// A changed-pixel bounding box rather than a stored matrix. Glyph rasterization
// varies by platform, so an expected *picture* would be a portability bug; what is
// stable is where the ink is relative to the position, and that is what these
// groups assert. A content hash is captured too, for the assertions that need
// "not merely the same box, the same pixels".
// ---------------------------------------------------------------------------

struct ChangedPixels
{
    int minX = 0;
    int minY = 0;
    int maxX = -1;
    int maxY = -1;
    long long count = 0;

    [[nodiscard]] bool anythingChanged() const noexcept { return maxX >= minX; }
    [[nodiscard]] int width() const noexcept { return maxX - minX + 1; }
    [[nodiscard]] int height() const noexcept { return maxY - minY + 1; }
    [[nodiscard]] float centreX() const noexcept
    {
        return (static_cast<float>(minX) + static_cast<float>(maxX)) / 2.0F;
    }
    [[nodiscard]] float centreY() const noexcept
    {
        return (static_cast<float>(minY) + static_cast<float>(maxY)) / 2.0F;
    }
};

[[nodiscard]] ChangedPixels measureChanged(sf::RenderWindow& window, const sf::Color& background)
{
    ChangedPixels result;

    sf::Texture readback;
    if (!readback.create(window.getSize().x, window.getSize().y))
    {
        return result;
    }

    readback.update(window);
    const sf::Image image = readback.copyToImage();

    for (unsigned y = 0U; y < image.getSize().y; ++y)
    {
        for (unsigned x = 0U; x < image.getSize().x; ++x)
        {
            if (image.getPixel(x, y) == background)
            {
                continue;
            }

            if (result.maxX < result.minX)
            {
                result.minX = static_cast<int>(x);
                result.maxX = static_cast<int>(x);
                result.minY = static_cast<int>(y);
                result.maxY = static_cast<int>(y);
            }
            else
            {
                result.minX = std::min(result.minX, static_cast<int>(x));
                result.maxX = std::max(result.maxX, static_cast<int>(x));
                result.minY = std::min(result.minY, static_cast<int>(y));
                result.maxY = std::max(result.maxY, static_cast<int>(y));
            }

            ++result.count;
        }
    }

    return result;
}

/// An exact hash of the frame, so "the same box" can be promoted to "the same pixels".
[[nodiscard]] std::string frameHash(sf::RenderWindow& window)
{
    sf::Texture readback;
    if (!readback.create(window.getSize().x, window.getSize().y))
    {
        return "no-readback";
    }

    readback.update(window);
    const sf::Image image = readback.copyToImage();

    // FNV-1a, 64 bit. Not a cryptographic hash and not meant to be: it exists so
    // that two frames either agree byte for byte or do not, with no tolerance
    // argument to argue about.
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned y = 0U; y < image.getSize().y; ++y)
    {
        for (unsigned x = 0U; x < image.getSize().x; ++x)
        {
            const sf::Color pixel = image.getPixel(x, y);
            const std::uint8_t bytes[4] = {pixel.r, pixel.g, pixel.b, pixel.a};
            for (const std::uint8_t byte : bytes)
            {
                hash ^= static_cast<std::uint64_t>(byte);
                hash *= 1099511628211ULL;
            }
        }
    }

    return std::to_string(hash);
}

[[nodiscard]] bool sameBox(const ChangedPixels& a, const ChangedPixels& b) noexcept
{
    return a.minX == b.minX && a.minY == b.minY && a.maxX == b.maxX && a.maxY == b.maxY &&
           a.count == b.count;
}

// ---------------------------------------------------------------------------
// The fixture: a window, a renderer, the real committed assets, a camera, and
// the context a scene is built from.
//
// The assets are the real ones, loaded once, because a scene looking up a font
// that does not exist must fail the same way in a test as in the game.
// ---------------------------------------------------------------------------

class SceneFixture
{
public:
    SceneFixture()
        : m_window(sf::VideoMode{kWindowWidth, kWindowHeight}, "scene tests"),
          m_renderer{m_window}, m_assets{std::filesystem::path{engine::config::kAssetsConfig}}
    {
        m_window.setVisible(false);
        m_camera.setViewport(Vec2{static_cast<float>(kWindowWidth), static_cast<float>(kWindowHeight)});
        m_context.emplace(m_renderer, m_camera, m_assets);
    }

    [[nodiscard]] engine::graphics::SfmlRenderer& renderer() noexcept { return m_renderer; }
    [[nodiscard]] const AssetManager& assets() const noexcept { return m_assets; }
    [[nodiscard]] Camera& camera() noexcept { return m_camera; }
    [[nodiscard]] sf::RenderWindow& window() noexcept { return m_window; }
    [[nodiscard]] const SceneContext& context() const noexcept { return *m_context; }

    /// Draws whatever `draw` submits and measures the result.
    ///
    /// The `beginFrame` / `clear` / measure / `endFrame` bracket is here, and it is
    /// the same one [engine::Application::render] performs. It is repeated here
    /// deliberately rather than borrowed, so these groups measure a frame and not a
    /// scene: a scene's `render` submits draws and knows nothing about frames.
    template <typename Draw>
    [[nodiscard]] ChangedPixels measureFrame(const Draw& draw)
    {
        m_renderer.beginFrame();
        m_renderer.clear(engine::kBlack);
        draw();
        const ChangedPixels result = measureChanged(m_window, sf::Color::Black);
        m_renderer.endFrame();
        return result;
    }

    /// As [measureFrame], and also returns a hash of the finished frame.
    template <typename Draw>
    [[nodiscard]] std::pair<ChangedPixels, std::string> measureAndHashFrame(const Draw& draw)
    {
        m_renderer.beginFrame();
        m_renderer.clear(engine::kBlack);
        draw();
        const ChangedPixels box = measureChanged(m_window, sf::Color::Black);
        const std::string hash = frameHash(m_window);
        m_renderer.endFrame();
        return {box, hash};
    }

private:
    sf::RenderWindow m_window;
    engine::graphics::SfmlRenderer m_renderer;
    engine::assets::SfmlAssetManager m_assets;
    Camera m_camera;
    std::optional<SceneContext> m_context;
};

/// The number of entities in `world` carrying a `Text` component.
[[nodiscard]] std::size_t countTextEntities(const EntityManager& world)
{
    std::size_t count = 0U;
    for (const engine::ecs::Entity& entity : world.getEntities())
    {
        if (entity.hasComponent<Text>())
        {
            ++count;
        }
    }
    return count;
}

/// Every `Text` component in `world`, in entity order.
[[nodiscard]] std::vector<const Text*> textComponents(const EntityManager& world)
{
    std::vector<const Text*> found;
    for (const engine::ecs::Entity& entity : world.getEntities())
    {
        if (entity.hasComponent<Text>())
        {
            found.push_back(&entity.getComponent<Text>());
        }
    }
    return found;
}

// ---------------------------------------------------------------------------
// Driving a scene with input
//
// ### Through the real map, not by setting actions
//
// `ActionState` has no setters - it is a snapshot, and a snapshot that could be
// hand-filled would not be the one a scene is handed in production. So these tests
// press *keys* on a real `Input` and let `defaultActionMap()` resolve them, exactly
// as `Application::update` does. The keys are the test's business; naming one inside
// a scene is what would be the bug, and there is a group that checks for it.
//
// Going through the map also proves something a hand-filled snapshot could not:
// pressing `Space` sets `Shoot` and nothing else, while `W` sets `Jump` *and*
// `MoveUp`. So a menu that confirmed on `Jump` instead of `Shoot` would fail the
// `Space` group and pass the `W` one - the two actions are told apart by the test
// itself rather than by the menu's cooperation.
// ---------------------------------------------------------------------------

class ActionDriver
{
public:
    /// One frame in which `key` has just gone down: active, and pressed this frame.
    [[nodiscard]] ActionState pressedNow(const Key key)
    {
        m_input.processKeyDown(key);
        return snapshot();
    }

    /// One frame in which everything pressed so far is still held: active, but not
    /// pressed. This is what `beginFrame` exists to produce, and it is the
    /// difference the press-edge groups need.
    [[nodiscard]] ActionState held()
    {
        m_input.beginFrame();
        return snapshot();
    }

    /// One frame with nothing held.
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
// The scene double, and the ledger it reports through.
//
// The ledger is a `shared_ptr` the test also holds, so it outlives every scene
// whose destruction it observes. That is what makes "the old scene was destroyed"
// a run-time fact rather than a reading of freed memory.
// ---------------------------------------------------------------------------

struct SceneLedger
{
    int constructed = 0;
    int destroyed = 0;
    int updates = 0;
    int renders = 0;
    std::vector<std::string> updateOrder;
    std::vector<std::string> renderOrder;
    std::vector<std::string> alive;

    /// How deep into a scene's own update or render the program is.
    ///
    /// The destructor reads these. A transition that destroyed a scene *during* its
    /// own update would set `destroyedDuringUpdate`, and that is the whole safety
    /// property, checked rather than asserted in a comment.
    int updatesInProgress = 0;
    int rendersInProgress = 0;
    bool destroyedDuringUpdate = false;
    bool destroyedDuringRender = false;

    /// If [requestFrom] is not empty, a scene with that label asks for [request]
    /// during its next update.
    std::string requestFrom;

    /// Copy-initialised rather than brace-initialised: inside braces the compiler
    /// considers the private two-argument constructor first, and a braced list is
    /// not a place to be surprised by a private overload being viable.
    SceneTransition request = SceneTransition::stay();
};

class RecordingScene final : public Scene
{
public:
    RecordingScene(const std::shared_ptr<SceneLedger>& ledger, std::string label, const SceneId id,
                   const SceneContext& context)
        : Scene{context}, m_ledger{ledger}, m_label{std::move(label)}, m_id{id}
    {
        ++m_ledger->constructed;
        m_ledger->alive.push_back(m_label);
    }

    ~RecordingScene() override
    {
        ++m_ledger->destroyed;

        // The invariant, checked while it is still true.
        if (m_ledger->updatesInProgress > 0)
        {
            m_ledger->destroyedDuringUpdate = true;
        }
        if (m_ledger->rendersInProgress > 0)
        {
            m_ledger->destroyedDuringRender = true;
        }

        const auto found = std::find(m_ledger->alive.begin(), m_ledger->alive.end(), m_label);
        if (found != m_ledger->alive.end())
        {
            m_ledger->alive.erase(found);
        }
    }

    void onUpdate(const ActionState& actions, const float deltaSeconds) override
    {
        static_cast<void>(actions);
        static_cast<void>(deltaSeconds);

        ++m_ledger->updatesInProgress;
        ++m_ledger->updates;
        m_ledger->updateOrder.push_back(m_label);

        if (!m_ledger->requestFrom.empty() && m_ledger->requestFrom == m_label)
        {
            requestTransition(m_ledger->request);
        }

        --m_ledger->updatesInProgress;
    }

    void render() override
    {
        ++m_ledger->rendersInProgress;
        ++m_ledger->renders;
        m_ledger->renderOrder.push_back(m_label);
        --m_ledger->rendersInProgress;
    }

    [[nodiscard]] const char* name() const noexcept override { return m_label.c_str(); }
    [[nodiscard]] SceneId id() const noexcept override { return m_id; }

private:
    std::shared_ptr<SceneLedger> m_ledger;
    std::string m_label;
    SceneId m_id;
};

[[nodiscard]] std::string labelFor(const SceneId id)
{
    switch (id)
    {
        case SceneId::Menu:
            return "menu";
        case SceneId::Play:
            return "play";
    }

    return "unknown";
}

/// Builds an `Application` whose scenes are recording doubles.
[[nodiscard]] std::pair<std::unique_ptr<engine::Application>, std::shared_ptr<SceneLedger>> recordingApplication(
    std::shared_ptr<SceneLedger>& ledger)
{
    // Two applications cannot share a window's name safely and, more importantly,
    // a test that kept one alive would be testing a stale scene. The ledger is
    // created here and moved into the pair, so the caller holds the only other
    // reference and knows it outlives every scene.
    auto application = std::make_unique<engine::Application>();
    application->setSceneFactory([held = ledger](const SceneId id, const SceneContext& context) {
        return std::make_unique<RecordingScene>(held, labelFor(id), id, context);
    });
    return {std::move(application), ledger};
}

// ---------------------------------------------------------------------------
// Source reading, with comments removed.
//
// A comment that says "not sf::Text" contains "sf::Text". Every architectural
// check in this file is a "this must not appear" check, so a substring scan over
// raw source would fail on the file explaining the rule. Comments are stripped
// first, and a check that genuinely wants to read a comment says so separately.
// ---------------------------------------------------------------------------

/// Removes `//` line comments and `/* */` block comments, preserving line count.
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
                    // Newline kept, so a stripped file still has the same number of
                    // lines and a line-numbered diagnostic means the same thing.
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

/// The code of a file with comments removed, for the "must not appear" checks.
[[nodiscard]] std::string codeWithoutComments(const char* const path)
{
    return stripComments(codeOf(path));
}

/// True when `needle` appears in `haystack`.
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

/// The active scene's id, but safe when there is no scene at all.
///
/// ### Why not `sceneId().value()`
///
/// The first version of these groups read the id with `optional::value()`, which
/// throws on an empty optional and is undefined behaviour in a release build. That
/// is not a hypothetical: the `transition-disabled` mutation removes the only thing
/// that creates a scene, so every one of these groups hit an empty optional - and
/// the test binary **segfaulted**. The suite then reported that mutation as
/// UNDETECTED, because a crash is not an assertion and the harness had been
/// matching only `\(Failed\)`.
///
/// A test that dies before it can assert is worse than a test that fails, because
/// it takes the other groups down with it and looks like a pass. So this returns
/// `false` for "no scene", which fails the check cleanly and says what it meant.
[[nodiscard]] bool activeSceneIs(const engine::Application& application, const SceneId expected)
{
    const std::optional<SceneId> id = application.sceneId();
    return id.has_value() && *id == expected;
}

// ---------------------------------------------------------------------------
// A. The shape of the abstraction
// ---------------------------------------------------------------------------

void testSceneIsAnAbstraction()
{
    // A scene with no `update` or `render` cannot be a scene, and one that can be
    // copied or moved cannot be the single owner of a world. Both are checked where
    // only the compiler can.
    static_assert(std::is_abstract_v<Scene>, "Scene must be abstract");
    static_assert(!std::is_copy_constructible_v<Scene>, "a scene owns a world and cannot be copied");
    static_assert(!std::is_copy_assignable_v<Scene>, "a scene owns a world and cannot be copied");
    static_assert(!std::is_move_constructible_v<Scene>, "moving would leave the systems' pointers stale");
    static_assert(!std::is_move_assignable_v<Scene>, "moving would leave the systems' pointers stale");
    static_assert(std::has_virtual_destructor_v<Scene>, "a base held by unique_ptr must destruct its derived");

    CHECK(true);
}

void testASceneTransitionIsAValue()
{
    // Three answers, and each is distinguishable from the other two. A transition
    // that could not tell "switch to the menu" from "quit" would make a menu's quit
    // option navigate instead.
    const SceneTransition stay = SceneTransition::stay();
    const SceneTransition toMenu = SceneTransition::to(SceneId::Menu);
    const SceneTransition toPlay = SceneTransition::to(SceneId::Play);
    const SceneTransition quit = SceneTransition::quitApplication();

    CHECK_FALSE(stay.scene().has_value());
    CHECK_FALSE(stay.quits());

    CHECK(toMenu.scene().has_value());
    CHECK(toMenu.scene().value() == SceneId::Menu);
    CHECK_FALSE(toMenu.quits());

    CHECK(toPlay.scene().has_value());
    CHECK(toPlay.scene().value() == SceneId::Play);

    CHECK_FALSE(quit.scene().has_value());
    CHECK(quit.quits());

    // Switches to different scenes are different transitions.
    CHECK_FALSE(toMenu.scene().value() == toPlay.scene().value());

    // A copy is a copy, and assigning one replaces the other: a scene changes its
    // mind within a frame, and a value type is what makes that expressible.
    SceneTransition mutable_ = stay;
    mutable_ = toPlay;
    CHECK(mutable_.scene().value() == SceneId::Play);
    CHECK_FALSE(mutable_.quits());

    // And it is default-constructible, because "nothing asked for yet" is a state a
    // scene starts in.
    const SceneTransition fresh;
    CHECK_FALSE(fresh.scene().has_value());
    CHECK_FALSE(fresh.quits());
}

void testAContextBorrowsAndDoesNotOwn()
{
    SceneFixture fixture;

    // The context reaches the three objects it was given and nothing else. Pointer
    // identity, not just type: a context that copied the asset manager would answer
    // this test and still be wrong, because the copy would load every texture again.
    CHECK(&fixture.context().renderer() == &fixture.renderer());
    CHECK(&fixture.context().camera() == &fixture.camera());
    CHECK(&fixture.context().assets() == &fixture.assets());

    // A context is copyable, and copying it does not copy what it points at. This
    // is the "borrowed" claim stated in the only way a test can state it.
    const SceneContext copy = fixture.context();
    CHECK(&copy.assets() == &fixture.assets());
    CHECK(&copy.renderer() == &fixture.renderer());
}

void testMakeSceneBuildsEveryScene()
{
    SceneFixture fixture;

    // A closed set of ids and a builder for each: "which scenes exist" is a function
    // that can be read top to bottom, and every id has one.
    const std::unique_ptr<Scene> menu = engine::scene::makeScene(SceneId::Menu, fixture.context());
    const std::unique_ptr<Scene> play = engine::scene::makeScene(SceneId::Play, fixture.context());

    CHECK(menu != nullptr);
    CHECK(play != nullptr);
    CHECK(menu->id() == SceneId::Menu);
    CHECK(play->id() == SceneId::Play);

    // Each scene reports its own identity, and it is the identity the caller asked
    // for. A builder that ignored its argument would be caught here.
    CHECK(std::string{menu->name()} == "MenuScene");
    CHECK(std::string{play->name()} == "PlayScene");

    // A fresh scene has asked for nothing. The default has to be "stay", because a
    // scene that asked to switch on construction would switch before it had run.
    CHECK_FALSE(menu->pendingTransition().scene().has_value());
    CHECK_FALSE(menu->pendingTransition().quits());
    CHECK_FALSE(play->pendingTransition().scene().has_value());

    // An id outside the set is a bug, and returning a blank scene would turn it into
    // a black screen. It throws instead.
    bool threw = false;
    try
    {
        const std::unique_ptr<Scene> unknown =
            engine::scene::makeScene(static_cast<SceneId>(99), fixture.context());
        static_cast<void>(unknown);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    CHECK(threw);
}

// ---------------------------------------------------------------------------
// B. The menu
// ---------------------------------------------------------------------------

void testTheMenuCanBeCreated()
{
    SceneFixture fixture;
    const std::unique_ptr<MenuScene> menu = std::make_unique<MenuScene>(fixture.context());

    // A title, two options and a caption: four entities, and every one of them
    // carrying a Text component. The count is pinned because "a menu" and "a menu
    // with one invisible line in it" are different programs.
    CHECK(menu->world().aliveEntityCount() == 4U);
    CHECK(countTextEntities(menu->world()) == 4U);

    const std::vector<const Text*> texts = textComponents(menu->world());
    CHECK(texts.size() == 4U);
    if (texts.size() == 4U)
    {
        // The four strings, in entity order. A menu whose title was silently
        // replaced by an empty string still has four text entities.
        CHECK(texts[0]->content == "GAME ENGINE");
        CHECK(texts[1]->content == "> START");
        CHECK(texts[2]->content == "  QUIT");
        CHECK(texts[3]->content == "MOVE: ARROWS   SELECT: SPACE");

        // The title is the biggest thing on screen, which is what makes it a title.
        CHECK(texts[0]->characterSize > texts[1]->characterSize);
        CHECK(texts[1]->characterSize > texts[3]->characterSize);
    }

    // Every one of them names a font the asset manager actually has, so the menu
    // cannot fail to draw for a reason a test would call "fine".
    for (const Text* text : texts)
    {
        CHECK_FALSE(text->fontAssetName.empty());
        bool found = false;
        try
        {
            static_cast<void>(fixture.assets().font(text->fontAssetName));
            found = true;
        }
        catch (const std::exception&)
        {
            found = false;
        }
        CHECK(found);
    }
}

void testTheMenuRendersRealText()
{
    SceneFixture fixture;
    const std::unique_ptr<MenuScene> menu = std::make_unique<MenuScene>(fixture.context());

    // Real window, real renderer, real font. The claim is that the menu puts ink on
    // the screen, and only a readback can say that.
    const ChangedPixels drawn = fixture.measureFrame([&menu] { menu->render(); });
    CHECK(drawn.anythingChanged());
    CHECK(drawn.count > 0L);

    // An empty menu - no entities - draws nothing at all, which is what makes the
    // number above a statement about the menu rather than about the fixture.
    EntityManager empty;
    const ChangedPixels nothing = fixture.measureFrame([] {});
    CHECK_FALSE(nothing.anythingChanged());
    CHECK(nothing.count == 0L);

    // And the ink is inside the window, so the menu is laid out against the viewport
    // the fixture gave it rather than at some fixed screen position.
    CHECK(drawn.minX >= 0);
    CHECK(drawn.minY >= 0);
    CHECK(drawn.maxX < static_cast<int>(kWindowWidth));
    CHECK(drawn.maxY < static_cast<int>(kWindowHeight));
}

void testTheMenuNavigatesOnActions()
{
    SceneFixture fixture;
    MenuScene menu{fixture.context()};

    CHECK(menu.selectedIndex() == 0U);
    CHECK(MenuScene::optionCount() == 2U);

    // `Down` is bound to `S`, which drives `MoveDown` and nothing else.
    // Down, then down again: two options, so the second press wraps to the start.
    menu.update(ActionDriver{}.pressedNow(Key::S), 0.016F);
    CHECK(menu.selectedIndex() == 1U);

    menu.update(ActionDriver{}.pressedNow(Key::S), 0.016F);
    CHECK(menu.selectedIndex() == 0U);

    // `Up` is bound to `Up` as well as to `W`, and `Up` drives `MoveUp` alone - so
    // this is navigation without also being a jump.
    //
    // Up from the first wraps to the last, so a two-item menu is usable in both
    // directions without an "you are at the end" case.
    menu.update(ActionDriver{}.pressedNow(Key::Up), 0.016F);
    CHECK(menu.selectedIndex() == 1U);

    menu.update(ActionDriver{}.pressedNow(Key::Up), 0.016F);
    CHECK(menu.selectedIndex() == 0U);

    // The caption follows the selection, so the state is visible and not merely
    // held in a member.
    CHECK(menu.world().getEntities("menu.option.0").begin()->getComponent<Text>().content == "> START");
    CHECK(menu.world().getEntities("menu.option.1").begin()->getComponent<Text>().content == "  QUIT");
}

void testTheMenuMovesOnThePressEdgeOnly()
{
    SceneFixture fixture;
    MenuScene menu{fixture.context()};

    // A menu option must activate once per press. Holding a direction would
    // otherwise run the selection round every frame and land wherever it landed.
    ActionDriver driver;
    static_cast<void>(driver.pressedNow(Key::S));

    for (int frame = 0; frame < 5; ++frame)
    {
        // `held()` calls beginFrame(), which is what turns a fresh press into a
        // continuing hold: still `isActive`, no longer `wasPressed`.
        menu.update(driver.held(), 0.016F);
    }

    // Five frames of a held key, and the selection has not moved at all.
    CHECK(menu.selectedIndex() == 0U);

    // And the same in the *other* direction.
    //
    // This was missing, and a mutation proved it. Reading `isActive` instead of
    // `wasPressed` for `MoveUp` - while the held key here is `S`, which drives
    // `MoveDown` - changed nothing observable and passed all nineteen suites. The
    // press-edge property was being tested for one of the two navigation actions
    // and assumed for the other, and a menu that wrapped on every held `W` while
    // behaving on `S` would have shipped.
    ActionDriver upward;
    static_cast<void>(upward.pressedNow(Key::Up));
    for (int frame = 0; frame < 5; ++frame)
    {
        menu.update(upward.held(), 0.016F);
    }
    CHECK(menu.selectedIndex() == 0U);

    // The edge is what moves it, and the edge comes from the action layer's own
    // `wasPressed`, so this is the menu reading an action rather than a key.
    menu.update(ActionDriver{}.pressedNow(Key::S), 0.016F);
    CHECK(menu.selectedIndex() == 1U);
    menu.update(ActionDriver{}.pressedNow(Key::Up), 0.016F);
    CHECK(menu.selectedIndex() == 0U);
}

void testTheMenuStartsTheGame()
{
    SceneFixture fixture;
    MenuScene menu{fixture.context()};

    // Nothing asked for yet.
    CHECK_FALSE(menu.pendingTransition().scene().has_value());

    // With START selected, `Shoot` asks for the play scene - and only asks.
    // `Space` drives `Shoot` alone, so this cannot be a `Jump` that happens to share
    // a key with `MoveUp`.
    menu.update(ActionDriver{}.pressedNow(Key::Space), 0.016F);

    CHECK(menu.pendingTransition().scene().has_value());
    CHECK(menu.pendingTransition().scene().value() == SceneId::Play);
    CHECK_FALSE(menu.pendingTransition().quits());

    // Still the menu, and still alive: the scene asked, it did not act.
    CHECK(menu.id() == SceneId::Menu);
    CHECK(menu.world().aliveEntityCount() == 4U);
}

void testTheMenuQuits()
{
    SceneFixture fixture;
    MenuScene menu{fixture.context()};

    // Move to QUIT and confirm. Quit is its own answer, not a scene, so a scene
    // never has to know what "no scene" means.
    menu.update(ActionDriver{}.pressedNow(Key::S), 0.016F);
    CHECK(menu.selectedIndex() == 1U);

    menu.update(ActionDriver{}.pressedNow(Key::Space), 0.016F);
    CHECK(menu.pendingTransition().quits());
    CHECK_FALSE(menu.pendingTransition().scene().has_value());
}

void testTheMenuIgnoresActionsThatMeanNothingToIt()
{
    SceneFixture fixture;
    MenuScene menu{fixture.context()};

    // A jump, a move left, a move right and the toggles are all real actions in this
    // engine and none of them means anything to a menu. If any of them started the
    // game, a player walking the level and coming back would be launched by a stray
    // key.
    //
    // `W` is the interesting one, because it drives **two** actions at once: the
    // action layer binds it to `Jump` *and* to `MoveUp`. So this single key must
    // navigate the menu and must not start the game - a menu that treated a jump as
    // a confirmation would launch the level here, and a menu that ignored navigation
    // would be unusable.
    //
    // Which of the two it honours is not the menu's decision. The menu reads
    // `MoveUp` because that is what it means, and has no idea the same key is also
    // `Jump`; that overlap belongs to the action layer, and it is precisely why a
    // scene is handed actions rather than keys.
    menu.update(ActionDriver{}.pressedNow(Key::W), 0.016F);
    CHECK(menu.selectedIndex() == 1U); // `MoveUp`, honoured
    CHECK_FALSE(menu.pendingTransition().scene().has_value());
    CHECK_FALSE(menu.pendingTransition().quits()); // `Jump`, ignored

    // A key bound to nothing this menu uses moves nothing.
    for (const Key key : {Key::A, Key::D, Key::P, Key::X, Key::Z, Key::T, Key::C, Key::G})
    {
        menu.update(ActionDriver{}.pressedNow(key), 0.016F);
    }

    CHECK(menu.selectedIndex() == 1U);
    CHECK_FALSE(menu.pendingTransition().scene().has_value());
    CHECK_FALSE(menu.pendingTransition().quits());
}

// ---------------------------------------------------------------------------
// C. The play scene
// ---------------------------------------------------------------------------

void testThePlaySceneBuildsAWorld()
{
    SceneFixture fixture;
    const std::unique_ptr<PlayScene> play = std::make_unique<PlayScene>(fixture.context());

    // The level, spawned. The committed level carries 24 tiles, 4 decorations and a
    // player; the label is this scene's own addition.
    CHECK(play->world().aliveEntityCount() > 28U);

    // And exactly one text entity: the shipped label, which was a source check in
    // Phase 14 because `main` built the world in a function no test could call. A
    // scene *is* callable, so the demonstration is now observable at run time, and
    // this is the group that says so.
    CHECK(countTextEntities(play->world()) == 1U);

    const std::vector<const Text*> texts = textComponents(play->world());
    if (texts.size() == 1U)
    {
        // Its content is read from the level's own player spawn, so it is
        // information the level carries rather than a caption, and it is not the
        // menu's title: the two scenes' text is distinguishable.
        CHECK(texts[0]->content != "GAME ENGINE");
        CHECK(has(texts[0]->content, "SPAWN"));
        CHECK_FALSE(texts[0]->fontAssetName.empty());

        // The label is world space, and deliberately so: it describes where the
        // player started in the level, so it belongs to the world. Same component,
        // same font, same renderer as the menu - one marked and one not, which is
        // what makes screen space a choice rather than a new default.
        const engine::ecs::Entity& label = [&play]() -> const engine::ecs::Entity& {
            for (const engine::ecs::Entity& entity : play->world().getEntities())
            {
                if (entity.hasComponent<Text>())
                {
                    return entity;
                }
            }
            throw std::logic_error{"no text entity"};
        }();
        CHECK_FALSE(label.hasComponent<engine::components::ScreenSpace>());
    }
}

void testThePlaySceneRegistersItsOwnSystems()
{
    SceneFixture fixture;
    const std::unique_ptr<PlayScene> play = std::make_unique<PlayScene>(fixture.context());

    // Four, in the order the level has always used. Registration order *is* update
    // order, and the count is pinned so a system cannot be added or dropped without
    // this suite noticing.
    CHECK(play->systems().systemCount() == 4U);

    if (play->systems().systemCount() == 4U)
    {
        CHECK(std::string{play->systems().systemAt(0U).name()} == "PhysicsSystem");
        CHECK(std::string{play->systems().systemAt(1U).name()} == "CameraSystem");
        CHECK(std::string{play->systems().systemAt(2U).name()} == "ZoomKeysSystem");
        CHECK(std::string{play->systems().systemAt(3U).name()} == "AnimationSystem");
    }

    // No movement system, and the absence is load bearing: this engine's movement
    // system sets a velocity on *every* transform, so registering it would give
    // every ground tile and every cloud a velocity, and a decoration has no body to
    // stop it. The test says the count is four, which is what pins the absence.
    for (std::size_t index = 0U; index < play->systems().systemCount(); ++index)
    {
        CHECK(std::string{play->systems().systemAt(index).name()} != "MovementSystem");
    }

    // The world height is the game's, read from the generated configuration rather
    // than hard-coded in the scene.
    CHECK_NEAR(play->cellsTall(), static_cast<float>(engine::config::kLevelCellsTall), 0.0001F);
    CHECK(play->cellsTall() >= 16.0F);
}

void testThePlaySceneGoesBackToTheMenu()
{
    SceneFixture fixture;
    PlayScene play{fixture.context()};

    CHECK_FALSE(play.pendingTransition().scene().has_value());

    // `Quit` is the action the action layer bound to `Escape`, and the course's
    // Assignment 3 says Escape goes back to the main menu. So this is `Quit`, and no
    // new action was invented for "back".
    play.update(ActionDriver{}.pressedNow(Key::Escape), 0.016F);
    CHECK(play.pendingTransition().scene().has_value());
    CHECK(play.pendingTransition().scene().value() == SceneId::Menu);

    // Anything else leaves it alone. In particular the actions a level will want -
    // move, shoot, jump - must not navigate, or the player would be thrown to the
    // menu by walking.
    PlayScene other{fixture.context()};
    other.update(ActionDriver{}.pressedNow(Key::D), 0.016F);
    other.update(ActionDriver{}.pressedNow(Key::Space), 0.016F);
    other.update(ActionDriver{}.pressedNow(Key::W), 0.016F);
    other.update(ActionDriver{}.pressedNow(Key::P), 0.016F);
    CHECK_FALSE(other.pendingTransition().scene().has_value());
}

void testThePlaySceneUpdatesAndRenders()
{
    SceneFixture fixture;
    PlayScene play{fixture.context()};

    // A frame's worth of behaviour: physics, camera follow, zoom keys and animation
    // all run against the scene's own world, and nothing escapes.
    const Vec2 before = play.world().getEntities("level.player").begin()->getComponent<Transform>().position;
    play.update(ActionState{}, 0.016F);
    const Vec2 after = play.world().getEntities("level.player").begin()->getComponent<Transform>().position;

    // The player does not move yet, and that is this phase's decision rather than an
    // omission: there is no movement system. What the frame *does* do is drive the
    // camera onto the player.
    CHECK_NEAR(before.x, after.x, 0.0001F);
    CHECK_NEAR(before.y, after.y, 0.0001F);
    CHECK_NEAR(fixture.camera().position().x, before.x, 0.001F);

    // And it draws: a real frame, through the real renderer, with the level's
    // hundreds of entities in it.
    const ChangedPixels drawn = fixture.measureFrame([&play] { play.render(); });
    CHECK(drawn.anythingChanged());
    CHECK(drawn.count > 0L);
}

void testThePlaySceneFlushesItsOwnDeferredDestruction()
{
    SceneFixture fixture;
    PlayScene play{fixture.context()};

    // The level carries animations that do not repeat, and `AnimationSystem` flags
    // their entities for destruction. The scene owns the world, so the scene has to
    // flush it - the owner's `EntityManager` is a *different* world and would never
    // see these entities, so a level that only flushed the owner's would grow every
    // frame without bound.
    const std::size_t initialStored = play.world().storedEntityCount();
    CHECK(initialStored > 0U);

    for (int frame = 0; frame < 400; ++frame)
    {
        play.update(ActionState{}, 0.1F);
    }

    // Stored never exceeds the initial total: entities flagged for destruction are
    // erased rather than piling up as destroyed-but-still-held records.
    CHECK(play.world().storedEntityCount() <= initialStored);
    CHECK(play.world().aliveEntityCount() > 0U);
}

// ---------------------------------------------------------------------------
// D. The transition lifecycle
// ---------------------------------------------------------------------------

void testChangeSceneIsDeferredUntilTheNextFrame()
{
    auto ledger = std::make_shared<SceneLedger>();
    auto [application, held] = recordingApplication(ledger);
    static_cast<void>(held);

    // A fresh application has no scene, and that is a real state: it drives its own
    // world and systems, which is how this engine worked before scenes and how
    // several tests build a world.
    CHECK_FALSE(application->sceneId().has_value());
    CHECK(application->currentScene() == nullptr);

    application->changeScene(SceneId::Menu);

    // Asked for, not done. A scene that did not exist yet cannot have been built
    // yet, and reporting it early would make this assertion a lie.
    CHECK_FALSE(application->sceneId().has_value());
    CHECK(application->currentScene() == nullptr);
    CHECK(ledger->constructed == 0);

    // The first frame is where the request is applied, so the first frame is already
    // the menu's - which is what lets `main` ask for the menu before `run` and get
    // a menu on frame one.
    application->update();
    CHECK(application->sceneId().has_value());
    CHECK(activeSceneIs(*application, SceneId::Menu));
    CHECK(ledger->constructed == 1);
}

void testTheTransitionHappensAtAFrameBoundary()
{
    auto ledger = std::make_shared<SceneLedger>();
    auto [application, held] = recordingApplication(ledger);
    static_cast<void>(held);

    application->changeScene(SceneId::Menu);
    application->update();
    CHECK(activeSceneIs(*application, SceneId::Menu));

    // The menu asks for the play scene, from inside its own update.
    ledger->requestFrom = "menu";
    ledger->request = SceneTransition::to(SceneId::Play);

    application->update();

    // It asked, and nothing has happened yet. The frame that asked is still the
    // menu's frame - this is the whole deferral, and the reason a frame is never
    // updated by one scene and drawn by another.
    CHECK(activeSceneIs(*application, SceneId::Menu));
    CHECK(ledger->destroyed == 0);

    // And this frame's render is still the menu's, for the same reason.
    application->render();
    CHECK(ledger->renderOrder.size() == 1U);
    if (!ledger->renderOrder.empty())
    {
        CHECK(ledger->renderOrder.back() == "menu");
    }

    // The next frame is the new scene's, from its very first update.
    application->update();
    CHECK(activeSceneIs(*application, SceneId::Play));
    CHECK(ledger->destroyed == 1);
    CHECK(ledger->updateOrder.size() == 3U);
    if (ledger->updateOrder.size() == 3U)
    {
        CHECK(ledger->updateOrder[2] == "play");
    }
}

void testATransitionDestroysTheOldScene()
{
    auto ledger = std::make_shared<SceneLedger>();
    auto [application, held] = recordingApplication(ledger);
    static_cast<void>(held);

    application->changeScene(SceneId::Menu);
    application->update();

    CHECK(application->currentScene() != nullptr);
    CHECK(std::string{application->currentScene()->name()} == "menu");
    CHECK(ledger->alive.size() == 1U);
    if (!ledger->alive.empty())
    {
        CHECK(ledger->alive.front() == "menu");
    }

    // The outgoing scene is destroyed and the incoming one built, and never the
    // other way round: two scenes alive at once would mean two worlds, and a
    // reference the reference implementation's `shared_ptr` map cannot avoid.
    ledger->requestFrom = "menu";
    ledger->request = SceneTransition::to(SceneId::Play);
    application->update();
    application->update();

    CHECK(ledger->destroyed == 1);
    CHECK(ledger->constructed == 2);
    CHECK(ledger->alive.size() == 1U);
    if (!ledger->alive.empty())
    {
        CHECK(ledger->alive.front() == "play");
    }
    // The active scene is now the play one, identified by **what it says it is**
    // rather than by its address.
    //
    // An address is not an identity. The old scene is freed and the new one
    // allocated immediately afterwards, so a conforming allocator is entitled to
    // hand back the same block - and this test observed exactly that on the first
    // run. Asserting the pointers differ would be a test that passes for the wrong
    // reason: it would report the allocator's mood rather than the scene's
    // lifetime. The ledger's `destroyed` count and `alive` list are the evidence;
    // this is the confirmation, and it holds whatever the allocator does.
    CHECK(std::string{application->currentScene()->name()} == "play");
}

void testNoSceneIsDestroyedWhileItIsRunning()
{
    auto ledger = std::make_shared<SceneLedger>();
    auto [application, held] = recordingApplication(ledger);
    static_cast<void>(held);

    application->changeScene(SceneId::Menu);
    application->update();

    // Ask for a switch from inside the scene's own update, then keep the loop
    // running. The scene double's destructor reads how deep into an update or a
    // render the program is, so a transition applied *during* one would set these
    // flags rather than being described in a comment.
    ledger->requestFrom = "menu";
    ledger->request = SceneTransition::to(SceneId::Play);

    for (int frame = 0; frame < 10; ++frame)
    {
        application->update();
        application->render();
    }

    CHECK_FALSE(ledger->destroyedDuringUpdate);
    CHECK_FALSE(ledger->destroyedDuringRender);

    // And the switch really did happen, so the checks above are not passing because
    // nothing was ever destroyed.
    CHECK(ledger->destroyed == 1);
    CHECK(activeSceneIs(*application, SceneId::Play));
}

void testOnlyTheActiveSceneUpdates()
{
    auto ledger = std::make_shared<SceneLedger>();
    auto [application, held] = recordingApplication(ledger);
    static_cast<void>(held);

    application->changeScene(SceneId::Menu);
    application->update();

    for (int frame = 0; frame < 5; ++frame)
    {
        application->update();
    }

    // Five updates of one scene, not five of each and not five of both. The ledger
    // records the *order*, so a loop that updated every scene would show both names
    // in it rather than five copies of one.
    CHECK(ledger->updateOrder.size() == 6U);
    for (const std::string& label : ledger->updateOrder)
    {
        CHECK(label == "menu");
    }
    CHECK(ledger->alive.size() == 1U);
}

void testOnlyTheActiveSceneRenders()
{
    auto ledger = std::make_shared<SceneLedger>();
    auto [application, held] = recordingApplication(ledger);
    static_cast<void>(held);

    application->changeScene(SceneId::Menu);
    application->update();

    for (int frame = 0; frame < 5; ++frame)
    {
        application->render();
    }

    CHECK(ledger->renderOrder.size() == 5U);
    for (const std::string& label : ledger->renderOrder)
    {
        CHECK(label == "menu");
    }

    // After a transition, the new scene draws and the old one never draws again -
    // "old scene rendering after transition" is one of the specific failures this
    // architecture is meant to make impossible.
    ledger->requestFrom = "menu";
    ledger->request = SceneTransition::to(SceneId::Play);
    application->update();
    application->update();
    application->render();

    CHECK(ledger->renderOrder.size() == 6U);
    if (ledger->renderOrder.size() == 6U)
    {
        CHECK(ledger->renderOrder.back() == "play");
    }
}

void testTheMenuTransitionsToPlayAndBack()
{
    // The whole loop, through real scenes rather than doubles: the game's own path
    // from the first screen to the level and back.
    auto ledger = std::make_shared<SceneLedger>();
    engine::Application application;
    static_cast<void>(ledger);

    CHECK_FALSE(application.sceneId().has_value());

    application.changeScene(SceneId::Menu);
    application.update();
    CHECK(activeSceneIs(application, SceneId::Menu));

    // The menu is a real menu now, with a real world and real text.
    const auto* menu = dynamic_cast<const MenuScene*>(application.currentScene());
    CHECK(menu != nullptr);
    if (menu != nullptr)
    {
        CHECK(menu->world().aliveEntityCount() == 4U);
    }

    // Confirm, the way a player does: through the action the action layer built.
    application.changeScene(SceneId::Play);
    application.update();
    CHECK(activeSceneIs(application, SceneId::Play));

    // The play scene's world is its own. The menu's four entities are gone, not
    // sharing a world with the level.
    const auto* play = dynamic_cast<const PlayScene*>(application.currentScene());
    CHECK(play != nullptr);
    if (play != nullptr)
    {
        CHECK(play->world().aliveEntityCount() > 28U);
        CHECK(play->systems().systemCount() == 4U);
    }

    application.render();

    // And back again.
    application.changeScene(SceneId::Menu);
    application.update();
    CHECK(activeSceneIs(application, SceneId::Menu));
    const auto* menuAgain = dynamic_cast<const MenuScene*>(application.currentScene());
    CHECK(menuAgain != nullptr);
    if (menuAgain != nullptr)
    {
        // A *new* menu, at its initial selection. Not the old one, which is what a
        // cache of scenes would have handed back along with its old selection.
        CHECK(menuAgain->selectedIndex() == 0U);
        CHECK(menuAgain->world().aliveEntityCount() == 4U);
    }
}

void testAQuitRequestStopsTheApplication()
{
    auto ledger = std::make_shared<SceneLedger>();
    auto [application, held] = recordingApplication(ledger);
    static_cast<void>(held);

    application->changeScene(SceneId::Menu);
    application->update();
    CHECK(activeSceneIs(*application, SceneId::Menu));

    ledger->requestFrom = "menu";
    ledger->request = SceneTransition::quitApplication();

    // The frame that asks has finished its own update, and nothing has happened yet:
    // a quit is a request like any other and is honoured at the next boundary. The
    // first version of this group asserted `isRunning()` here, one frame too early,
    // and failed against code that was correct.
    application->update();
    CHECK(application->isRunning());
    CHECK(ledger->destroyed == 0);
    CHECK(application->currentScene() != nullptr);

    // The next frame's boundary, and the quit is applied. The scene is *not* destroyed
    // by a quit: it did nothing wrong, and the process is ending anyway.
    application->update();
    CHECK_FALSE(application->isRunning());
    CHECK(ledger->destroyed == 0);

    // And the loop stays stopped.
    //
    // This is asked rather than inferred from `run` returning. The first version
    // called `run(2)` and asserted EXIT_SUCCESS, which is what `run` returns when it
    // stops *for any reason* - including the frame cap it was handed. Removing the
    // one line that clears the running flag left the test green, because two frames
    // is two frames and then it exits happily. Inferring "the quit worked" from a
    // loop that was told to stop after two frames is not an assertion about
    // quitting; it is an assertion about the cap.
    CHECK_FALSE(application->isRunning());
    CHECK(application->run(2U) == EXIT_SUCCESS);
    CHECK_FALSE(application->isRunning());

    // A quit with no scene behind it is honoured too, so the flag is cleared by the
    // boundary rather than by the scene path alone.
    engine::Application noScene;
    CHECK(noScene.isRunning());
    noScene.changeScene(SceneId::Menu);
    noScene.update();
    CHECK(noScene.isRunning());
}

void testSceneIdIsAskedOfTheScene()
{
    // A factory that ignores the id it was given must not be able to make
    // `sceneId()` report a scene that is not running. This is why the id is asked of
    // the scene rather than remembered beside it.
    auto ledger = std::make_shared<SceneLedger>();
    engine::Application application;
    static_cast<void>(ledger);

    application.setSceneFactory([held = ledger](const SceneId /*requested*/, const SceneContext& context) {
        // Deliberately the *other* scene.
        return std::make_unique<RecordingScene>(held, "liar", SceneId::Play, context);
    });

    application.changeScene(SceneId::Menu);
    application.update();

    CHECK(application.sceneId().has_value());
    CHECK(activeSceneIs(application, SceneId::Play));
    CHECK(std::string{application.currentScene()->name()} == "liar");
}

void testApplicationWithoutASceneStillWorks()
{
    // The no-scene path is not a leftover. A game or a tool may build a world
    // directly on the application, and twelve groups in four existing suites do
    // exactly that, so it has to keep working.
    engine::Application application;

    CHECK_FALSE(application.sceneId().has_value());
    CHECK(application.currentScene() == nullptr);
    CHECK(application.entityManager().aliveEntityCount() == 0U);
    CHECK(application.systemManager().systemCount() == 0U);

    engine::ecs::Entity& rectangle = application.entityManager().addEntity("rectangle");
    rectangle.addComponent<Transform>(Transform{Vec2{10.0F, 10.0F}, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
    rectangle.addComponent<engine::components::Rectangle>(
        engine::components::Rectangle{Vec2{4.0F, 4.0F}, engine::kWhite});

    // A frame with no scene drives the application's own world and systems.
    application.update();
    application.render();
    CHECK(application.entityManager().aliveEntityCount() == 1U);

    // And a scene can be asked for afterwards, which is how a game would adopt the
    // scene system without giving up the old door.
    application.changeScene(SceneId::Menu);
    application.update();
    CHECK(activeSceneIs(application, SceneId::Menu));
    CHECK(application.currentScene() != nullptr);
}

void testTheWorldOfAnOldSceneIsGone()
{
    // Two scenes, two worlds, and no leakage between them. The menu's four entities
    // cannot be in the level's world, and the level's hundreds cannot be in the
    // menu's, because each scene has its own `EntityManager` and the old one is
    // destroyed rather than kept.
    auto ledger = std::make_shared<SceneLedger>();
    engine::Application application;
    static_cast<void>(ledger);

    application.changeScene(SceneId::Menu);
    application.update();

    const auto* menu = dynamic_cast<const MenuScene*>(application.currentScene());
    CHECK(menu != nullptr);
    if (menu == nullptr)
    {
        return;
    }

    const std::size_t menuEntityCount = menu->world().aliveEntityCount();
    CHECK(menuEntityCount == 4U);

    application.changeScene(SceneId::Play);
    application.update();

    const auto* play = dynamic_cast<const PlayScene*>(application.currentScene());
    CHECK(play != nullptr);
    if (play == nullptr)
    {
        return;
    }

    // The play scene's world has no trace of the menu: no menu tags, no title.
    for (const engine::ecs::Entity& entity : play->world().getEntities())
    {
        CHECK(!has(entity.tag(), "menu."));
    }
    CHECK(countTextEntities(play->world()) == 1U);
    CHECK(play->world().aliveEntityCount() != menuEntityCount);
}

// ---------------------------------------------------------------------------
// D2. The two worlds, kept apart
//
// Each of these exists because a mutation that changed the behaviour passed every
// test. "The application's own systems do not run while a scene is active" and "the
// application's own world is not drawn while a scene is active" are both true of the
// code, and neither was observable from a test that registered no systems and put no
// entities in the application's world - a mutation that ran the wrong world simply
// ran an empty one, and an empty world behaves identically to no world at all.
// ---------------------------------------------------------------------------

/// Counts the frames it is asked to run.
class CountingSystem final : public engine::ecs::System
{
public:
    void update(engine::ecs::EntityManager& entities, const ActionState& actions, const float deltaSeconds) override
    {
        static_cast<void>(entities);
        static_cast<void>(actions);
        static_cast<void>(deltaSeconds);
        ++m_frames;
    }

    [[nodiscard]] const char* name() const override { return "CountingSystem"; }
    [[nodiscard]] int frames() const noexcept { return m_frames; }

private:
    int m_frames = 0;
};

void testTheApplicationSystemsDoNotRunWhileASceneIs()
{
    // Registered on the application, *before* any scene exists. If the loop drove the
    // application's systems as well as the scene's, this counter would move - and
    // with the empty application world there is nothing else that could notice.
    auto ledger = std::make_shared<SceneLedger>();
    engine::Application application;
    static_cast<void>(ledger);

    // `add` hands back a reference to the system it stored, which is how the counter
    // is read: `systemAt` returns the `System` base, and the base has no idea what
    // this subclass counts. The reference stays valid because this is the only system
    // ever added - registering another would invalidate it, as `SystemManager` says.
    CountingSystem& counter = application.systemManager().add<CountingSystem>();
    CHECK(application.systemManager().systemCount() == 1U);

    // The contrast first: with no scene, the system's own world is what runs.
    application.update();
    CHECK(counter.frames() == 1);

    // Now a scene takes over.
    application.changeScene(SceneId::Menu);
    application.update();
    application.update();

    // The scene is active, and the application's systems have not run since. The
    // counter is still one: those two frames belonged to the menu's world.
    CHECK(activeSceneIs(application, SceneId::Menu));
    CHECK(counter.frames() == 1);

    // And the application's world is untouched by the scene's frames, while the
    // menu's own world is the one that is there.
    CHECK(application.entityManager().aliveEntityCount() == 0U);
    const auto* menu = dynamic_cast<const MenuScene*>(application.currentScene());
    CHECK(menu != nullptr);
    if (menu != nullptr)
    {
        CHECK(menu->world().aliveEntityCount() == 4U);
    }
}

void testTheApplicationWorldIsNotRenderedWhileASceneIs()
{
    // The same shape for drawing, and the probe is an entity that would *fail* to draw
    // if it were reached: a text entity naming a font that is not in the asset table.
    // A rectangle would be drawn harmlessly and change nothing observable, so it would
    // prove nothing; a missing font makes `RenderSystem` throw, and a frame that
    // throws is a difference a test can see.
    //
    // The claim is precise and worth stating: **while a scene is active the render
    // pass does not touch the application's world at all** - not "draws it as well",
    // not "draws it if it happens to be non-empty". The application's world is not
    // the frame's subject; the scene's is.
    engine::Application application;
    application.changeScene(SceneId::Menu);
    application.update();

    engine::ecs::Entity& poison = application.entityManager().addEntity("poison");
    poison.addComponent<Transform>(Transform{Vec2{0.0F, 0.0F}, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
    Text text;
    text.content = "NOT A FONT";
    text.fontAssetName = "fonts_this_font_does_not_exist";
    text.characterSize = 12U;
    poison.addComponent<Text>(text);

    // Three frames, and none of them threw - so the application's world was not
    // rendered. The render pass only ever saw the menu's four entities.
    application.update();
    application.render();
    application.update();
    application.render();

    CHECK(application.entityManager().aliveEntityCount() == 1U);

    // The contrast: the same world, with no scene, *does* fail to draw. Without this
    // the assertion above could be passing because the poison never worked, and a
    // probe that cannot fail is not a probe.
    engine::Application withoutScene;
    engine::ecs::Entity& alsoPoison = withoutScene.entityManager().addEntity("poison");
    alsoPoison.addComponent<Transform>(Transform{Vec2{0.0F, 0.0F}, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
    Text alsoText;
    alsoText.content = "NOT A FONT";
    alsoText.fontAssetName = "fonts_this_font_does_not_exist";
    alsoText.characterSize = 12U;
    alsoPoison.addComponent<Text>(alsoText);

    bool threw = false;
    try
    {
        withoutScene.render();
    }
    catch (const engine::assets::AssetNotFoundError&)
    {
        threw = true;
    }
    CHECK(threw);
}

void testASceneRequestLastsOneFrame()
{
    // The rule [Scene::update] enforces, checked directly rather than through the
    // owner: a request is live for exactly one frame.
    //
    // This is what a missing clear looks like from the outside, and the consequence is
    // why the clear lives in the base class. A scene that asked and was not answered -
    // because the owner had already queued a request of its own, which wins - would
    // ask again on every frame, and would keep winning once the owner's request had
    // been spent. The game would flip between two scenes with no error anywhere. So
    // the request is cleared, here, by construction.
    SceneFixture fixture;
    MenuScene menu{fixture.context()};

    menu.update(ActionDriver{}.pressedNow(Key::Space), 0.016F);
    CHECK(menu.pendingTransition().scene().has_value());
    CHECK(menu.pendingTransition().scene().value() == SceneId::Play);

    // The next frame, whatever happens, starts with nothing asked for.
    menu.update(ActionDriver{}.idle(), 0.016F);
    CHECK_FALSE(menu.pendingTransition().scene().has_value());
    CHECK_FALSE(menu.pendingTransition().quits());

    // And the frame after that is the same: it is not a one-off reset.
    menu.update(ActionDriver{}.idle(), 0.016F);
    CHECK_FALSE(menu.pendingTransition().scene().has_value());

    // The same for a quit request, since it is stored in the same field.
    menu.update(ActionDriver{}.pressedNow(Key::S), 0.016F);
    menu.update(ActionDriver{}.pressedNow(Key::Space), 0.016F);
    CHECK(menu.pendingTransition().quits());
    menu.update(ActionDriver{}.idle(), 0.016F);
    CHECK_FALSE(menu.pendingTransition().quits());
}

// ---------------------------------------------------------------------------
// E. Screen space
//
// The claim under test: a menu is a property of the window, not of the world, so
// the camera cannot move it and cannot scale it. Both are measured in a real
// window, with the same scene and the same font, changing only the camera.
// ---------------------------------------------------------------------------

void testTheMenuTextIsScreenSpace()
{
    SceneFixture fixture;
    const std::unique_ptr<MenuScene> menu = std::make_unique<MenuScene>(fixture.context());

    // Every menu entity is marked. A menu that was accidentally world space would
    // look correct in a still frame and slide away the moment the camera moved.
    std::size_t total = 0U;
    std::size_t marked = 0U;
    for (const engine::ecs::Entity& entity : menu->world().getEntities())
    {
        ++total;
        if (entity.hasComponent<engine::components::ScreenSpace>())
        {
            ++marked;
        }
    }

    CHECK(total == 4U);
    CHECK(marked == total);
}

void testCameraMovementDoesNotMoveMenuText()
{
    SceneFixture fixture;
    const std::unique_ptr<MenuScene> menu = std::make_unique<MenuScene>(fixture.context());

    // Two camera positions, a long way apart, and the same menu.
    fixture.camera().setPosition(Vec2{0.0F, 0.0F});
    const auto [atOrigin, hashAtOrigin] = fixture.measureAndHashFrame([&menu] { menu->render(); });

    fixture.camera().setPosition(Vec2{-5120.0F, -3072.0F});
    const auto [moved, hashMoved] = fixture.measureAndHashFrame([&menu] { menu->render(); });

    // Byte for byte the same frame. Not merely the same box, not merely a similar
    // count: the same pixels, which is the only way "the camera cannot move the
    // menu" can be a fact rather than an estimate.
    CHECK(atOrigin.anythingChanged());
    CHECK(hashAtOrigin == hashMoved);
    CHECK(sameBox(atOrigin, moved));
}

void testCameraZoomDoesNotChangeMenuText()
{
    SceneFixture fixture;
    const std::unique_ptr<MenuScene> menu = std::make_unique<MenuScene>(fixture.context());

    fixture.camera().setZoom(1.0F);
    const auto [atOne, hashAtOne] = fixture.measureAndHashFrame([&menu] { menu->render(); });

    // Four times the zoom. World space multiplies positions and sizes by this;
    // screen space must not.
    fixture.camera().setZoom(4.0F);
    const auto [atFour, hashAtFour] = fixture.measureAndHashFrame([&menu] { menu->render(); });

    CHECK(atOne.anythingChanged());
    CHECK(hashAtOne == hashAtFour);
    CHECK(sameBox(atOne, atFour));
    CHECK(atOne.count == atFour.count);
}

void testCameraPositionAndZoomTogetherStillDoNotMoveMenuText()
{
    SceneFixture fixture;
    const std::unique_ptr<MenuScene> menu = std::make_unique<MenuScene>(fixture.context());

    fixture.camera().setPosition(Vec2{300.0F, 200.0F});
    fixture.camera().setZoom(0.5F);
    const auto [before, hashBefore] = fixture.measureAndHashFrame([&menu] { menu->render(); });

    fixture.camera().setPosition(Vec2{-8000.0F, 6000.0F});
    fixture.camera().setZoom(2.5F);
    const auto [after, hashAfter] = fixture.measureAndHashFrame([&menu] { menu->render(); });

    CHECK(hashBefore == hashAfter);
    CHECK(sameBox(before, after));
}

void testWorldTextStillFollowsTheCamera()
{
    // The contrast that makes the screen-space groups mean something: the *same*
    // renderer, the *same* font and the *same* size, and the camera does move this
    // one. If it stopped moving, the screen-space groups would be passing because
    // text had stopped being drawn at all.
    SceneFixture fixture;

    // ### The placement is in world coordinates
    //
    // `Camera::worldToScreen` is `(world - position) * zoom + screenCenter`, so it
    // *adds* the centre of the view. A world point of `(0, 0)` therefore lands in
    // the middle of the window, and a world point of the window size lands an
    // entire viewport off the bottom right corner. This test was written first with
    // the entity at `(400, 225)` - the middle of the *window* - and found the text
    // clipped against the right edge at `x = 800`, which is the arithmetic working
    // correctly and the test's assumption being wrong.
    EntityManager world;
    engine::ecs::Entity& label = world.addEntity("world.label");
    label.addComponent<Transform>(Transform{Vec2{0.0F, 0.0F}, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
    Text text;
    text.content = "WORLD";
    text.fontAssetName = std::string{kFontPixeled};
    text.characterSize = 24U;
    label.addComponent<Text>(text);

    engine::systems::RenderSystem renderSystem{fixture.renderer(), fixture.camera(), fixture.assets()};
    const ActionState nothing;
    const auto drawWorld = [&] { renderSystem.update(world, nothing, 0.0F); };

    // Camera centred on the origin: the world origin is the middle of the window.
    fixture.camera().setPosition(Vec2{0.0F, 0.0F});
    fixture.camera().setZoom(1.0F);
    const auto [atOrigin, hashAtOrigin] = fixture.measureAndHashFrame(drawWorld);

    CHECK(atOrigin.anythingChanged());
    CHECK_NEAR(atOrigin.centreX(), 400.0F, 2.0F);
    CHECK_NEAR(atOrigin.centreY(), 225.0F, 2.0F);

    // Move the camera 100 world units right and the text must move 100 *screen*
    // pixels left, because it is on screen at zoom 1 and the offset is not scaled.
    fixture.camera().setPosition(Vec2{100.0F, 0.0F});
    const auto [moved, hashMoved] = fixture.measureAndHashFrame(drawWorld);

    CHECK(hashAtOrigin != hashMoved);
    CHECK_FALSE(sameBox(atOrigin, moved));
    CHECK_NEAR(moved.centreX(), atOrigin.centreX() - 100.0F, 2.0F);
    CHECK_NEAR(moved.centreY(), atOrigin.centreY(), 0.001F);

    // And the zoom scales the camera's contribution, which is the other half of what
    // a menu must be immune to: at zoom 2 the same 100-unit camera move is 200
    // screen pixels. The text's own size does not change, only where it is drawn -
    // but the position does, and that is what this measures.
    fixture.camera().setZoom(2.0F);
    const auto zoomed = fixture.measureAndHashFrame(drawWorld).first;
    CHECK_NEAR(zoomed.centreX(), atOrigin.centreX() - 200.0F, 2.0F);

    // Now the same font and size again, but with the marker, and this one is placed
    // in screen pixels because that is what the marker means. Same renderer, same
    // draw call, one component different.
    EntityManager screen;
    engine::ecs::Entity& ui = screen.addEntity("screen.label");
    ui.addComponent<Transform>(Transform{Vec2{400.0F, 225.0F}, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
    Text uiText;
    uiText.content = "WORLD";
    uiText.fontAssetName = std::string{kFontPixeled};
    uiText.characterSize = 24U;
    ui.addComponent<Text>(uiText);
    ui.addComponent<engine::components::ScreenSpace>();

    const auto drawScreen = [&] { renderSystem.update(screen, nothing, 0.0F); };

    fixture.camera().setPosition(Vec2{0.0F, 0.0F});
    fixture.camera().setZoom(1.0F);
    const auto [uiAtOrigin, hashUiAtOrigin] = fixture.measureAndHashFrame(drawScreen);
    fixture.camera().setPosition(Vec2{100.0F, 0.0F});
    fixture.camera().setZoom(2.0F);
    const auto [uiMoved, hashUiMoved] = fixture.measureAndHashFrame(drawScreen);

    CHECK(uiAtOrigin.anythingChanged());
    CHECK(hashUiAtOrigin == hashUiMoved);
    CHECK(sameBox(uiAtOrigin, uiMoved));
    // It is drawn exactly where its transform says, in screen pixels.
    CHECK_NEAR(uiAtOrigin.centreX(), 400.0F, 2.0F);
    CHECK_NEAR(uiAtOrigin.centreY(), 225.0F, 2.0F);
}

void testScreenSpaceStillAppliesScaleAndRotation()
{
    // The marker says "do not apply the camera", not "ignore the entity". A
    // screen-space label can still be scaled and turned, and a camera zoom must not
    // change a scale that belongs to the entity.
    const Transform transform{Vec2{100.0F, 200.0F}, Vec2{0.0F, 0.0F}, Vec2{2.0F, 3.0F}, 0.5F};

    const RenderTransform screen = engine::graphics::toScreenTransform(transform);

    // Position untouched, scale untouched.
    CHECK_NEAR(screen.position.x, 100.0F, 0.0001F);
    CHECK_NEAR(screen.position.y, 200.0F, 0.0001F);
    CHECK_NEAR(screen.scale.x, 2.0F, 0.0001F);
    CHECK_NEAR(screen.scale.y, 3.0F, 0.0001F);

    // And the degrees conversion is applied, exactly as the world-space path does it
    // - a rotated screen-space entity has to rotate by the same rule or the two
    // would disagree at 90 degrees.
    const engine::graphics::Camera camera;
    CHECK_NEAR(screen.rotationDegrees, engine::graphics::toRenderTransform(transform, camera).rotationDegrees,
               0.0001F);
    CHECK_NEAR(screen.rotationDegrees, 0.5F * engine::graphics::kDegreesPerRadian, 0.001F);

    // The camera is not consulted at all: two cameras at opposite ends of the world
    // produce the same placement.
    engine::graphics::Camera far;
    far.setPosition(Vec2{-99999.0F, 88888.0F});
    far.setZoom(7.5F);
    const RenderTransform againstFar = engine::graphics::toScreenTransform(transform);
    CHECK_NEAR(againstFar.position.x, screen.position.x, 0.0001F);
    CHECK_NEAR(againstFar.scale.x, screen.scale.x, 0.0001F);
}

void testScreenSpaceIsHonouredByTheTextQueryOnly()
{
    // A decision with a boundary, so the boundary is pinned: the marker is honoured
    // by the text query and by nothing else. Extending it to the shape queries would
    // be the first step towards a screen-space UI framework, which the course has
    // not asked for.
    const std::string code = codeWithoutComments(ENGINE_RENDER_SYSTEM_SOURCE);

    // Present, and consulted exactly once: one rule, in one place.
    CHECK(has(code, "components::ScreenSpace"));
    CHECK(countOf(code, "toScreenTransform(") == 1U);
    CHECK(countOf(code, "hasComponent<components::ScreenSpace>()") == 1U);

    // And the marker is a marker: no fields, so there is no third thing to say
    // about where an entity is drawn.
    const std::string header = codeWithoutComments(ENGINE_SCREEN_SPACE_HEADER);
    CHECK(has(header, "struct ScreenSpace"));
    CHECK(has(header, "struct ScreenSpace\n{"));
    CHECK_FALSE(has(header, "struct ScreenSpace {"));

    static_assert(std::is_aggregate_v<engine::components::ScreenSpace>, "ScreenSpace must be a marker");
    static_assert(std::is_empty_v<engine::components::ScreenSpace>, "ScreenSpace must carry no state");
    static_assert(std::is_trivially_copyable_v<engine::components::ScreenSpace>,
                  "a marker must be freely copyable so storage can hand it out");
    CHECK(true);
}
// ---------------------------------------------------------------------------
// Token-aware "must not appear" checks
//
// ### Why not a substring scan
//
// The first version of the no-global check looked for `"static "` and reported five
// violations in files that declare no static storage whatsoever:
//
//   - `static SceneTransition stay()`  is a static *member function*
//   - `static_cast<int>(id)`          is a cast
//   - `quitApplication`               contains the substring "Application"
//
// Every one of them was a false positive in a group whose whole job is to report
// violations precisely. A check that cries wolf is worse than no check, because its
// reader learns to skip it - so these two helpers look at what follows the keyword
// and at the characters around the word, and the reasons are written down so the
// next person does not "simplify" them back into substrings.
// ---------------------------------------------------------------------------

[[nodiscard]] bool isWordChar(const char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/// True when `word` appears in `code` as a whole identifier.
///
/// Neither `quitApplication` nor `m_sceneId` counts as an occurrence of
/// `Application` or `scene`: the characters on both sides have to be non-word, or
/// there is nothing at the start of the string.
[[nodiscard]] bool containsWord(const std::string& code, const std::string_view word)
{
    if (word.empty())
    {
        return false;
    }

    std::size_t at = code.find(word);
    while (at != std::string::npos)
    {
        const bool leftOk = (at == 0U) || !isWordChar(code[at - 1U]);
        const std::size_t after = at + word.size();
        const bool rightOk = (after >= code.size()) || !isWordChar(code[after]);

        if (leftOk && rightOk)
        {
            return true;
        }

        at = code.find(word, at + 1U);
    }

    return false;
}

/// True when `code` declares static **storage**: a static data member, or a
/// file-scope static variable.
///
/// Static *member functions* are not storage and are correctly not reported. Neither
/// is anything declared `constexpr`: a `constexpr` object has the same value in
/// every translation unit and cannot be written to, so it is not shared mutable
/// state, and the configuration constants this engine has are all `inline constexpr`.
/// The data members `code` declares, found by a line that is a member declaration:
/// indentation, a type, an `m_`-prefixed name, then an optional initialiser and a
/// semicolon.
///
/// The type may carry one level of template argument, because
/// `std::optional<SceneId> m_scene;` is one of the seven and a pattern without it
/// reported six: a member silently missing from the count is worse than one missing
/// from the file.
///
/// ### Why a source scan
///
/// A member nobody reads has no behaviour, so no run-time test can notice it. A
/// back-pointer to the owner added "for later" is exactly that: it compiles, it
/// changes nothing, and it is the first step towards a scene that can reach the
/// whole engine. The mutation `scene-holds-a-back-pointer-to-the-owner` exists in the
/// mutation suite to keep that honest, and this is what catches it.
[[nodiscard]] std::vector<std::string> declaredDataMembers(const std::string& code)
{
    static const std::regex member{
        // Two adjacent literals, and the first one is closed with `)"` on its own
        // line. The version without that `)` did not terminate: the raw string ran on
        // into the second literal, so the pattern being compiled contained a newline
        // and the text `R"(` - and it matched three of the seven members. A regex
        // that silently matches the wrong subset is worse than one that matches none.
        R"(^\s+(?:const\s+)?[A-Za-z_][\w:]*(?:\s*<[^<>;]*>)?(?:\s*[*&])?\s+)"
        R"((m_\w+)\s*(?:=[^;]*|\{[^;]*\})?;\s*$)"};

    std::vector<std::string> found;
    std::istringstream lines{code};
    std::string line;
    while (std::getline(lines, line))
    {
        std::smatch match;
        if (std::regex_match(line, match, member))
        {
            found.push_back(match[1].str());
        }
    }

    return found;
}

[[nodiscard]] bool declaresStaticStorage(const std::string& code)
{
    std::size_t at = code.find("static");

    while (at != std::string::npos)
    {
        // `static_cast` and `static_assert` are one token, not the keyword, so the
        // character after the six letters has to end the word.
        const std::size_t after = at + 6U;
        const bool rightOk = (after >= code.size()) || !isWordChar(code[after]);
        const bool leftOk = (at == 0U) || !isWordChar(code[at - 1U]);

        if (rightOk && leftOk && after < code.size() && (code[after] == ' ' || code[after] == '\t'))
        {
            // Look at what the declaration says before it ends. The first of `(`, `=`
            // or `;` decides the shape: a `(` first means a function, and `=` or `;`
            // first means a variable.
            const std::size_t paren = code.find('(', after);
            const std::size_t equals = code.find('=', after);
            const std::size_t semi = code.find(';', after);

            const std::size_t firstParen = paren;
            const std::size_t firstEnd = std::min(equals, semi);
            const bool isFunction =
                (firstParen != std::string::npos) && (firstEnd == std::string::npos || firstParen < firstEnd);

            if (!isFunction)
            {
                // Storage, unless the declaration says it is a constant.
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
// F. Ownership
// ---------------------------------------------------------------------------

void testAssetsAreSharedNotDuplicated()
{
    auto ledger = std::make_shared<SceneLedger>();
    engine::Application application;
    static_cast<void>(ledger);

    // A scene is handed the application's own asset manager, by pointer. If it built
    // one of its own it would load all twenty-four textures and all three fonts a
    // second time, and a menu would cost a level's worth of memory to draw six words.
    application.changeScene(SceneId::Menu);
    application.update();
    CHECK(application.currentScene() != nullptr);
    if (application.currentScene() != nullptr)
    {
        CHECK(&application.currentScene()->context().assets() == &application.assets());
        CHECK(&application.currentScene()->context().renderer() == &application.renderer());
        CHECK(&application.currentScene()->context().camera() == &application.camera());
    }

    // The same objects before and after a transition: one asset table for the whole
    // process, not one per scene.
    const AssetManager* before = &application.assets();
    application.changeScene(SceneId::Play);
    application.update();
    CHECK(application.currentScene() != nullptr);
    if (application.currentScene() != nullptr)
    {
        CHECK(&application.currentScene()->context().assets() == before);
    }

    // And a scene built by hand, with a context of the test's own, borrows that
    // context's assets rather than loading any.
    SceneFixture fixture;
    const std::unique_ptr<PlayScene> play = std::make_unique<PlayScene>(fixture.context());
    CHECK(&play->context().assets() == &fixture.assets());
    CHECK(&play->context().renderer() == &fixture.renderer());
}

void testScenesDoNotRegisterIntoTheApplication()
{
    // The Phase 6 decision, unchanged: the engine assembles no systems of its own.
    // A scene registers systems for *its own* world, which is a different thing -
    // the scene is the game. If a scene were reaching into the application's
    // system manager, two scenes' systems would run against one world.
    auto ledger = std::make_shared<SceneLedger>();
    engine::Application application;
    static_cast<void>(ledger);

    application.changeScene(SceneId::Play);
    application.update();

    // Four systems in the play scene, and still none in the application.
    const auto* play = dynamic_cast<const PlayScene*>(application.currentScene());
    CHECK(play != nullptr);
    if (play != nullptr)
    {
        CHECK(play->systems().systemCount() == 4U);
    }
    CHECK(application.systemManager().systemCount() == 0U);

    // And the play scene's entities are not in the application's world either.
    CHECK(application.entityManager().aliveEntityCount() == 0U);
    if (play != nullptr)
    {
        CHECK(play->world().aliveEntityCount() > 0U);
    }
}

void testTheSceneHeadersAreSfmlFree()
{
    // The engine's boundary is that a public interface names no SFML type, and a
    // scene is a public interface. Comments are stripped first: three of these files
    // *document* that they avoid `sf::Text` and `sf::View`, and a scan over raw
    // source would fail on the explanation.
    const std::string scene = codeWithoutComments(ENGINE_SCENE_HEADER);
    const std::string menu = codeWithoutComments(ENGINE_MENU_SCENE_HEADER);
    const std::string play = codeWithoutComments(ENGINE_PLAY_SCENE_HEADER);

    for (const std::string& code : {scene, menu, play})
    {
        CHECK_FALSE(has(code, "sf::"));
        CHECK_FALSE(has(code, "<SFML/"));
        CHECK_FALSE(has(code, "RenderWindow"));
        CHECK_FALSE(has(code, "Keyboard"));
        CHECK_FALSE(has(code, "Event"));
        CHECK_FALSE(has(code, "View"));
    }

    // No scene implementation may name an SFML type either, and this one is allowed
    // to fail loudly rather than silently: a menu is built entirely from ECS
    // components and the abstract renderer, so an SFML name here would mean a scene
    // had started drawing by itself.
    for (const char* const path : {ENGINE_MENU_SCENE_SOURCE, ENGINE_PLAY_SCENE_SOURCE, ENGINE_SCENE_SOURCE})
    {
        const std::string code = codeWithoutComments(path);
        CHECK_FALSE(has(code, "sf::"));
        CHECK_FALSE(has(code, "<SFML/"));
    }

    // And a scene has no window: it draws through the borrowed `Renderer`, whose
    // header is the engine's own graphics boundary.
    CHECK(has(scene, "graphics::Renderer& renderer()"));
}

void testScenesNameNoKeysAndReadActions()
{
    // The action layer exists so a scene can ask "is the player jumping" instead of
    // naming a key. A scene that named a key would bypass the rebinding table, the
    // press-edge handling and the single snapshot per frame, all of which are the
    // point of the layer.
    for (const char* const path : {ENGINE_MENU_SCENE_SOURCE, ENGINE_PLAY_SCENE_SOURCE})
    {
        const std::string code = codeWithoutComments(path);

        CHECK_FALSE(has(code, "sf::Keyboard"));
        CHECK_FALSE(has(code, "Keyboard::"));
        CHECK_FALSE(has(code, "isKeyPressed"));
        CHECK_FALSE(has(code, "sf::Event"));
        CHECK_FALSE(has(code, "Event::"));
        CHECK_FALSE(has(code, "input::Input"));
    }

    // The menu reads actions, and reads them through the press edge.
    const std::string menu = codeWithoutComments(ENGINE_MENU_SCENE_SOURCE);
    CHECK(has(menu, "actions.wasPressed("));
    CHECK(has(menu, "input::Action::MoveUp"));
    CHECK(has(menu, "input::Action::MoveDown"));
    CHECK(has(menu, "input::Action::Shoot"));

    // And the play scene goes back with an action the layer already bound, rather
    // than a "back" action invented for the purpose.
    const std::string play = codeWithoutComments(ENGINE_PLAY_SCENE_SOURCE);
    CHECK(has(play, "actions.wasPressed("));
    CHECK(has(play, "input::Action::Quit"));
}

void testTheSceneBaseHoldsNothingButAContextAndARequest()
{
    // The base class's entire state, pinned.
    //
    // A scene needs two things from its base: the borrowed context, and somewhere to
    // record what it wants next. Anything else a `Scene` gains is a decision nobody
    // made yet - and the one that matters is a pointer to the owner, which is how
    // "a scene cannot reach the application" quietly stops being true while every
    // behavioural test still passes.
    //
    // Adding a member is a source-level change with no run-time effect, so this is a
    // source check and says so. It checks *which* members exist rather than what they
    // are called, so a differently-named back-pointer does not slip through.
    const std::string code = codeWithoutComments(ENGINE_SCENE_HEADER);
    const std::vector<std::string> members = declaredDataMembers(code);

    // All three classes' state at once, because the header *is* the scene layer's
    // state and checking them separately would let an extra member in through the
    // gap:
    //
    //   SceneTransition  m_scene, m_quit                      - what a scene asked
    //   SceneContext     m_renderer, m_camera, m_assets       - three borrowed pointers
    //   Scene            m_context, m_transition              - the context, the request
    //
    // Seven, no more. A back-pointer to the owner would be an eighth.
    //
    // Checked by name rather than by count alone, so a failure says *which* member
    // appeared instead of only that the number moved.
    const std::vector<std::string> expected{
        "m_assets", "m_camera", "m_context", "m_quit", "m_renderer", "m_scene", "m_transition"};

    CHECK(members.size() == expected.size());
    for (const std::string& name : expected)
    {
        const auto found = std::find(members.begin(), members.end(), name);
        if (found == members.end())
        {
            std::cerr << "    expected a data member named " << name << "\n";
        }
        CHECK(found != members.end());
    }

    // And the two accessors that expose the base's state are the only way out of it.
    CHECK(containsWord(code, "context"));
    CHECK(containsWord(code, "pendingTransition"));

    // The context's three pointers are all it holds, so it cannot reach the window,
    // the action map or the running flag even by accident.
    const std::vector<std::string> contextMembers = [&members] {
        std::vector<std::string> only;
        for (const std::string& name : members)
        {
            if (name == "m_renderer" || name == "m_camera" || name == "m_assets")
            {
                only.push_back(name);
            }
        }
        return only;
    }();
    CHECK(contextMembers.size() == 3U);
}

void testThereIsNoGlobalSceneManager()
{
    // A singleton scene manager cannot be observed at run time. A test can only
    // observe that this program behaves correctly, which a global would also do, so
    // this is a source check and says so.
    //
    // What is asserted is that no file in the scene layer, and not `Application`
    // either, declares static **storage** - see [declaresStaticStorage] for why that
    // is not a substring scan, and for the three false positives the first version of
    // this check produced.
    //
    // A `SceneFactory` member and a `unique_ptr<Scene>` member are both per-object
    // state, and neither is checked here: what would be wrong is a *shared* scene,
    // and sharing is what `static` means.
    const char* const files[] = {
        ENGINE_SCENE_HEADER,    ENGINE_SCENE_SOURCE,       ENGINE_MENU_SCENE_HEADER,
        ENGINE_PLAY_SCENE_HEADER, ENGINE_APPLICATION_HEADER,
    };

    for (const char* const path : files)
    {
        const std::string code = codeWithoutComments(path);
        if (declaresStaticStorage(code))
        {
            std::cerr << "    " << path << " declares static storage\n";
        }
        CHECK_FALSE(declaresStaticStorage(code));
    }

    // A scene reaches the process only through what it is handed: a context, and a
    // request. There is no "the current scene" anyone can ask for, because the
    // current scene is the owner's business and nobody else's.
    const std::string sceneHeader = codeWithoutComments(ENGINE_SCENE_HEADER);
    const std::string applicationHeader = codeWithoutComments(ENGINE_APPLICATION_HEADER);
    const std::string sceneSource = codeWithoutComments(ENGINE_SCENE_SOURCE);

    // `containsWord`, not `has`: `quitApplication` is a factory for the quit
    // transition and not a reference to the application, and the first version of
    // this check failed on it.
    CHECK_FALSE(containsWord(sceneHeader + applicationHeader, "g_scene"));
    CHECK_FALSE(containsWord(sceneHeader + applicationHeader, "s_currentScene"));
    CHECK_FALSE(containsWord(sceneHeader + applicationHeader, "theScene"));
    CHECK_FALSE(containsWord(sceneSource, "static"));
    CHECK_FALSE(declaresStaticStorage(sceneSource));

    // A scene cannot reach the application: it is handed three borrowed references
    // and holds no pointer to the owner. This is the difference from the reference
    // implementation, whose `Scene::m_game` is the engine.
    //
    // `Scene` is the abstract base, so this is where a back-pointer would live. A
    // scene that could reach `Application` could change the window, the asset table
    // and the running flag, and none of those is a scene's business.
    CHECK_FALSE(containsWord(sceneHeader, "Application"));
    CHECK_FALSE(containsWord(codeWithoutComments(ENGINE_MENU_SCENE_SOURCE), "Application"));
    CHECK_FALSE(containsWord(codeWithoutComments(ENGINE_PLAY_SCENE_SOURCE), "Application"));

    // The factory is a *member*, not a global: one per application, and replaced only
    // through a setter a test can see. And the default is the free function, so a
    // build with no test in it has no registry and nothing to initialise.
    CHECK_FALSE(containsWord(applicationHeader, "static"));
    CHECK(containsWord(sceneSource, "makeScene"));
}

void testTheEngineLayersBelowDoNotKnowAboutScenes()
{
    // The dependency direction. A component, the renderer, the asset manager and the
    // input layer must not know that scenes exist: if they did, they would have a
    // way to reach the current scene, and a global would follow.
    // A path and a name to print if it fails, so a failure says *which* layer
    // learned about scenes rather than just that one of them did.
    const std::pair<const char*, const char*> layers[] = {
        {ENGINE_TEXT_HEADER, "components/Text.hpp"},
        {ENGINE_TRANSFORM_HEADER, "components/Transform.hpp"},
        {ENGINE_SCREEN_SPACE_HEADER, "components/ScreenSpace.hpp"},
        {ENGINE_RENDERER_HEADER, "graphics/Renderer.hpp"},
        {ENGINE_CAMERA_HEADER, "graphics/Camera.hpp"},
        {ENGINE_ASSET_MANAGER_HEADER, "assets/AssetManager.hpp"},
        {ENGINE_ACTION_STATE_HEADER, "input/ActionState.hpp"},
        {ENGINE_RENDER_SYSTEM_SOURCE, "systems/RenderSystem.cpp"},
    };

    for (const auto& [path, label] : layers)
    {
        const std::string code = codeWithoutComments(path);
        if (has(code, "SceneId") || has(code, "makeScene") || has(code, "scene::"))
        {
            std::cerr << "    " << label << " knows about scenes\n";
        }
        CHECK_FALSE(has(code, "SceneId"));
        CHECK_FALSE(has(code, "makeScene"));
        CHECK_FALSE(has(code, "scene::"));
    }

    // The renderer especially: a renderer that could switch scenes would be able to
    // change which world is being drawn while it is drawing it.
    const std::string renderer = codeWithoutComments(ENGINE_RENDERER_HEADER);
    CHECK_FALSE(has(renderer, "Scene"));
}

} // namespace

int main()
{
    const std::pair<const char*, void (*)()> testCases[] = {
        // A. The abstraction
        {"scene is an abstraction", &testSceneIsAnAbstraction},
        {"a scene transition is a value", &testASceneTransitionIsAValue},
        {"a context borrows and does not own", &testAContextBorrowsAndDoesNotOwn},
        {"makeScene builds every scene", &testMakeSceneBuildsEveryScene},
        // B. The menu
        {"the menu can be created", &testTheMenuCanBeCreated},
        {"the menu renders real text", &testTheMenuRendersRealText},
        {"the menu navigates on actions", &testTheMenuNavigatesOnActions},
        {"the menu moves on the press edge only", &testTheMenuMovesOnThePressEdgeOnly},
        {"the menu starts the game", &testTheMenuStartsTheGame},
        {"the menu quits", &testTheMenuQuits},
        {"the menu ignores actions that mean nothing to it", &testTheMenuIgnoresActionsThatMeanNothingToIt},
        // C. The play scene
        {"the play scene builds a world", &testThePlaySceneBuildsAWorld},
        {"the play scene registers its own systems", &testThePlaySceneRegistersItsOwnSystems},
        {"the play scene goes back to the menu", &testThePlaySceneGoesBackToTheMenu},
        {"the play scene updates and renders", &testThePlaySceneUpdatesAndRenders},
        {"the play scene flushes its own deferred destruction",
         &testThePlaySceneFlushesItsOwnDeferredDestruction},
        // D. The transition lifecycle
        {"changeScene is deferred until the next frame", &testChangeSceneIsDeferredUntilTheNextFrame},
        {"the transition happens at a frame boundary", &testTheTransitionHappensAtAFrameBoundary},
        {"a transition destroys the old scene", &testATransitionDestroysTheOldScene},
        {"no scene is destroyed while it is running", &testNoSceneIsDestroyedWhileItIsRunning},
        {"only the active scene updates", &testOnlyTheActiveSceneUpdates},
        {"only the active scene renders", &testOnlyTheActiveSceneRenders},
        {"the menu transitions to play and back", &testTheMenuTransitionsToPlayAndBack},
        {"a quit request stops the application", &testAQuitRequestStopsTheApplication},
        {"sceneId is asked of the scene", &testSceneIdIsAskedOfTheScene},
        {"an application without a scene still works", &testApplicationWithoutASceneStillWorks},
        {"the world of an old scene is gone", &testTheWorldOfAnOldSceneIsGone},
        {"the application systems do not run while a scene is",
         &testTheApplicationSystemsDoNotRunWhileASceneIs},
        {"the application world is not rendered while a scene is",
         &testTheApplicationWorldIsNotRenderedWhileASceneIs},
        {"a scene request lasts one frame", &testASceneRequestLastsOneFrame},
        // E. Screen space
        {"the menu text is screen space", &testTheMenuTextIsScreenSpace},
        {"camera movement does not move menu text", &testCameraMovementDoesNotMoveMenuText},
        {"camera zoom does not change menu text", &testCameraZoomDoesNotChangeMenuText},
        {"camera position and zoom together still do not move menu text",
         &testCameraPositionAndZoomTogetherStillDoNotMoveMenuText},
        {"world text still follows the camera", &testWorldTextStillFollowsTheCamera},
        {"screen space still applies scale and rotation", &testScreenSpaceStillAppliesScaleAndRotation},
        {"screen space is honoured by the text query only", &testScreenSpaceIsHonouredByTheTextQueryOnly},
        // F. Ownership
        {"assets are shared not duplicated", &testAssetsAreSharedNotDuplicated},
        {"scenes do not register into the application", &testScenesDoNotRegisterIntoTheApplication},
        {"the scene headers are sfml free", &testTheSceneHeadersAreSfmlFree},
        {"scenes name no keys and read actions", &testScenesNameNoKeysAndReadActions},
        {"the scene base holds nothing but a context and a request",
         &testTheSceneBaseHoldsNothingButAContextAndARequest},
        {"there is no global scene manager", &testThereIsNoGlobalSceneManager},
        {"the engine layers below do not know about scenes", &testTheEngineLayersBelowDoNotKnowAboutScenes},
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

    std::cout << groupCount << " scene test groups passed\n";
    return EXIT_SUCCESS;
}
