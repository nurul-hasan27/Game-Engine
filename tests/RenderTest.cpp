#include "engine/Application.hpp"
#include "engine/Color.hpp"
#include "engine/assets/AssetManager.hpp"
#include "engine/assets/SfmlAssetManager.hpp"
#include "engine/components/Rectangle.hpp"
#include "engine/components/Texture.hpp"
#include "engine/components/Transform.hpp"
#include "engine/EngineConfig.hpp"
#include "engine/ecs/EntityManager.hpp"
#include "engine/graphics/Camera.hpp"
#include "engine/graphics/RenderTransform.hpp"
#include "engine/graphics/Renderer.hpp"

#include <optional>
#include "engine/systems/RenderSystem.hpp"
#include "engine/input/Input.hpp"
#include "engine/math/Vec2.hpp"

// SFML appears here, and only here, because this file contains a real graphics
// integration test. Everything above the integration section is SFML free and is
// what the engine itself depends on.
#include <SFML/Graphics/Color.hpp>
#include <SFML/Graphics/Image.hpp>
#include <SFML/Graphics/RenderWindow.hpp>
#include <SFML/Graphics/Texture.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{

// ---------------------------------------------------------------------------
// A recording renderer, so RenderSystem can be tested with no window at all.
//
// It implements the same public interface as the SFML renderer, which is the
// point of having an interface: the system's behaviour is verifiable in
// isolation.
// ---------------------------------------------------------------------------

struct DrawCall
{
    engine::Vec2 size;
    engine::Color color;
    engine::graphics::RenderTransform placement;
};

/// A recorded texture draw. Records the handle by address rather than by value,
/// because a handle is non-copyable by design and because identity is the thing
/// worth asserting: a system must submit the asset the name resolved to, not a
/// copy of it.
struct TextureDrawCall
{
    const engine::assets::Texture* texture = nullptr;
    engine::graphics::RenderTransform placement;

    /// The region of the image that was drawn, or `std::nullopt` for all of it.
    /// Recorded because "did the animation query select the right frame" is not
    /// observable any other way, and a system that quietly drew the whole image
    /// would otherwise look identical to one that did the right thing.
    std::optional<engine::IntRect> source;
};

class RecordingRenderer final : public engine::graphics::Renderer
{
public:
    void beginFrame() override
    {
        ++m_beginFrames;
        m_cleared = false;
        // Order is recorded across a whole frame, because the question of which
        // query submits first is exactly what a texture test needs to see.
        m_order.clear();
    }

    void clear(const engine::Color& color) override
    {
        m_cleared = true;
        m_clearColor = color;
    }

    void drawRectangle(const engine::Vec2& size, const engine::Color& color,
                       const engine::graphics::RenderTransform& placement) override
    {
        m_draws.push_back(DrawCall{size, color, placement});
        m_order.emplace_back("rectangle");
    }

    void drawTexture(const engine::assets::Texture& texture, const engine::graphics::RenderTransform& placement,
                     const std::optional<engine::IntRect>& source) override
    {
        m_textureDraws.push_back(TextureDrawCall{&texture, placement, source});
        m_order.emplace_back(source.has_value() ? "texture-region" : "texture");
    }

    void endFrame() override { ++m_endFrames; }

    [[nodiscard]] std::uint64_t frameCount() const noexcept override { return m_endFrames; }

    [[nodiscard]] const std::vector<DrawCall>& draws() const noexcept { return m_draws; }
    [[nodiscard]] const std::vector<TextureDrawCall>& textureDraws() const noexcept { return m_textureDraws; }
    [[nodiscard]] const std::vector<std::string>& order() const noexcept { return m_order; }
    [[nodiscard]] bool cleared() const noexcept { return m_cleared; }
    [[nodiscard]] const engine::Color& clearColor() const noexcept { return m_clearColor; }
    [[nodiscard]] std::size_t beginFrameCount() const noexcept { return m_beginFrames; }
    [[nodiscard]] std::size_t endFrameCount() const noexcept { return m_endFrames; }

    void reset()
    {
        m_draws.clear();
        m_textureDraws.clear();
        m_order.clear();
        m_cleared = false;
        m_beginFrames = 0;
        m_endFrames = 0;
    }

private:
    std::vector<DrawCall> m_draws;
    std::vector<TextureDrawCall> m_textureDraws;
    std::vector<std::string> m_order;
    engine::Color m_clearColor{};
    std::size_t m_beginFrames = 0;
    std::size_t m_endFrames = 0;
    bool m_cleared = false;
};

namespace assets = engine::assets;
namespace components = engine::components;
namespace graphics = engine::graphics;
namespace config = engine::config;
using engine::Application;
using engine::Color;
using engine::IntRect;
using engine::isEmpty;
using engine::kWholeImage;
using engine::Vec2;
using engine::components::Rectangle;
using engine::components::Transform;
using engine::ecs::Entity;
using engine::ecs::EntityManager;
using engine::graphics::Camera;
using engine::graphics::RenderTransform;
using engine::input::Input;
using engine::graphics::Renderer;
using engine::systems::RenderSystem;

/// A test double asset manager.
///
/// Hands back stable, empty handles and records which names were asked for. Empty
/// is sufficient here because the recording renderer never looks inside a handle,
/// only records its address; the groups that need real pixels use a real
/// `SfmlAssetManager` and a real window instead.
class FakeAssetManager final : public assets::AssetManager
{
public:
    const assets::Texture& texture(const std::string_view name) const override
    {
        m_requested.emplace_back(name);

        const auto found = m_textures.find(std::string{name});
        if (found == m_textures.end())
        {
            throw assets::AssetNotFoundError{"no texture named '" + std::string{name} + "'"};
        }

        return found->second;
    }

    const assets::Font& font(const std::string_view name) const override
    {
        const std::string key{name};
        const auto found = m_fonts.find(key);
        if (found == m_fonts.end())
        {
            throw assets::AssetNotFoundError{"no font named '" + key + "'"};
        }

        return found->second;
    }

    const assets::Animation& animation(const std::string_view name) const override
    {
        const auto found = m_animations.find(std::string{name});
        if (found == m_animations.end())
        {
            throw assets::AssetNotFoundError{"no animation named '" + std::string{name} + "'"};
        }

        return found->second;
    }

    /// Pre-declares a name, so `texture(name)` will resolve.
    void declare(const std::string& name) { m_textures.emplace(name, assets::Texture{}); }

    /// Pre-declares an animation, so `animation(name)` will resolve.
    ///
    /// The frame width and height are passed in rather than derived, because this
    /// double has no image behind it and the tests that care about real geometry
    /// use a real manager and a real window.
    void declareAnimation(const std::string& name, const std::string& textureName, const std::uint32_t frameCount,
                          const std::uint32_t speed, const int frameWidth, const int frameHeight)
    {
        m_animations.emplace(name, assets::Animation{textureName, frameCount, speed, frameWidth, frameHeight});
    }

    [[nodiscard]] const std::vector<std::string>& requested() const noexcept { return m_requested; }

private:
    mutable std::map<std::string, assets::Texture> m_textures;
    mutable std::map<std::string, assets::Font> m_fonts;

    /// Animations are values, so this needs no `mutable` dance for the same reason
    /// the textures do not: the map is only ever read.
    std::map<std::string, assets::Animation> m_animations;
    mutable std::vector<std::string> m_requested;
};

/// A test-only system that moves Transforms, to prove the render system and the
/// simulation systems compose.
class DriftSystem final : public engine::ecs::System
{
public:
    void update(engine::ecs::EntityManager& entities, engine::input::Input& input, const float deltaSeconds) override
    {
        (void)input;
        for (auto&& [entity, transform] : entities.query<engine::components::Transform>())
        {
            (void)entity;
            transform.position += transform.velocity * deltaSeconds;
        }
    }

    [[nodiscard]] const char* name() const override { return "DriftSystem"; }
};


/// The default camera: position `(0, 0)`, zoom `1`, viewport `(0, 0)`.
///
/// It is the **identity** on positions. A zero viewport puts the screen centre
/// at the origin, and zoom 1 changes nothing, so `worldToScreen(p) == p`. These
/// tests are about the Transform-to-RenderTransform mapping in isolation, so they
/// pass a default camera and assert that no camera transformation is applied to
/// them. The camera's own behaviour is covered by CameraTest.cpp.
///
/// Returned by value rather than held in a function-local static: the engine has
/// no global state, and a test helper that quietly introduced one would be
/// contradicting the very thing the rest of the suite asserts.
[[nodiscard]] Camera identityCamera() { return {}; }

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

void checkEqual(const std::string& actual, const std::string& expected, const char* const expression,
                const char* const file, const int line)
{
    if (actual != expected)
    {
        ++g_failureCount;
        std::cerr << "    " << file << ':' << line << ": CHECK(" << expression << ") failed"
                  << "\n      actual   = \"" << actual << "\"\n      expected = \"" << expected << "\"\n";
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
#define CHECK_STR(actual, expected) checkEqual((actual), (expected), #actual " == " #expected, __FILE__, __LINE__)
#define CHECK_NEAR(actual, expected) checkNear((actual), (expected), #actual " ~= " #expected, __FILE__, __LINE__)
#define CHECK_NEAR_VEC(actual, expected) checkNearVec((actual), (expected), #actual, __FILE__, __LINE__)

constexpr float kPi = 3.14159265358979323846F;

// ---------------------------------------------------------------------------
// Compile-time guarantees.
// ---------------------------------------------------------------------------

// Transform and Rectangle must stay free of SFML, which the SFML-free unit tests
// below already demonstrate by running without any SFML type in play. What we
// can assert here is that both remain plain data.
static_assert(std::is_trivially_copyable_v<Transform>, "Transform must stay plain data");
static_assert(std::is_trivially_copyable_v<Rectangle>, "Rectangle must stay plain data");
static_assert(std::is_aggregate_v<Rectangle>, "Rectangle must remain an aggregate");
static_assert(sizeof(Color) == 4 * sizeof(float), "Color must be four float channels");
static_assert(std::is_aggregate_v<Color>, "Color must remain an aggregate");
static_assert(std::is_trivially_copyable_v<RenderTransform>, "RenderTransform must be plain data");

// ---------------------------------------------------------------------------
// Rectangle component
// ---------------------------------------------------------------------------

void testRectangleDefaults()
{
    const Rectangle rectangle;

    CHECK(rectangle.size == Vec2(100.0F, 100.0F));
    CHECK(rectangle.color.red == 1.0F);
    CHECK(rectangle.color.alpha == 1.0F);
    CHECK(rectangle.size.x > 0.0F);
    CHECK(rectangle.size.y > 0.0F);
}

void testRectangleAggregateConstruction()
{
    const Rectangle rectangle{Vec2{32.0F, 48.0F}, Color{0.0F, 0.0F, 1.0F, 1.0F}};

    CHECK(rectangle.size == Vec2(32.0F, 48.0F));
    CHECK(rectangle.color.blue == 1.0F);
    CHECK(rectangle.color.red == 0.0F);
}

void testRectangleIsStoredOnEntities()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::Input input;
    Entity& entity = manager.addEntity("entity");

    entity.addComponent<Transform>();
    entity.addComponent<Rectangle>(Rectangle{Vec2{20.0F, 20.0F}, Color{1.0F, 0.0F, 0.0F, 1.0F}});

    CHECK(entity.hasComponent<Rectangle>());
    CHECK(entity.componentCount() == 2);
    CHECK(entity.getComponent<Rectangle>().size == Vec2(20.0F, 20.0F));

    // Two entities hold independent rectangles.
    Entity& other = manager.addEntity("other");
    other.addComponent<Rectangle>();
    CHECK(other.getComponent<Rectangle>().size == Vec2(100.0F, 100.0F));
    CHECK(entity.getComponent<Rectangle>().size == Vec2(20.0F, 20.0F));
}

// ---------------------------------------------------------------------------
// Transform to render mapping: the pure, unit-testable conversion
// ---------------------------------------------------------------------------

void testRenderTransformDefaults()
{
    const RenderTransform placement;

    CHECK(placement.position == Vec2(0.0F, 0.0F));
    CHECK(placement.scale == Vec2(1.0F, 1.0F));
    CHECK_NEAR(placement.rotationDegrees, 0.0F);
}

void testRenderTransformPositionMapping()
{
    Transform transform;
    transform.position = Vec2(320.0F, 180.0F);

    const RenderTransform placement = engine::graphics::toRenderTransform(transform, identityCamera());

    // Position passes through untouched, in pixels, no conversion or flipping.
    CHECK(placement.position == Vec2(320.0F, 180.0F));
}

void testRenderTransformScaleMapping()
{
    Transform transform;
    transform.scale = Vec2(2.0F, 0.5F);

    const RenderTransform placement = engine::graphics::toRenderTransform(transform, identityCamera());

    // Scale passes through per axis, unswapped and unswizzled.
    CHECK(placement.scale == Vec2(2.0F, 0.5F));
}

void testRenderTransformAngleConversion()
{
    Transform transform;

    transform.angle = 0.0F;
    CHECK_NEAR(engine::graphics::toRenderTransform(transform, identityCamera()).rotationDegrees, 0.0F);

    transform.angle = kPi / 2.0F;
    CHECK_NEAR(engine::graphics::toRenderTransform(transform, identityCamera()).rotationDegrees, 90.0F);

    transform.angle = kPi;
    CHECK_NEAR(engine::graphics::toRenderTransform(transform, identityCamera()).rotationDegrees, 180.0F);

    transform.angle = -kPi / 2.0F;
    CHECK_NEAR(engine::graphics::toRenderTransform(transform, identityCamera()).rotationDegrees, -90.0F);

    transform.angle = kPi / 4.0F;
    CHECK_NEAR(engine::graphics::toRenderTransform(transform, identityCamera()).rotationDegrees, 45.0F);
}

void testRenderTransformAngleIsNotFlipped()
{
    // Engine and screen space share the same axes in this phase, so the
    // conversion is a plain unit change. A negative here would be a double flip
    // and would rotate the wrong way on screen.
    Transform transform;
    transform.angle = kPi / 2.0F;

    const float degrees = engine::graphics::toRenderTransform(transform, identityCamera()).rotationDegrees;

    CHECK(degrees > 0.0F);
    CHECK_NEAR(degrees, 90.0F);
    CHECK_NEAR(engine::graphics::kDegreesPerRadian, 57.295779513082320876798154814105F);
}

void testRenderTransformMapsEverythingAtOnce()
{
    Transform transform{Vec2{10.0F, 20.0F}, Vec2{0.0F, 0.0F}, Vec2{3.0F, 4.0F}, kPi};

    const RenderTransform placement = engine::graphics::toRenderTransform(transform, identityCamera());

    CHECK(placement.position == Vec2(10.0F, 20.0F));
    CHECK(placement.scale == Vec2(3.0F, 4.0F));
    CHECK_NEAR(placement.rotationDegrees, 180.0F);
}

// ---------------------------------------------------------------------------
// RenderSystem, driven against the recording renderer: no window required
// ---------------------------------------------------------------------------

void testRenderSystemDrawsMatchingEntities()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::Input input;
    RecordingRenderer renderer;
    engine::graphics::Camera camera;
    FakeAssetManager assets;
    RenderSystem renderSystem{renderer, camera, assets};

    Entity& entity = manager.addEntity("entity");
    entity.addComponent<Transform>(Transform{Vec2{100.0F, 100.0F}, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
    entity.addComponent<Rectangle>(Rectangle{Vec2{40.0F, 20.0F}, Color{1.0F, 0.0F, 0.0F, 1.0F}});

    renderSystem.update(manager, input, 0.016F);

    CHECK(renderer.draws().size() == 1);
    if (renderer.draws().size() == 1)
    {
        const DrawCall& call = renderer.draws().front();
        CHECK(call.size == Vec2(40.0F, 20.0F));
        CHECK(call.color.red == 1.0F);
        CHECK(call.placement.position == Vec2(100.0F, 100.0F));
    }
}

void testRenderSystemIgnoresEntitiesMissingComponents()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::Input input;
    RecordingRenderer renderer;
    engine::graphics::Camera camera;
    FakeAssetManager assets;
    RenderSystem renderSystem{renderer, camera, assets};

    // Only a Transform: nothing to draw.
    Entity& transformOnly = manager.addEntity("transformOnly");
    transformOnly.addComponent<Transform>();

    // Only a Rectangle: nowhere to draw it.
    Entity& rectangleOnly = manager.addEntity("rectangleOnly");
    rectangleOnly.addComponent<Rectangle>();

    // Neither.
    manager.addEntity("bare");

    renderSystem.update(manager, input, 0.016F);

    CHECK(renderer.draws().empty());
}

void testRenderSystemIgnoresDeadEntities()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::Input input;
    RecordingRenderer renderer;
    engine::graphics::Camera camera;
    FakeAssetManager assets;
    RenderSystem renderSystem{renderer, camera, assets};

    Entity& alive = manager.addEntity("alive");
    alive.addComponent<Transform>();
    alive.addComponent<Rectangle>();

    Entity& doomed = manager.addEntity("doomed");
    doomed.addComponent<Transform>();
    doomed.addComponent<Rectangle>();

    manager.destroyEntity(doomed);

    renderSystem.update(manager, input, 0.016F);

    CHECK(renderer.draws().size() == 1);

    // After cleanup the dead entity is gone and the survivor is still drawn.
    // The recording renderer accumulates, so reset it to count this frame only.
    manager.update();
    renderer.reset();
    renderSystem.update(manager, input, 0.016F);
    CHECK(renderer.draws().size() == 1);
}

void testRenderSystemDrawsManyEntities()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::Input input;
    RecordingRenderer renderer;
    engine::graphics::Camera camera;
    FakeAssetManager assets;
    RenderSystem renderSystem{renderer, camera, assets};

    for (int i = 0; i < 16; ++i)
    {
        Entity& entity = manager.addEntity("entity");
        entity.addComponent<Transform>(Transform{Vec2{static_cast<float>(i), 0.0F}, Vec2{0.0F, 0.0F},
                                               Vec2{1.0F, 1.0F}, 0.0F});
        entity.addComponent<Rectangle>();
    }

    renderSystem.update(manager, input, 0.016F);

    CHECK(renderer.draws().size() == 16);
}

void testRenderSystemMapsTransformIntoDrawCalls()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::Input input;
    RecordingRenderer renderer;
    engine::graphics::Camera camera;
    FakeAssetManager assets;
    RenderSystem renderSystem{renderer, camera, assets};

    Entity& entity = manager.addEntity("entity");
    entity.addComponent<Transform>(Transform{Vec2{50.0F, 60.0F}, Vec2{0.0F, 0.0F}, Vec2{2.0F, 3.0F}, kPi / 2.0F});
    entity.addComponent<Rectangle>(Rectangle{Vec2{10.0F, 10.0F}, Color{0.0F, 1.0F, 0.0F, 1.0F}});

    renderSystem.update(manager, input, 0.016F);

    CHECK(renderer.draws().size() == 1);
    if (renderer.draws().size() == 1)
    {
        const DrawCall& call = renderer.draws().front();
        CHECK(call.placement.position == Vec2(50.0F, 60.0F));
        CHECK(call.placement.scale == Vec2(2.0F, 3.0F));
        CHECK_NEAR(call.placement.rotationDegrees, 90.0F);
        CHECK(call.color.green == 1.0F);
    }
}

void testRenderSystemAppliesTheCameraToDrawCalls()
{
    // The wiring itself: RenderSystem must hand the renderer a *screen* position,
    // not the world position it read out of the Transform. A default camera is
    // the identity and would hide a system that ignored the camera entirely.
    EntityManager manager;
    [[maybe_unused]] engine::input::Input input;
    RecordingRenderer renderer;
    engine::graphics::Camera camera;
    camera.setViewport(Vec2{1280.0F, 720.0F});
    camera.setPosition(Vec2{1000.0F, 500.0F});
    FakeAssetManager assets;
    RenderSystem renderSystem{renderer, camera, assets};

    Entity& entity = manager.addEntity("entity");
    entity.addComponent<Transform>(Transform{Vec2{1100.0F, 500.0F}, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
    entity.addComponent<Rectangle>();

    renderSystem.update(manager, input, 0.016F);

    CHECK(renderer.draws().size() == 1);
    if (renderer.draws().size() == 1)
    {
        // 100 to the right of the camera becomes 100 right of the screen centre.
        CHECK_NEAR_VEC(renderer.draws().front().placement.position, Vec2(740.0F, 360.0F));
    }
}

void testRenderSystemAppliesCameraZoomToDrawCalls()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::Input input;
    RecordingRenderer renderer;
    engine::graphics::Camera camera;
    camera.setViewport(Vec2{1280.0F, 720.0F});
    camera.setZoom(2.0F);
    FakeAssetManager assets;
    RenderSystem renderSystem{renderer, camera, assets};

    Entity& entity = manager.addEntity("entity");
    // A non-unit Transform::scale, so composing zoom with scale can be told
    // apart from either one replacing the other.
    entity.addComponent<Transform>(Transform{Vec2{100.0F, 100.0F}, Vec2{0.0F, 0.0F}, Vec2{3.0F, 3.0F}, 0.0F});
    entity.addComponent<Rectangle>();

    renderSystem.update(manager, input, 0.016F);

    CHECK(renderer.draws().size() == 1);
    if (renderer.draws().size() == 1)
    {
        // 3 * 2, not 2 and not 3.
        CHECK_NEAR_VEC(renderer.draws().front().placement.scale, Vec2(6.0F, 6.0F));
    }
}

void testRenderSystemDoesNotModifyTheTransform()
{
    // A camera changes the picture, never the data. Whatever the camera is doing,
    // the entity's world position and scale come out the other side untouched.
    EntityManager manager;
    [[maybe_unused]] engine::input::Input input;
    RecordingRenderer renderer;
    engine::graphics::Camera camera;
    camera.setViewport(Vec2{1280.0F, 720.0F});
    camera.setPosition(Vec2{-5000.0F, 2500.0F});
    camera.setZoom(3.5F);
    FakeAssetManager assets;
    RenderSystem renderSystem{renderer, camera, assets};

    Entity& entity = manager.addEntity("entity");
    entity.addComponent<Transform>(Transform{Vec2{640.0F, 360.0F}, Vec2{9.0F, 9.0F}, Vec2{1.5F, 2.5F}, 0.75F});
    entity.addComponent<Rectangle>();
    const Transform before = entity.getComponent<Transform>();

    renderSystem.update(manager, input, 0.016F);

    const Transform& after = entity.getComponent<Transform>();
    CHECK(after.position == before.position);
    CHECK(after.velocity == before.velocity);
    CHECK(after.scale == before.scale);
    CHECK_NEAR(after.angle, before.angle);

    // And the draw call really did carry the camera, so the test is not passing
    // because the system did nothing. All three parts of the mapping at once:
    // the offset is (640, 360) - (-5000, 2500) = (5640, -2140), scaled by the
    // zoom of 3.5 to (19740, -7490), then offset by the screen centre.
    CHECK(renderer.draws().size() == 1);
    if (renderer.draws().size() == 1)
    {
        CHECK_NEAR_VEC(renderer.draws().front().placement.position, Vec2(20380.0F, -7130.0F));
        CHECK_NEAR_VEC(renderer.draws().front().placement.scale, Vec2(5.25F, 8.75F));
    }
}

void testRenderSystemRunsAfterSimulationSystems()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::Input input;
    RecordingRenderer renderer;

    engine::ecs::SystemManager systems;
    systems.add<DriftSystem>();
    engine::graphics::Camera camera;
    FakeAssetManager assets;
    RenderSystem renderSystem{renderer, camera, assets};

    Entity& entity = manager.addEntity("entity");
    entity.addComponent<Transform>(Transform{Vec2{0.0F, 0.0F}, Vec2{100.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
    entity.addComponent<Rectangle>();

    // One second of drift, then draw: the draw must see the moved position.
    systems.update(manager, input, 1.0F);
    renderSystem.update(manager, input, 0.0F);

    CHECK(renderer.draws().size() == 1);
    if (renderer.draws().size() == 1)
    {
        CHECK(renderer.draws().front().placement.position == Vec2(100.0F, 0.0F));
    }
}

void testRenderSystemDoesNotOwnEntities()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::Input input;
    RecordingRenderer renderer;
    engine::graphics::Camera camera;
    FakeAssetManager assets;
    RenderSystem renderSystem{renderer, camera, assets};

    {
        engine::ecs::SystemManager systems;
        Entity& entity = manager.addEntity("entity");
        entity.addComponent<Transform>();
        entity.addComponent<Rectangle>();
        renderSystem.update(manager, input, 0.016F);
        CHECK(renderer.draws().size() == 1);
    }

    // Systems are gone; the world is untouched and still drawable.
    CHECK(manager.aliveEntityCount() == 1);
    CHECK(manager.query<Transform, Rectangle>().size() == 1);
    renderSystem.update(manager, input, 0.016F);
    CHECK(renderer.draws().size() == 2);
}

void testRendererFrameProtocol()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::Input input;
    RecordingRenderer renderer;
    engine::graphics::Camera camera;
    FakeAssetManager assets;
    RenderSystem renderSystem{renderer, camera, assets};

    // A full frame, in the order the Renderer contract documents.
    renderer.beginFrame();
    renderer.clear(engine::kBlack);

    Entity& entity = manager.addEntity("entity");
    entity.addComponent<Transform>();
    entity.addComponent<Rectangle>();

    renderSystem.update(manager, input, 0.0F);
    renderer.endFrame();

    CHECK(renderer.beginFrameCount() == 1);
    CHECK(renderer.endFrameCount() == 1);
    CHECK(renderer.frameCount() == 1);
    CHECK(renderer.cleared());
    CHECK(renderer.clearColor() == engine::kBlack);
    CHECK(renderer.draws().size() == 1);
}

// ---------------------------------------------------------------------------
// Graphics integration: a real window, real SFML, real pixels
// ---------------------------------------------------------------------------

/// Reads a pixel out of the window's current draw buffer.
///
/// This must happen before endFrame(): on this platform the front buffer after
/// display() reads back black, whereas the buffer that was just drawn reads back
/// correctly. So the pixel check runs inside the frame, not after it.
[[nodiscard]] sf::Color readPixel(sf::RenderWindow& window, const std::size_t x, const std::size_t y)
{
    sf::Texture texture;
    if (!texture.create(window.getSize().x, window.getSize().y))
    {
        return sf::Color::Black;
    }

    texture.update(window);
    return texture.copyToImage().getPixel(static_cast<unsigned>(x), static_cast<unsigned>(y));
}

void testSfmlRendererDrawsPixels()
{
    // A real window, a real renderer, and a pixel assertion on the result.
    sf::RenderWindow window(sf::VideoMode{200, 100}, "render pixels");
    engine::graphics::SfmlRenderer renderer{window};

    constexpr Color background{0.0F, 0.0F, 1.0F, 1.0F};  // blue
    constexpr Color fill{1.0F, 0.0F, 0.0F, 1.0F};        // red

    renderer.beginFrame();
    renderer.clear(background);

    // 40x20 red rectangle centred at (100, 50), unscaled and unrotated.
    const RenderTransform placement{Vec2{100.0F, 50.0F}, Vec2{1.0F, 1.0F}, 0.0F};
    renderer.drawRectangle(Vec2{40.0F, 20.0F}, fill, placement);

    // Read before presenting.
    const sf::Color centre = readPixel(window, 100, 50);
    const sf::Color corner = readPixel(window, 4, 4);
    const sf::Color outside = readPixel(window, 190, 95);

    renderer.endFrame();

    // Inside the rectangle: the fill colour.
    CHECK(centre.r == 255);
    CHECK(centre.g == 0);
    CHECK(centre.b == 0);

    // Outside the rectangle: the configured background.
    CHECK(corner.b == 255);
    CHECK(corner.r == 0);
    CHECK(outside.b == 255);

    CHECK(renderer.frameCount() == 1);
}

void testSfmlRendererHonoursPosition()
{
    sf::RenderWindow window(sf::VideoMode{200, 100}, "render position");
    engine::graphics::SfmlRenderer renderer{window};

    renderer.beginFrame();
    renderer.clear(engine::kBlack);

    // Position is the centre, so a 20x20 box at (50, 50) spans 40..60.
    renderer.drawRectangle(Vec2{20.0F, 20.0F}, engine::kWhite, RenderTransform{Vec2{50.0F, 50.0F}, Vec2{1.0F, 1.0F}, 0.0F});

    const sf::Color centre = readPixel(window, 50, 50);
    const sf::Color atOrigin = readPixel(window, 5, 5);

    renderer.endFrame();

    // Drawn centred on the position, not hanging off the top-left.
    CHECK(centre.r == 255);
    CHECK(atOrigin.r == 0);
}

void testSfmlRendererHonoursScale()
{
    sf::RenderWindow window(sf::VideoMode{200, 100}, "render scale");
    engine::graphics::SfmlRenderer renderer{window};

    renderer.beginFrame();
    renderer.clear(engine::kBlack);

    // A 20x20 box scaled 3x is 60x60, so it spans 70..130 horizontally. Sample
    // well inside and well outside, clear of the rasterised edge.
    renderer.drawRectangle(Vec2{20.0F, 20.0F}, engine::kWhite, RenderTransform{Vec2{100.0F, 50.0F}, Vec2{3.0F, 3.0F}, 0.0F});

    const sf::Color inside = readPixel(window, 120, 50);
    const sf::Color outside = readPixel(window, 150, 50);

    renderer.endFrame();

    CHECK(inside.r == 255);
    CHECK(outside.r == 0);
}

void testSfmlRendererHonoursRotation()
{
    sf::RenderWindow window(sf::VideoMode{200, 100}, "render rotation");
    engine::graphics::SfmlRenderer renderer{window};

    renderer.beginFrame();
    renderer.clear(engine::kBlack);

    // A wide, short bar rotated a quarter turn about its centre becomes tall and
    // narrow. Sampling above and below the centre proves it turned.
    renderer.drawRectangle(Vec2{60.0F, 10.0F}, engine::kWhite,
                           RenderTransform{Vec2{100.0F, 50.0F}, Vec2{1.0F, 1.0F}, 90.0F});

    const sf::Color aboveCentre = readPixel(window, 100, 35);
    const sf::Color besideCentre = readPixel(window, 130, 50);

    renderer.endFrame();

    // Rotated 90 degrees, the bar now reaches vertically.
    CHECK(aboveCentre.r == 255);
    CHECK(besideCentre.r == 0);
}

void testSfmlRendererClampsColourChannels()
{
    sf::RenderWindow window(sf::VideoMode{100, 100}, "render clamp");
    engine::graphics::SfmlRenderer renderer{window};

    renderer.beginFrame();
    renderer.clear(engine::kBlack);

    // Channels outside [0, 1] must clamp rather than wrap, so 2.0 becomes 255
    // and -1.0 becomes 0. Unclamped these would wrap to 254 and 255.
    const Color wild{2.0F, -1.0F, 0.5F, 1.0F};
    renderer.drawRectangle(Vec2{80.0F, 80.0F}, wild, RenderTransform{Vec2{50.0F, 50.0F}, Vec2{1.0F, 1.0F}, 0.0F});

    const sf::Color centre = readPixel(window, 50, 50);

    renderer.endFrame();

    CHECK(centre.r == 255);
    CHECK(centre.g == 0);
    CHECK(centre.b == 127);
}

// ---------------------------------------------------------------------------
// Camera through the real renderer, verified with real pixels
// ---------------------------------------------------------------------------

/// A camera looking at a 200x100 window, the same size the pixel tests use.
Camera pixelCamera(const Vec2& position, const float zoom = 1.0F)
{
    Camera camera;
    camera.setViewport(Vec2{200.0F, 100.0F});
    camera.setPosition(position);
    camera.setZoom(zoom);
    return camera;
}

void testCameraDrawsAtViewportCentre()
{
    sf::RenderWindow window(sf::VideoMode{200, 100}, "camera centre");
    engine::graphics::SfmlRenderer renderer{window};

    renderer.beginFrame();
    renderer.clear(engine::kBlack);

    // A box in the world, with the camera sitting exactly on it, must land in
    // the middle of the 200x100 window.
    Transform transform{Vec2{1000.0F, 500.0F}, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F};
    const Camera camera = pixelCamera(Vec2{1000.0F, 500.0F});
    renderer.drawRectangle(Vec2{40.0F, 40.0F}, engine::kWhite, engine::graphics::toRenderTransform(transform, camera));

    const sf::Color atCentre = readPixel(window, 100, 50);
    const sf::Color topLeft = readPixel(window, 5, 5);

    renderer.endFrame();

    CHECK(atCentre.r == 255);
    CHECK(topLeft.r == 0);
}

void testCameraMovesObjectsOnScreenOnly()
{
    // The point of the phase: moving the camera slides the object across the
    // screen without changing anything about where the object is in the world.
    sf::RenderWindow window(sf::VideoMode{200, 100}, "camera slide");
    engine::graphics::SfmlRenderer renderer{window};

    Transform transform{Vec2{1000.0F, 500.0F}, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F};
    const Vec2 before = transform.position;

    // Camera left of the object: the object appears to the right of centre.
    renderer.beginFrame();
    renderer.clear(engine::kBlack);
    renderer.drawRectangle(Vec2{20.0F, 20.0F}, engine::kWhite,
                           engine::graphics::toRenderTransform(transform, pixelCamera(Vec2{950.0F, 500.0F})));
    const sf::Color rightOfCentre = readPixel(window, 150, 50);
    const sf::Color leftOfCentre = readPixel(window, 50, 50);
    renderer.endFrame();

    // Camera right of the object: it appears to the left of centre instead.
    renderer.beginFrame();
    renderer.clear(engine::kBlack);
    renderer.drawRectangle(Vec2{20.0F, 20.0F}, engine::kWhite,
                           engine::graphics::toRenderTransform(transform, pixelCamera(Vec2{1050.0F, 500.0F})));
    const sf::Color nowLeft = readPixel(window, 50, 50);
    const sf::Color nowRight = readPixel(window, 150, 50);
    renderer.endFrame();

    // The world position never moved, and never would.
    CHECK(transform.position == before);

    CHECK(rightOfCentre.r == 255);
    CHECK(leftOfCentre.r == 0);
    CHECK(nowLeft.r == 255);
    CHECK(nowRight.r == 0);
}

void testZoomDoublesApparentSize()
{
    // A 20x20 box at zoom 1 is 20x20; at zoom 2 it must be 40x40. Sampling
    // inside the doubled extent and just outside the original one proves the
    // size really changed and not just the position.
    sf::RenderWindow window(sf::VideoMode{200, 100}, "zoom in");
    engine::graphics::SfmlRenderer renderer{window};

    Transform transform{Vec2{500.0F, 500.0F}, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F};

    renderer.beginFrame();
    renderer.clear(engine::kBlack);
    renderer.drawRectangle(Vec2{20.0F, 20.0F}, engine::kWhite,
                           engine::graphics::toRenderTransform(transform, pixelCamera(Vec2{500.0F, 500.0F}, 2.0F)));

    // At zoom 2 the box spans 80..120 horizontally, so 110 is inside and 130 is
    // outside, where at zoom 1 both would have been outside.
    const sf::Color insideDoubled = readPixel(window, 110, 50);
    const sf::Color outsideDoubled = readPixel(window, 130, 50);
    renderer.endFrame();

    // The world position and size are untouched by the zoom.
    CHECK(transform.position == Vec2(500.0F, 500.0F));
    CHECK(transform.scale == Vec2(1.0F, 1.0F));

    CHECK(insideDoubled.r == 255);
    CHECK(outsideDoubled.r == 0);
}

void testZoomHalvesApparentSize()
{
    sf::RenderWindow window(sf::VideoMode{200, 100}, "zoom out");
    engine::graphics::SfmlRenderer renderer{window};

    Transform transform{Vec2{500.0F, 500.0F}, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F};

    // A 40x40 box at zoom 0.5 is 20x20, so it must fit inside the 20x20 region a
    // 40x40 box would have filled at zoom 1.
    renderer.beginFrame();
    renderer.clear(engine::kBlack);
    renderer.drawRectangle(Vec2{40.0F, 40.0F}, engine::kWhite,
                           engine::graphics::toRenderTransform(transform, pixelCamera(Vec2{500.0F, 500.0F}, 0.5F)));

    const sf::Color insideHalved = readPixel(window, 105, 50);
    const sf::Color outsideHalved = readPixel(window, 120, 50);
    renderer.endFrame();

    CHECK(insideHalved.r == 255);
    CHECK(outsideHalved.r == 0);
}

void testZoomIsAboutTheViewportCentre()
{
    // Zoom must magnify about the middle of the screen, not about the top-left.
    // A box on the camera stays dead centre however far it is zoomed.
    sf::RenderWindow window(sf::VideoMode{200, 100}, "zoom centre");
    engine::graphics::SfmlRenderer renderer{window};

    Transform transform{Vec2{500.0F, 500.0F}, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F};

    for (const float zoom : {0.5F, 1.0F, 2.0F, 4.0F})
    {
        renderer.beginFrame();
        renderer.clear(engine::kBlack);
        renderer.drawRectangle(Vec2{10.0F, 10.0F}, engine::kWhite,
                               engine::graphics::toRenderTransform(transform, pixelCamera(Vec2{500.0F, 500.0F}, zoom)));
        const sf::Color atCentre = readPixel(window, 100, 50);
        renderer.endFrame();

        CHECK(atCentre.r == 255);
    }
}

void testCameraPreservesRotationAndScale()
{
    // The camera changes where and how big an object is drawn. It must not
    // change the rotation convention, and it must compose with Transform::scale
    // rather than replacing it.
    sf::RenderWindow window(sf::VideoMode{200, 100}, "camera rotation");
    engine::graphics::SfmlRenderer renderer{window};

    // A wide short bar, turned a quarter turn (pi/2 radians), at zoom 2.
    Transform transform{Vec2{500.0F, 500.0F}, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, kPi * 0.5F};
    const Camera camera = pixelCamera(Vec2{500.0F, 500.0F}, 2.0F);

    const RenderTransform placement = engine::graphics::toRenderTransform(transform, camera);

    // Rotation survives the camera, and zoom is composed with scale, not swapped
    // for it: Transform::scale of 1 times zoom 2 is 2.
    CHECK_NEAR(placement.rotationDegrees, 90.0F);
    CHECK_NEAR_VEC(placement.scale, Vec2(2.0F, 2.0F));

    renderer.beginFrame();
    renderer.clear(engine::kBlack);
    renderer.drawRectangle(Vec2{40.0F, 10.0F}, engine::kWhite, placement);

    // Rotated 90 degrees and doubled, the bar reaches well above and below the
    // centre and no longer reaches to the right.
    const sf::Color above = readPixel(window, 100, 30);
    const sf::Color beside = readPixel(window, 140, 50);
    renderer.endFrame();

    CHECK(above.r == 255);
    CHECK(beside.r == 0);
}

void testApplicationRendersRenderableEntities()
{
    // Full end-to-end: a real Application, a real window, a real render pass.
    engine::Application application;

    Entity& rectangle = application.entityManager().addEntity("rectangle");
    rectangle.addComponent<Transform>(Transform{Vec2{100.0F, 100.0F}, Vec2{50.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
    rectangle.addComponent<Rectangle>(Rectangle{Vec2{40.0F, 40.0F}, Color{1.0F, 0.0F, 0.0F, 1.0F}});

    application.systemManager().add<DriftSystem>();

    // Bounded execution: the loop stops on its own.
    CHECK(application.run(5) == EXIT_SUCCESS);
    CHECK(application.time().frameCount() >= 5);
    CHECK(application.renderer().frameCount() == 5);

    // The simulation system ran, so the rectangle drifted 50 units right.
    CHECK(rectangle.getComponent<Transform>().position.x > 100.0F);
}

void testApplicationSurvivesNoRenderableEntities()
{
    engine::Application application;
    application.entityManager().addEntity("nothing to draw");

    CHECK(application.run(3) == EXIT_SUCCESS);
    CHECK(application.renderer().frameCount() == 3);
}

// ---------------------------------------------------------------------------
// Step 7: textures.
//
// Everything above tested rectangles. These groups add a second draw and a second
// query, under one standing rule: nothing that already worked may move. The
// rectangle query is unchanged, the world-to-screen conversion is shared, and the
// 33 groups above were not edited to accommodate any of this.
// ---------------------------------------------------------------------------

/// A textured entity, wired the way a game would wire one: a name in the
/// component, everything else in the transform.
Entity& addTexturedEntity(EntityManager& manager, const std::string& name, const Vec2& position,
                          const Vec2& scale = Vec2{1.0F, 1.0F})
{
    Entity& entity = manager.addEntity(name);
    entity.addComponent<Transform>(Transform{position, Vec2{0.0F, 0.0F}, scale, 0.0F});
    entity.addComponent<components::Texture>(components::Texture{name});
    return entity;
}

/// The real, shipped configuration, for the groups that need real pixels.
[[nodiscard]] assets::SfmlAssetManager shippedAssets()
{
    return assets::SfmlAssetManager{std::filesystem::path{ENGINE_ASSET_CONFIG}};
}

void testTextureComponentStoresAnAssetName()
{
    components::Texture texture;

    // The one semantic field, and it is a name, so it is readable data.
    CHECK(texture.assetName.empty());

    texture.assetName = "mario_ground";
    CHECK(texture.assetName == "mario_ground");

    // A name is plain data, so it copies and compares like every other component.
    const components::Texture copy = texture;
    CHECK(copy.assetName == texture.assetName);
}

void testTextureComponentIsPlainDataWithNoNativeState()
{
    // The point of storing a name: the component is ordinary data, so the ECS
    // never touches a graphics type and an entity can be described in a file.
    static_assert(std::is_aggregate_v<components::Texture>, "Texture must remain an aggregate");
    static_assert(std::is_default_constructible_v<components::Texture>, "Texture must be default constructible");
    static_assert(std::is_copy_constructible_v<components::Texture>, "a name is copyable, unlike a handle");
    static_assert(std::is_copy_assignable_v<components::Texture>, "a name is copy assignable, unlike a handle");

    // One field, and it is a string: not a handle, not an index, not a pointer.
    static_assert(std::is_same_v<decltype(components::Texture{}.assetName), std::string>,
                  "the component must hold a name and nothing else");

    // A std::string member is deliberately not trivially copyable. That is the
    // price of being describable in a level file, and it is the only component
    // that pays it, because it is the only one holding an owned value.
    static_assert(!std::is_trivially_copyable_v<components::Texture>,
                  "a std::string member is not trivially copyable, by design");

    // It is emphatically not a handle: a handle is neither copyable nor
    // assignable, and a component that was one would stop being data.
    static_assert(!std::is_same_v<components::Texture, assets::Texture>, "the component must not be a handle");

    CHECK(true);
}

void testRendererInterfaceExposesDrawTexture()
{
    // The exact signature the rest of the engine is written against: a const
    // handle, a placement and an optional source region, all const, and no size
    // parameter, because the size belongs to the image or to the region.
    //
    // The region is `std::optional<IntRect>` and not a bare rect, so that "the
    // whole image" is expressible without a sentinel value that would have to be
    // distinguished from a legitimately empty region. `std::nullopt` is the whole
    // image; a present value is the region.
    using DrawTexture = void (graphics::Renderer::*)(const assets::Texture&, const graphics::RenderTransform&,
                                                     const std::optional<engine::IntRect>&);
    static_assert(std::is_same_v<decltype(&graphics::Renderer::drawTexture), DrawTexture>,
                  "drawTexture must take a const handle, a placement and an optional source region");

    // The region type is the engine's own, never a graphics one. If this ever
    // became `sf::IntRect` the interface would drag SFML into every system.
    static_assert(std::is_same_v<engine::IntRect, engine::IntRect>, "IntRect must be a complete engine type");
    static_assert(!std::is_pointer_v<engine::IntRect>, "IntRect must be a value, not a handle");
    static_assert(std::is_trivially_copyable_v<engine::IntRect>, "IntRect must be cheaply copyable");
    static_assert(std::is_default_constructible_v<engine::IntRect>, "IntRect must have a default state");
    static_assert(sizeof(engine::IntRect) == 4U * sizeof(int), "IntRect must be exactly four ints");

    static_assert(std::is_abstract_v<graphics::Renderer>, "Renderer must stay abstract");

    // Additive: every pre-existing member is byte-for-byte what it was.
    using DrawRectangle = void (graphics::Renderer::*)(const Vec2&, const Color&, const graphics::RenderTransform&);
    static_assert(std::is_same_v<decltype(&graphics::Renderer::drawRectangle), DrawRectangle>,
                  "drawRectangle must be unchanged");
    static_assert(std::is_same_v<decltype(&graphics::Renderer::beginFrame), void (graphics::Renderer::*)()>,
                  "beginFrame must be unchanged");
    static_assert(std::is_same_v<decltype(&graphics::Renderer::endFrame), void (graphics::Renderer::*)()>,
                  "endFrame must be unchanged");
    static_assert(std::is_same_v<decltype(&graphics::Renderer::clear), void (graphics::Renderer::*)(const Color&)>,
                  "clear must be unchanged");
    static_assert(std::is_same_v<decltype(&graphics::Renderer::frameCount),
                                 std::uint64_t (graphics::Renderer::*)() const noexcept>,
                  "frameCount must be unchanged");
    static_assert(std::is_destructible_v<graphics::Renderer>, "Renderer must stay destructible");

    CHECK(true);
}

void testARendererWithoutSfmlStillSatisfiesTheInterface()
{
    // Nothing in the interface needs a graphics library, so a test double can
    // implement the whole thing. If that ever stops being true, the fake used
    // throughout this file stops compiling, which is the point.
    struct PlainRenderer final : graphics::Renderer
    {
        void beginFrame() override {}
        void clear(const Color&) override {}
        void drawRectangle(const Vec2&, const Color&, const graphics::RenderTransform&) override {}
        void drawTexture(const assets::Texture&, const graphics::RenderTransform&,
                         const std::optional<engine::IntRect>&) override
        {
        }
        void endFrame() override {}
        [[nodiscard]] std::uint64_t frameCount() const noexcept override { return 0; }
    };

    static_assert(std::is_final_v<graphics::SfmlRenderer>, "SfmlRenderer must stay final");
    static_assert(std::is_base_of_v<graphics::Renderer, PlainRenderer>, "a non-SFML renderer must satisfy Renderer");
    static_assert(!std::is_copy_constructible_v<graphics::Renderer>, "a renderer must not be copyable");

    CHECK(true);
}

// --- RenderSystem: the second query -----------------------------------------

void testRenderSystemTextureQueryDrawsTextures()
{
    EntityManager manager;
    RecordingRenderer renderer;
    FakeAssetManager assets;
    assets.declare("mario_ground");
    [[maybe_unused]] Input input;

    const Camera camera;
    RenderSystem renderSystem{renderer, camera, assets};
    renderer.beginFrame();

    addTexturedEntity(manager, "mario_ground", Vec2{100.0F, 50.0F});
    renderSystem.update(manager, input, 0.0F);

    // The new query drew, and the rectangle query did not.
    CHECK(renderer.textureDraws().size() == 1U);
    CHECK(renderer.draws().empty());
}

void testRenderSystemRequestsTheNameTheComponentCarries()
{
    EntityManager manager;
    RecordingRenderer renderer;
    FakeAssetManager assets;
    assets.declare("mario_ground");
    [[maybe_unused]] Input input;

    const Camera camera;
    RenderSystem renderSystem{renderer, camera, assets};
    renderer.beginFrame();

    addTexturedEntity(manager, "mario_ground", Vec2{10.0F, 10.0F});
    renderSystem.update(manager, input, 0.0F);

    // The name the component holds, not one the system invented.
    CHECK(assets.requested().size() == 1U);
    if (!assets.requested().empty())
    {
        CHECK(assets.requested().front() == "mario_ground");
    }
}

void testRenderSystemSubmitsTheManagersOwnHandle()
{
    EntityManager manager;
    RecordingRenderer renderer;
    FakeAssetManager assets;
    assets.declare("mario_ground");
    [[maybe_unused]] Input input;

    const Camera camera;
    RenderSystem renderSystem{renderer, camera, assets};
    renderer.beginFrame();

    addTexturedEntity(manager, "mario_ground", Vec2{10.0F, 10.0F});
    renderSystem.update(manager, input, 0.0F);

    // Identity, not a copy: the draw references the very handle the manager owns,
    // which is what makes loading once worth anything.
    CHECK(renderer.textureDraws().size() == 1U);
    if (!renderer.textureDraws().empty())
    {
        CHECK(renderer.textureDraws().front().texture == &assets.texture("mario_ground"));
    }
}

void testTransformPositionMovesTheTexture()
{
    EntityManager manager;
    RecordingRenderer renderer;
    FakeAssetManager assets;
    assets.declare("mario_ground");
    [[maybe_unused]] Input input;

    const Camera camera;
    RenderSystem renderSystem{renderer, camera, assets};
    renderer.beginFrame();

    addTexturedEntity(manager, "mario_ground", Vec2{300.0F, 200.0F});
    renderSystem.update(manager, input, 0.0F);

    // The same conversion the rectangle query uses.
    CHECK(renderer.textureDraws().size() == 1U);
    if (!renderer.textureDraws().empty())
    {
        const RenderTransform placement = renderer.textureDraws().front().placement;
        CHECK_NEAR_VEC(placement.position, Vec2(300.0F, 200.0F));
    }
}

void testTransformScaleScalesTheTexture()
{
    EntityManager manager;
    RecordingRenderer renderer;
    FakeAssetManager assets;
    assets.declare("mario_ground");
    [[maybe_unused]] Input input;

    const Camera camera;
    RenderSystem renderSystem{renderer, camera, assets};
    renderer.beginFrame();

    // The same shape of change a rectangle sees.
    addTexturedEntity(manager, "mario_ground", Vec2{0.0F, 0.0F}, Vec2{3.0F, 0.5F});
    renderSystem.update(manager, input, 0.0F);

    CHECK(renderer.textureDraws().size() == 1U);
    if (!renderer.textureDraws().empty())
    {
        CHECK_NEAR_VEC(renderer.textureDraws().front().placement.scale, Vec2(3.0F, 0.5F));
    }
}

void testCameraZoomScalesTheTextureWithoutMovingIt()
{
    EntityManager manager;
    RecordingRenderer renderer;
    FakeAssetManager assets;
    assets.declare("mario_ground");
    [[maybe_unused]] Input input;

    Camera camera;
    camera.setZoom(2.0F);

    RenderSystem renderSystem{renderer, camera, assets};
    renderer.beginFrame();

    // The SAME Transform on a rectangle and on a texture. The camera has to reach
    // both identically, and comparing the two placements directly is a stronger
    // statement than re-deriving the expected numbers here.
    //
    // Worth being precise about what zoom does. It multiplies the object's own
    // scale, and it also scales a world position's offset from the camera's
    // centre, because that is what looking more closely means: a point twice as
    // far from the centre appears twice as far out. What it must NOT do is scale
    // the object's own size independently of the camera, which is why the
    // rectangle comparison matters.
    Entity& rectangle = manager.addEntity("rectangle");
    rectangle.addComponent<Transform>(Transform{Vec2{150.0F, 80.0F}, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
    rectangle.addComponent<Rectangle>(Rectangle{Vec2{10.0F, 10.0F}, Color{0.0F, 1.0F, 0.0F, 1.0F}});
    addTexturedEntity(manager, "mario_ground", Vec2{150.0F, 80.0F});

    renderSystem.update(manager, input, 0.0F);

    CHECK(renderer.textureDraws().size() == 1U);
    CHECK(renderer.draws().size() == 1U);
    if (!renderer.textureDraws().empty() && !renderer.draws().empty())
    {
        const RenderTransform texturePlacement = renderer.textureDraws().front().placement;
        const RenderTransform rectanglePlacement = renderer.draws().front().placement;

        // Zoom multiplies the object's scale.
        CHECK_NEAR_VEC(texturePlacement.scale, Vec2(2.0F, 2.0F));

        // And the camera places the two identically, in every field.
        CHECK_NEAR_VEC(texturePlacement.position, rectanglePlacement.position);
        CHECK_NEAR_VEC(texturePlacement.scale, rectanglePlacement.scale);
        CHECK_NEAR(texturePlacement.rotationDegrees, rectanglePlacement.rotationDegrees);
    }
}

void testMissingAssetNamePropagates()
{
    EntityManager manager;
    RecordingRenderer renderer;
    FakeAssetManager assets;
    // Nothing declared, so the lookup fails.
    [[maybe_unused]] Input input;

    const Camera camera;
    RenderSystem renderSystem{renderer, camera, assets};
    renderer.beginFrame();

    addTexturedEntity(manager, "not_declared", Vec2{0.0F, 0.0F});

    bool threw = false;
    try
    {
        renderSystem.update(manager, input, 0.0F);
    }
    catch (const assets::AssetNotFoundError&)
    {
        threw = true;
    }

    // Loudly. An entity that quietly draws nothing is a bug that surfaces much
    // later as a missing sprite with nothing pointing at the cause.
    CHECK(threw);
    CHECK(renderer.textureDraws().empty());
}

void testRenderSystemHasNoAssetsUnavailableState()
{
    // The Step 7 bridge is gone. A RenderSystem cannot be built without an asset
    // manager, so there is no "assets unavailable" state for a textured entity to
    // stumble into, and no null check in the draw path that could be wrong.
    //
    // The absence of the two-argument constructor is a compile-time property, so
    // it is asserted in the type system: a call with two arguments must not
    // compile, and Probe's initialiser is the two-argument form.
    using TwoArgument = RenderSystem (*)(graphics::Renderer&, const Camera&);
    static_assert(!std::is_constructible_v<RenderSystem, graphics::Renderer&, const Camera&>,
                  "RenderSystem must not be constructible without an AssetManager");
    static_assert(std::is_constructible_v<RenderSystem, graphics::Renderer&, const Camera&,
                                          const assets::AssetManager&>,
                  "RenderSystem must be constructible with an AssetManager");
    static_assert(!std::is_default_constructible_v<RenderSystem>, "RenderSystem must not be default constructible");

    // And every constructor takes the manager, so there is no way to reach one
    // without it. Exactly one is constructible, which is the whole point.
    static_assert(!std::is_constructible_v<RenderSystem, graphics::Renderer&>,
                  "the renderer alone must not be enough");
    static_assert(!std::is_constructible_v<RenderSystem, const assets::AssetManager&>,
                  "the asset manager alone must not be enough");

    static_assert(sizeof(TwoArgument) == sizeof(void*), "unused, keeps the alias honest");

    // A textured entity on a properly built system simply draws.
    EntityManager manager;
    RecordingRenderer renderer;
    FakeAssetManager assets;
    assets.declare("mario_ground");
    [[maybe_unused]] Input input;

    const Camera camera;
    RenderSystem renderSystem{renderer, camera, assets};
    renderer.beginFrame();

    addTexturedEntity(manager, "mario_ground", Vec2{0.0F, 0.0F});
    renderSystem.update(manager, input, 0.0F);

    CHECK(renderer.textureDraws().size() == 1U);
}

void testRectangleQueryIsUnaffectedByTextures()
{
    EntityManager manager;
    RecordingRenderer renderer;
    FakeAssetManager assets;
    [[maybe_unused]] Input input;

    const Camera camera;
    RenderSystem renderSystem{renderer, camera, assets};
    renderer.beginFrame();

    // Only a rectangle, exactly as before this step existed.
    Entity& rectangle = manager.addEntity("rectangle");
    rectangle.addComponent<Transform>(Transform{Vec2{100.0F, 50.0F}, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
    rectangle.addComponent<Rectangle>(Rectangle{Vec2{40.0F, 20.0F}, Color{1.0F, 0.0F, 0.0F, 1.0F}});
    renderSystem.update(manager, input, 0.0F);

    CHECK(renderer.draws().size() == 1U);
    CHECK(renderer.textureDraws().empty());
    if (!renderer.draws().empty())
    {
        CHECK_NEAR_VEC(renderer.draws().front().size, Vec2(40.0F, 20.0F));
        CHECK_NEAR_VEC(renderer.draws().front().placement.position, Vec2(100.0F, 50.0F));
    }
}

void testTexturesAreDrawnAfterRectangles()
{
    EntityManager manager;
    RecordingRenderer renderer;
    FakeAssetManager assets;
    assets.declare("mario_ground");
    [[maybe_unused]] Input input;

    const Camera camera;
    RenderSystem renderSystem{renderer, camera, assets};
    renderer.beginFrame();

    // The texture entity is created FIRST, so plain iteration order would draw it
    // first. The documented rule is that ordering follows the query, not the
    // entity, and this is the group that says so.
    addTexturedEntity(manager, "mario_ground", Vec2{10.0F, 10.0F});

    Entity& rectangle = manager.addEntity("rectangle");
    rectangle.addComponent<Transform>(Transform{Vec2{100.0F, 50.0F}, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
    rectangle.addComponent<Rectangle>(Rectangle{Vec2{10.0F, 10.0F}, Color{0.0F, 1.0F, 0.0F, 1.0F}});

    renderSystem.update(manager, input, 0.0F);

    // Rectangle first, then texture, whatever the entity order was.
    CHECK(renderer.order().size() == 2U);
    if (renderer.order().size() == 2U)
    {
        CHECK(renderer.order()[0] == "rectangle");
        CHECK(renderer.order()[1] == "texture");
    }
}

void testAnEntityWithBothComponentsDrawsBothRectangleFirst()
{
    EntityManager manager;
    RecordingRenderer renderer;
    FakeAssetManager assets;
    assets.declare("mario_ground");
    [[maybe_unused]] Input input;

    const Camera camera;
    RenderSystem renderSystem{renderer, camera, assets};
    renderer.beginFrame();

    // Carrying both is not a special case: the entity qualifies for both queries,
    // and the ordering rule is what decides it.
    Entity& both = manager.addEntity("mario_ground");
    both.addComponent<Transform>(Transform{Vec2{50.0F, 50.0F}, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
    both.addComponent<Rectangle>(Rectangle{Vec2{10.0F, 10.0F}, Color{0.0F, 1.0F, 0.0F, 1.0F}});
    both.addComponent<components::Texture>(components::Texture{"mario_ground"});

    renderSystem.update(manager, input, 0.0F);

    CHECK(renderer.order().size() == 2U);
    CHECK(renderer.draws().size() == 1U);
    CHECK(renderer.textureDraws().size() == 1U);
    if (renderer.order().size() == 2U)
    {
        CHECK(renderer.order()[0] == "rectangle");
        CHECK(renderer.order()[1] == "texture");
    }
}

void testRenderSystemDoesNotWriteToTheTextureTransform()
{
    EntityManager manager;
    RecordingRenderer renderer;
    FakeAssetManager assets;
    assets.declare("mario_ground");
    [[maybe_unused]] Input input;

    const Camera camera;
    RenderSystem renderSystem{renderer, camera, assets};
    renderer.beginFrame();

    const Entity& entity = addTexturedEntity(manager, "mario_ground", Vec2{100.0F, 50.0F});
    const Transform before = entity.getComponent<Transform>();

    renderSystem.update(manager, input, 0.0F);

    // Drawing reads the world and never writes it, so a render pass cannot move
    // anything in the simulation.
    const Transform after = entity.getComponent<Transform>();
    CHECK_NEAR_VEC(after.position, before.position);
    CHECK_NEAR_VEC(after.velocity, before.velocity);
    CHECK_NEAR_VEC(after.scale, before.scale);
    CHECK_NEAR(after.angle, before.angle);
}

// --- real pixels -------------------------------------------------------------
//
// The four groups below use a real window, the real renderer, the real shipped
// configuration and a real 64x64 image, and read pixels back off the screen.
// mario_ground is opaque orange-brown (190,77,19) at its centre.

// A 64x64 image centred on (200,200) spans 168..232 unscaled and 136..264 at
// scale 2, so (150,200) is outside the first and inside the second.
constexpr int kProbeX = 150;
constexpr int kProbeY = 200;

void testARealConfiguredTextureRendersPixels()
{
    sf::RenderWindow window(sf::VideoMode{200, 200}, "texture pixels");
    graphics::SfmlRenderer renderer{window};
    const assets::SfmlAssetManager assets = shippedAssets();
    const Camera camera;
    RenderSystem renderSystem{renderer, camera, assets};
    [[maybe_unused]] Input input;

    EntityManager manager;
    addTexturedEntity(manager, "mario_ground", Vec2{100.0F, 100.0F});

    renderer.beginFrame();
    renderer.clear(Color{0.0F, 0.0F, 1.0F, 1.0F});
    renderSystem.update(manager, input, 0.0F);

    // The image spans 68..132, so the centre is image and the corner is not.
    const sf::Color centre = readPixel(window, 100, 100);
    const sf::Color outside = readPixel(window, 5, 5);
    renderer.endFrame();

    CHECK(centre.r == 190);
    CHECK(centre.g == 77);
    CHECK(centre.b == 19);
    CHECK(outside.b == 255);
    CHECK(outside.r == 0);
}

void testTexturePositionAffectsRenderedPixels()
{
    sf::RenderWindow window(sf::VideoMode{200, 200}, "texture position");
    graphics::SfmlRenderer renderer{window};
    const assets::SfmlAssetManager assets = shippedAssets();
    const Camera camera;
    RenderSystem renderSystem{renderer, camera, assets};
    [[maybe_unused]] Input input;

    EntityManager manager;
    // Off-centre, so the two candidate positions are distinguishable.
    addTexturedEntity(manager, "mario_ground", Vec2{50.0F, 50.0F});

    renderer.beginFrame();
    renderer.clear(Color{0.0F, 0.0F, 1.0F, 1.0F});
    renderSystem.update(manager, input, 0.0F);

    // At (50,50) the image spans 18..82, so (50,50) is image and (150,150) is not.
    const sf::Color atPosition = readPixel(window, 50, 50);
    const sf::Color elsewhere = readPixel(window, 150, 150);
    renderer.endFrame();

    CHECK(atPosition.r == 190);
    CHECK(atPosition.g == 77);
    CHECK(atPosition.b == 19);
    CHECK(elsewhere.b == 255);
    CHECK(elsewhere.r == 0);
}

void testTextureScaleAffectsRenderedPixels()
{
    // One window drawn twice, so scale is the only difference and the two frames
    // are directly comparable.
    sf::RenderWindow window(sf::VideoMode{400, 400}, "texture scale");
    graphics::SfmlRenderer renderer{window};
    const assets::SfmlAssetManager assets = shippedAssets();
    const Camera camera;
    RenderSystem renderSystem{renderer, camera, assets};
    [[maybe_unused]] Input input;

    EntityManager unscaledManager;
    addTexturedEntity(unscaledManager, "mario_ground", Vec2{200.0F, 200.0F}, Vec2{1.0F, 1.0F});

    renderer.beginFrame();
    renderer.clear(Color{0.0F, 0.0F, 1.0F, 1.0F});
    renderSystem.update(unscaledManager, input, 0.0F);
    const sf::Color atScaleOne = readPixel(window, kProbeX, kProbeY);
    renderer.endFrame();

    EntityManager doubledManager;
    addTexturedEntity(doubledManager, "mario_ground", Vec2{200.0F, 200.0F}, Vec2{2.0F, 2.0F});

    renderer.beginFrame();
    renderer.clear(Color{0.0F, 0.0F, 1.0F, 1.0F});
    renderSystem.update(doubledManager, input, 0.0F);
    const sf::Color atScaleTwo = readPixel(window, kProbeX, kProbeY);
    renderer.endFrame();

    CHECK(atScaleOne.b == 255);
    CHECK(atScaleOne.r == 0);
    CHECK(atScaleTwo.r == 190);
    CHECK(atScaleTwo.g == 77);
    CHECK(atScaleTwo.b == 19);
}

void testCameraZoomAffectsRenderedTexturePixels()
{
    // The camera reaches textures through the same RenderTransform as everything
    // else. There is no separate texture camera, so this is the rectangle zoom
    // test with an image in place of a shape.
    sf::RenderWindow window(sf::VideoMode{400, 400}, "texture zoom");
    graphics::SfmlRenderer renderer{window};
    const assets::SfmlAssetManager assets = shippedAssets();
    [[maybe_unused]] Input input;

    // The entity sits at the camera's own position, so it lands on the screen
    // centre and stays there at any zoom. worldToScreen is
    // (world - cameraPosition) * zoom + screenCentre, so a world position of
    // (0,0) with a camera at (0,0) is always exactly the screen centre.
    EntityManager manager;
    addTexturedEntity(manager, "mario_ground", Vec2{0.0F, 0.0F});

    // The camera's viewport is the window, so its screen centre is (200,200). That matters: zoom also scales a world
    // position's offset from the centre, so an entity away from the centre would
    // slide out from under the probe and the test would measure the wrong thing.
    Camera unzoomed;
    unzoomed.setViewport(Vec2{400.0F, 400.0F});
    renderer.beginFrame();
    renderer.clear(Color{0.0F, 0.0F, 1.0F, 1.0F});
    RenderSystem{renderer, unzoomed, assets}.update(manager, input, 0.0F);
    const sf::Color atZoomOne = readPixel(window, kProbeX, kProbeY);
    renderer.endFrame();

    Camera zoomed;
    zoomed.setViewport(Vec2{400.0F, 400.0F});
    zoomed.setZoom(2.0F);
    renderer.beginFrame();
    renderer.clear(Color{0.0F, 0.0F, 1.0F, 1.0F});
    RenderSystem{renderer, zoomed, assets}.update(manager, input, 0.0F);
    const sf::Color atZoomTwo = readPixel(window, kProbeX, kProbeY);
    renderer.endFrame();

    CHECK(atZoomOne.b == 255);
    CHECK(atZoomOne.r == 0);
    CHECK(atZoomTwo.r == 190);
    CHECK(atZoomTwo.g == 77);
    CHECK(atZoomTwo.b == 19);
}

void testRectangleRenderingIsUnchangedAlongsideTextures()
{
    // The regression guard for the whole step: with a texture in the world, a
    // rectangle still lands on exactly the pixels it always did.
    sf::RenderWindow window(sf::VideoMode{200, 200}, "rectangle unchanged");
    graphics::SfmlRenderer renderer{window};
    const assets::SfmlAssetManager assets = shippedAssets();
    const Camera camera;
    RenderSystem renderSystem{renderer, camera, assets};
    [[maybe_unused]] Input input;

    EntityManager manager;
    Entity& rectangle = manager.addEntity("rectangle");
    rectangle.addComponent<Transform>(Transform{Vec2{30.0F, 30.0F}, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
    rectangle.addComponent<Rectangle>(Rectangle{Vec2{20.0F, 20.0F}, Color{0.0F, 1.0F, 0.0F, 1.0F}});
    addTexturedEntity(manager, "mario_ground", Vec2{150.0F, 150.0F});

    renderer.beginFrame();
    renderer.clear(Color{0.0F, 0.0F, 1.0F, 1.0F});
    renderSystem.update(manager, input, 0.0F);

    const sf::Color inRectangle = readPixel(window, 30, 30);
    const sf::Color inTexture = readPixel(window, 150, 150);
    renderer.endFrame();

    // The rectangle is still exactly its own green, undisturbed.
    CHECK(inRectangle.g == 255);
    CHECK(inRectangle.r == 0);
    CHECK(inRectangle.b == 0);

    // And the texture drew its own colour, undisturbed.
    CHECK(inTexture.r == 190);
    CHECK(inTexture.g == 77);
    CHECK(inTexture.b == 19);
}

// ---------------------------------------------------------------------------
// Source rectangles
//
// The course's animation mechanism is a texture rectangle: an animation shows one
// frame of a sheet by changing which part of the sheet is drawn. These groups
// cover the draw call that makes that possible - that a region is honoured, that
// it is centred on the placement rather than on the sheet, and that a whole-image
// draw is byte-for-byte what it always was.
//
// The pixel expectations are derived from the committed artwork rather than
// guessed. `mario/GoombaWalk.png` is 100x41 with two 50x41 frames: frame 0's
// opaque pixels span local x 0..43 and frame 1's span local x 2..47, so local
// (45, 20) is transparent in frame 0 and opaque in frame 1, while local (5, 20)
// is opaque in both. Drawn centred at (100, 50) those are screen (120, 50) and
// (80, 50), which is what the pixel groups assert.
// ---------------------------------------------------------------------------

/// A real manager over the shipped configuration, so the pixel groups below use
/// a real loaded image rather than a stand-in.
[[nodiscard]] assets::SfmlAssetManager shippedManager()
{
    return assets::SfmlAssetManager{std::filesystem::path{config::kAssetsConfig}};
}

void testAWholeTextureDrawNamesNoSourceRegion()
{
    FakeAssetManager assets;
    assets.declare("mario_ground");
    RecordingRenderer renderer;
    Camera camera = identityCamera();
    RenderSystem system{renderer, camera, assets};

    EntityManager entities;
    Entity& entity = entities.addEntity("textured");
    entity.addComponent<Transform>(Transform{Vec2{10.0F, 20.0F}, Vec2{}, Vec2{1.0F, 1.0F}, 0.0F});
    entity.addComponent<components::Texture>(components::Texture{"mario_ground"});

    Input input;
    renderer.beginFrame();
    system.update(entities, input, 0.0F);

    CHECK(renderer.textureDraws().size() == 1U);
    if (renderer.textureDraws().size() != 1U)
    {
        return;
    }

    // Absent, not an empty rect. The distinction is what lets an implementation
    // tell "draw everything" from "draw nothing", and the draw order record uses
    // the same distinction to label the call.
    CHECK_FALSE(renderer.textureDraws()[0].source.has_value());
}

void testASourceRegionIsRecordedOnTheDrawCall()
{
    FakeAssetManager assets;
    assets.declare("mario_goomba");
    RecordingRenderer renderer;

    renderer.beginFrame();

    // Through the renderer directly rather than through a system, so this group is
    // about the draw call's contract and nothing else. The system that submits
    // regions is covered by the animation groups.
    static_cast<graphics::Renderer&>(renderer)
        .drawTexture(assets.texture("mario_goomba"), RenderTransform{Vec2{10.0F, 20.0F}, Vec2{1.0F, 1.0F}, 0.0F},
                     IntRect{50, 0, 50, 41});

    CHECK(renderer.textureDraws().size() == 1U);
    if (renderer.textureDraws().size() != 1U)
    {
        return;
    }

    CHECK(renderer.textureDraws()[0].source.has_value());
    if (!renderer.textureDraws()[0].source.has_value())
    {
        return;
    }

    CHECK(*renderer.textureDraws()[0].source == (IntRect{50, 0, 50, 41}));
}

void testTwoFramesOfOneSheetSubmitTwoDistinctDrawCalls()
{
    // Two frames of the same sheet, same placement, submitted in one frame. Both
    // calls must survive with their own region, and the recorded order must show a
    // region-bearing call is distinguishable from a whole-image one - otherwise a
    // system that animating silently drew whole sheets would look identical.
    FakeAssetManager assets;
    assets.declare("mario_goomba");
    RecordingRenderer renderer;

    const RenderTransform placement{Vec2{10.0F, 20.0F}, Vec2{1.0F, 1.0F}, 0.0F};
    const IntRect first{0, 0, 50, 41};
    const IntRect second{50, 0, 50, 41};

    CHECK(first != second);
    CHECK(first.left != second.left);
    CHECK(first.top == second.top);
    CHECK(first.width == second.width);
    CHECK(first.height == second.height);

    renderer.beginFrame();
    static_cast<graphics::Renderer&>(renderer).drawTexture(assets.texture("mario_goomba"), placement, first);
    static_cast<graphics::Renderer&>(renderer).drawTexture(assets.texture("mario_goomba"), placement, second);
    static_cast<graphics::Renderer&>(renderer).drawTexture(assets.texture("mario_goomba"), placement, std::nullopt);
    renderer.endFrame();

    CHECK(renderer.textureDraws().size() == 3U);
    if (renderer.textureDraws().size() != 3U)
    {
        return;
    }

    CHECK(renderer.textureDraws()[0].source.has_value());
    CHECK(renderer.textureDraws()[1].source.has_value());
    CHECK_FALSE(renderer.textureDraws()[2].source.has_value());

    if (renderer.textureDraws()[0].source.has_value() && renderer.textureDraws()[1].source.has_value())
    {
        CHECK(*renderer.textureDraws()[0].source == first);
        CHECK(*renderer.textureDraws()[1].source == second);
        CHECK(*renderer.textureDraws()[0].source != *renderer.textureDraws()[1].source);
    }

    // Same handle both times: two frames of one image, not two images.
    CHECK(renderer.textureDraws()[0].texture == renderer.textureDraws()[1].texture);

    // The order record distinguishes a region draw from a whole-image draw.
    const std::vector<std::string>& order = renderer.order();
    CHECK(order.size() == 3U);
    if (order.size() == 3U)
    {
        CHECK_STR(order[0], "texture-region");
        CHECK_STR(order[1], "texture-region");
        CHECK_STR(order[2], "texture");
    }
}

void testSourceRegionDefaultsToAnEmptyRect()
{
    // The default-constructed rect is empty, and `isEmpty` is what says so. It is
    // not overloaded to mean "everything" - see the note on the draw call.
    const IntRect empty;

    CHECK(isEmpty(empty));
    CHECK(empty.left == 0);
    CHECK(empty.top == 0);
    CHECK(empty.width == 0);
    CHECK(empty.height == 0);

    // A degenerate region is empty too, rather than an inverted one.
    CHECK(isEmpty(IntRect{10, 10, -5, 20}));
    CHECK(isEmpty(IntRect{10, 10, 20, -5}));
    CHECK_FALSE(isEmpty(IntRect{0, 0, 1, 1}));

    // Equality is exact and by value, so a caller can compare a computed region
    // with a recorded one.
    CHECK(IntRect{1, 2, 3, 4} == IntRect{1, 2, 3, 4});
    CHECK(IntRect{1, 2, 3, 4} != IntRect{1, 2, 3, 5});
}

void testFrameZeroDrawsTheFirstFramesPixels()
{
    // A real window, a real image, a real pixel read. If the region were ignored,
    // or interpreted as a destination, or applied as a scale, this would fail.
    sf::RenderWindow window(sf::VideoMode{200, 100}, "frame zero");
    engine::graphics::SfmlRenderer renderer{window};
    assets::SfmlAssetManager assets = shippedManager();

    constexpr Color background{0.0F, 0.0F, 1.0F, 1.0F};

    renderer.beginFrame();
    renderer.clear(background);
    renderer.drawTexture(assets.texture("mario_GoombaWalk"),
                         RenderTransform{Vec2{100.0F, 50.0F}, Vec2{1.0F, 1.0F}, 0.0F}, IntRect{0, 0, 50, 41});

    // Local (5, 20) of frame 0 is opaque, so screen (80, 50) is sprite, not
    // background.
    const sf::Color inside = readPixel(window, 80, 50);

    // Local (45, 20) of frame 0 is transparent, so screen (120, 50) is the
    // background. This is the discriminating pixel: the same pixel in frame 1 is
    // sprite.
    const sf::Color outsideFrame = readPixel(window, 120, 50);

    renderer.endFrame();

    CHECK(inside.a == 255);
    CHECK(inside != sf::Color{0, 0, 255, 255});
    CHECK(outsideFrame.b == 255);
    CHECK(outsideFrame.r == 0);
    CHECK(outsideFrame.a == 255);
}

void testFrameOneDrawsTheSecondFramesPixels()
{
    sf::RenderWindow window(sf::VideoMode{200, 100}, "frame one");
    engine::graphics::SfmlRenderer renderer{window};
    assets::SfmlAssetManager assets = shippedManager();

    constexpr Color background{0.0F, 0.0F, 1.0F, 1.0F};

    renderer.beginFrame();
    renderer.clear(background);
    renderer.drawTexture(assets.texture("mario_GoombaWalk"),
                         RenderTransform{Vec2{100.0F, 50.0F}, Vec2{1.0F, 1.0F}, 0.0F}, IntRect{50, 0, 50, 41});

    // Both of these are opaque in frame 1: local x 5 and local x 45.
    const sf::Color leftOfCentre = readPixel(window, 80, 50);
    const sf::Color rightOfCentre = readPixel(window, 120, 50);

    renderer.endFrame();

    CHECK(leftOfCentre.a == 255);
    CHECK(rightOfCentre.a == 255);
    CHECK(rightOfCentre != sf::Color{0, 0, 255, 255});
}

void testTwoFramesOfOneSheetLookDifferentOnScreen()
{
    // The end-to-end statement of the whole feature: the same texture, the same
    // placement, two regions, two different pictures. Frame 0 is transparent at
    // screen (120, 50); frame 1 is opaque there. If the source region were
    // ignored both would be identical, and if it were treated as a destination
    // both would be drawn at the wrong place.
    auto pixelsFor = [](const IntRect& region) {
        sf::RenderWindow window(sf::VideoMode{200, 100}, "compare frames");
        engine::graphics::SfmlRenderer renderer{window};
        assets::SfmlAssetManager assets = shippedManager();

        renderer.beginFrame();
        renderer.clear(Color{0.0F, 0.0F, 1.0F, 1.0F});
        renderer.drawTexture(assets.texture("mario_GoombaWalk"),
                             RenderTransform{Vec2{100.0F, 50.0F}, Vec2{1.0F, 1.0F}, 0.0F}, region);

        const sf::Color at = readPixel(window, 120, 50);
        const sf::Color edge = readPixel(window, 123, 50);
        const sf::Color left = readPixel(window, 80, 50);
        renderer.endFrame();
        return std::tuple<sf::Color, sf::Color, sf::Color>{at, edge, left};
    };

    const auto [frameZeroAt, frameZeroEdge, frameZeroLeft] = pixelsFor(IntRect{0, 0, 50, 41});
    const auto [frameOneAt, frameOneEdge, frameOneLeft] = pixelsFor(IntRect{50, 0, 50, 41});

    // The control, and the reason the comparison is meaningful. Local x 48 is
    // transparent in *both* frames - frame 0's opaque run ends at 43 and frame
    // 1's at 47 - so screen (123, 50) is background whichever frame is drawn. If
    // it were not, the two frames would be sitting in different places rather
    // than showing different pictures, which is a different bug with a different
    // fix.
    CHECK(frameZeroEdge.b == 255);
    CHECK(frameOneEdge.b == 255);

    // Both frames are drawn into the same 50x41 box, so both have opaque pixels
    // somewhere in it. Local x 5 is opaque in each.
    CHECK(frameZeroLeft.a == 255);
    CHECK(frameOneLeft.a == 255);

    // The discriminating pixel. Local x 45 of frame 0 is transparent and of frame
    // 1 is a semi-opaque brown, so the same screen pixel goes from background to
    // sprite purely because the source region moved.
    CHECK(frameZeroAt != frameOneAt);
    CHECK(frameZeroAt.b == 255);
    CHECK(frameZeroAt.a == 255);
    CHECK(frameOneAt.a == 255);
    CHECK(frameOneAt.b != 255);
}

void testASourceRegionIsCentredOnThePlacementNotTheSheet()
{
    // The single most important property of the feature, and the one the course
    // calls out: an animated character must not drift sideways as it changes
    // frame.
    //
    // The sheet is 100 wide and the frame is 50, so if the origin were the
    // *sheet's* centre the frame would be drawn 25 pixels to the right. Frame 1's
    // opaque run is local x 2..47, so at screen (120, 50) - which is local x 45 of
    // a frame centred on (100, 50) - there would be nothing. Both of the pixels
    // below are inside a correctly centred frame.
    sf::RenderWindow window(sf::VideoMode{200, 100}, "frame centring");
    engine::graphics::SfmlRenderer renderer{window};
    assets::SfmlAssetManager assets = shippedManager();

    renderer.beginFrame();
    renderer.clear(Color{0.0F, 0.0F, 1.0F, 1.0F});

    // Frame 1, the right-hand half of the sheet. Centred, it covers screen x
    // 75..124. Its local x 2 is screen 77, which is the first opaque column.
    renderer.drawTexture(assets.texture("mario_GoombaWalk"),
                         RenderTransform{Vec2{100.0F, 50.0F}, Vec2{1.0F, 1.0F}, 0.0F}, IntRect{50, 0, 50, 41});

    const sf::Color justInside = readPixel(window, 80, 50);
    const sf::Color farInside = readPixel(window, 120, 50);
    const sf::Color leftOfTheFrame = readPixel(window, 60, 50);
    const sf::Color rightOfTheFrame = readPixel(window, 140, 50);

    renderer.endFrame();

    CHECK(justInside.a == 255);
    CHECK(farInside.a == 255);

    // The frame is 50 wide centred on 100, so it cannot reach 60 or 140. If the
    // sheet's centre were used instead the frame would cover 100..149 and screen
    // 140 would be sprite.
    CHECK(leftOfTheFrame.b == 255);
    CHECK(rightOfTheFrame.b == 255);
}

void testAnEmptySourceRegionDrawsNothing()
{
    // An animation asked for a frame it does not have should produce nothing, not
    // the whole texture and not an exception. Both alternatives would be worse:
    // the whole texture would flash a wrong picture, and an exception would take
    // down a frame for a recoverable mistake.
    sf::RenderWindow window(sf::VideoMode{200, 100}, "empty region");
    engine::graphics::SfmlRenderer renderer{window};
    assets::SfmlAssetManager assets = shippedManager();

    renderer.beginFrame();
    renderer.clear(Color{0.0F, 0.0F, 1.0F, 1.0F});
    renderer.drawTexture(assets.texture("mario_GoombaWalk"),
                         RenderTransform{Vec2{100.0F, 50.0F}, Vec2{1.0F, 1.0F}, 0.0F}, IntRect{});

    const sf::Color centre = readPixel(window, 100, 50);
    const sf::Color edge = readPixel(window, 80, 50);
    renderer.endFrame();

    CHECK(centre.b == 255);
    CHECK(centre.r == 0);
    CHECK(edge.b == 255);
    CHECK(renderer.frameCount() == 1U);
}

void testAWholeTextureDrawIsUnchangedByTheRegionParameter()
{
    // The regression this API change had to avoid: a texture drawn with no region
    // must look exactly as it did before the parameter existed. Both halves of the
    // sheet visible at once, centred on the whole image's centre (100, 50) - which
    // is also the frame's centre, so both sprites land on screen together.
    sf::RenderWindow window(sf::VideoMode{200, 100}, "whole image");
    engine::graphics::SfmlRenderer renderer{window};
    assets::SfmlAssetManager assets = shippedManager();

    renderer.beginFrame();
    renderer.clear(Color{0.0F, 0.0F, 1.0F, 1.0F});
    renderer.drawTexture(assets.texture("mario_GoombaWalk"),
                         RenderTransform{Vec2{100.0F, 50.0F}, Vec2{1.0F, 1.0F}, 0.0F}, std::nullopt);

    // The sheet is 100 wide centred on 100, so it covers screen x 50..149. Both
    // halves have opaque pixels here: frame 0's local x 0 is screen 50, and frame
    // 1's local x 45 is screen 95.
    const sf::Color frameZeroHalf = readPixel(window, 70, 50);
    const sf::Color frameOneHalf = readPixel(window, 120, 50);
    const sf::Color outside = readPixel(window, 160, 50);

    renderer.endFrame();

    CHECK(frameZeroHalf.a == 255);
    CHECK(frameOneHalf.a == 255);
    CHECK(outside.b == 255);
}

void testAnExplicitWholeImageRegionMatchesNoRegion()
{
    // `std::nullopt` and an explicit rect covering the whole image must produce
    // the same picture, or a caller that computed a region from an animation's
    // frame size would see a sprite jump when the frame count is 1.
    auto pixelsFor = [](const std::optional<IntRect>& source) {
        sf::RenderWindow window(sf::VideoMode{200, 100}, "whole region");
        engine::graphics::SfmlRenderer renderer{window};
        assets::SfmlAssetManager assets = shippedManager();

        renderer.beginFrame();
        renderer.clear(Color{0.0F, 0.0F, 1.0F, 1.0F});
        renderer.drawTexture(assets.texture("mario_GoombaWalk"),
                             RenderTransform{Vec2{100.0F, 50.0F}, Vec2{1.0F, 1.0F}, 0.0F}, source);

        std::vector<sf::Color> row;
        for (int x = 40; x < 160; x += 8)
        {
            row.push_back(readPixel(window, static_cast<std::size_t>(x), 50));
        }
        renderer.endFrame();
        return row;
    };

    const auto noRegion = pixelsFor(std::nullopt);
    const auto wholeRegion = pixelsFor(IntRect{0, 0, 100, 41});

    CHECK(noRegion == wholeRegion);
}

void testASourceRegionHonoursScale()
{
    // Scale multiplies the region's size, not the sheet's. At 2x the 50x41 frame
    // covers 100x82 centred on (100, 50), so it reaches screen x 50..149 and
    // screen y 9..90 - and at 1x it would have reached only 75..124.
    sf::RenderWindow window(sf::VideoMode{200, 100}, "region scale");
    engine::graphics::SfmlRenderer renderer{window};
    assets::SfmlAssetManager assets = shippedManager();

    renderer.beginFrame();
    renderer.clear(Color{0.0F, 0.0F, 1.0F, 1.0F});
    renderer.drawTexture(assets.texture("mario_GoombaWalk"),
                         RenderTransform{Vec2{100.0F, 50.0F}, Vec2{2.0F, 2.0F}, 0.0F}, IntRect{0, 0, 50, 41});

    // Frame 0's opaque run is local x 0..43, so at 2x on screen x 50..136.
    const sf::Color inside = readPixel(window, 70, 50);
    const sf::Color beyondTheScaledFrame = readPixel(window, 150, 50);
    renderer.endFrame();

    CHECK(inside.a == 255);
    CHECK(beyondTheScaledFrame.b == 255);
}

void testASourceRegionDoesNotChangeTheCameraMapping()
{
    // The camera is applied to the placement before the draw call, exactly as it
    // is for a rectangle and for a whole texture. A region must not reintroduce a
    // world-to-screen step inside the renderer.
    FakeAssetManager assets;
    assets.declare("mario_ground");
    RecordingRenderer renderer;
    Input input;
    static_cast<void>(input);

    Camera camera = identityCamera();
    camera.setPosition(Vec2{500.0F, 300.0F});
    camera.setZoom(2.0F);

    const RenderTransform worldPlacement = toRenderTransform(
        Transform{Vec2{500.0F, 300.0F}, Vec2{}, Vec2{1.0F, 1.0F}, 0.0F}, camera);

    // With the camera centred on the entity and zoomed 2x, the screen scale is 2
    // and the world position maps to the viewport centre.
    CHECK(worldPlacement.position == camera.worldToScreen(Vec2{500.0F, 300.0F}));
    CHECK(worldPlacement.scale.x == camera.zoom());
    CHECK(worldPlacement.scale.y == camera.zoom());

    // A region does not participate: the placement the renderer receives is
    // identical whether or not a region accompanies it, so a region cannot have
    // shifted the camera mapping.
    static_cast<graphics::Renderer&>(renderer).drawTexture(
        assets.texture("mario_ground"), worldPlacement, IntRect{0, 0, 32, 32});
    CHECK(renderer.textureDraws().size() == 1U);
    if (renderer.textureDraws().size() == 1U)
    {
        CHECK(renderer.textureDraws()[0].placement.position == worldPlacement.position);
        CHECK(renderer.textureDraws()[0].placement.scale == worldPlacement.scale);
        CHECK(renderer.textureDraws()[0].source.has_value());
        if (renderer.textureDraws()[0].source.has_value())
        {
            CHECK(*renderer.textureDraws()[0].source == (IntRect{0, 0, 32, 32}));
        }
    }
}

void testTheRendererInterfaceNamesNoGraphicsType()
{
    // The whole reason the engine has its own IntRect. Checked on the header's
    // code with comments stripped, because the interface's documentation names
    // `sf::IntRect` while explaining that the type is never exposed, and scanning
    // the raw text would match its own explanation.
    std::ifstream file{ENGINE_RENDERER_HEADER};
    CHECK(static_cast<bool>(file));
    if (!file)
    {
        return;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string code = buffer.str();

    CHECK(!code.empty());
    if (code.empty())
    {
        return;
    }

    // Strip `//` comments and blank lines, so only code is searched.
    std::string stripped;
    {
        std::istringstream lines{code};
        std::string line;
        while (std::getline(lines, line))
        {
            const std::string withoutComment = line.substr(0, line.find("//"));
            if (withoutComment.find_first_not_of(" \t") != std::string::npos)
            {
                stripped += withoutComment;
                stripped += '\n';
            }
        }
    }

    CHECK(stripped.find("sf::") == std::string::npos);
    CHECK(stripped.find("SFML") == std::string::npos);
    CHECK(stripped.find("#include <SFML") == std::string::npos);

    // The region type is the engine's own and is included, not forward declared.
    CHECK(stripped.find("#include \"engine/math/IntRect.hpp\"") != std::string::npos);
    CHECK(stripped.find("std::optional<IntRect>") != std::string::npos);
    CHECK(stripped.find("IntRect& source") == std::string::npos);
}

void testAWholeImageIsNotOverloadedToMeanEverything()
{
    // `kWholeImage` exists as a named constant for readability, but it is a
    // zero-sized rect, which is empty - so it cannot be passed where "draw the
    // whole image" is meant. That is deliberate: overloading zero to mean the
    // opposite of what it says would be a trap. The whole-image case is
    // `std::nullopt`.
    CHECK(isEmpty(kWholeImage));
    CHECK(kWholeImage.width == 0);
    CHECK(kWholeImage.height == 0);
}

// ---------------------------------------------------------------------------
// Step 8: Application owns the asset manager.
//
// Before this step, RenderSystem could be built without one, and a nullable
// member carried the "assets unavailable" state. Now Application owns the
// manager, hands it down by reference, and the bridge is gone. These groups
// prove the ownership, the wiring, and the absence of the old state.
// ---------------------------------------------------------------------------

[[nodiscard]] std::string readEngineSource(const char* const path)
{
    std::ifstream file{path};
    if (!file)
    {
        std::cerr << "    unable to read source file: " << path << '\n';
        CHECK(false);
        return {};
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

void testApplicationConstructsWithTheRealConfiguration()
{
    // Construction loads the configured assets, so simply getting here means the
    // shipped configuration was read and every file in it opened. A missing or
    // broken configuration would have thrown before this line.
    Application application;

    // The configured path is absolute, so nothing about loading can depend on
    // where the process happens to be. A relative path here would be a working
    // directory assumption wearing a disguise.
    CHECK(std::filesystem::path{config::kAssetsConfig}.is_absolute());
    CHECK(std::filesystem::exists(std::filesystem::path{config::kAssetsConfig}));

    // The window is open and nothing has been presented yet.
    CHECK(application.renderer().frameCount() == 0U);
}

/// Restores the working directory however the scope is left, including by an
/// exception, so a failing test cannot quietly change the process for the rest of
/// the run.
class ScopedWorkingDirectory
{
public:
    explicit ScopedWorkingDirectory(const std::filesystem::path& directory)
        : m_original{std::filesystem::current_path()}
    {
        std::error_code error;
        std::filesystem::current_path(directory, error);
    }

    ~ScopedWorkingDirectory()
    {
        std::error_code error;
        std::filesystem::current_path(m_original, error);
    }

    ScopedWorkingDirectory(const ScopedWorkingDirectory&) = delete;
    ScopedWorkingDirectory& operator=(const ScopedWorkingDirectory&) = delete;

private:
    std::filesystem::path m_original;
};

void testAssetLoadingDoesNotDependOnTheWorkingDirectory()
{
    // The requirement was "do not silently fall back to CWD assumptions", and a
    // unit test asserting the configured string is absolute does not really prove
    // it. This does: the process is moved somewhere else entirely, and the engine
    // must still find and load exactly the same assets.
    bool resolved = false;
    std::string reported;

    {
        const ScopedWorkingDirectory elsewhere{std::filesystem::temp_directory_path()};

        try
        {
            const Application application;
            (void)application.assets().texture("mario_ground");
            resolved = true;
        }
        catch (const std::exception& error)
        {
            reported = error.what();
        }
    }

    if (!resolved)
    {
        std::cerr << "    loading failed from a different working directory: " << reported << '\n';
    }
    CHECK(resolved);
}

void testApplicationOwnsALoadedAssetManager()
{
    const Application application;

    // A real asset, resolvable by its configured name, and the same object every
    // time: one manager, loaded once, owned by the Application.
    const assets::Texture& first = application.assets().texture("mario_ground");
    const assets::Texture& second = application.assets().texture("mario_ground");
    CHECK(&first == &second);

    // And a name that is not configured is still refused, loudly, through the
    // same manager. Nothing about owning it made the lookup laxer.
    bool threw = false;
    try
    {
        (void)application.assets().texture("definitely_not_configured");
    }
    catch (const assets::AssetNotFoundError&)
    {
        threw = true;
    }
    CHECK(threw);
}

void testATexturedEntityRendersThroughTheApplication()
{
    // The integration that Step 7 could not reach: not a RenderSystem built in a
    // test, but the one the Application owns.
    //
    // This is the proof that Application's manager actually reaches RenderSystem.
    // "mario_ground" exists only in the shipped configuration, so if the render
    // system were looking anywhere else the lookup would throw AssetNotFoundError
    // out of run(). Reaching EXIT_SUCCESS means the name resolved.
    Application application;

    Entity& entity = application.entityManager().addEntity("mario_ground");
    entity.addComponent<Transform>(Transform{Vec2{100.0F, 100.0F}, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
    entity.addComponent<components::Texture>(components::Texture{"mario_ground"});

    CHECK(application.run(3) == EXIT_SUCCESS);
    CHECK(application.renderer().frameCount() == 3U);

    // A rectangle in the same application still renders, so owning assets did not
    // cost the existing path anything.
    Entity& rectangle = application.entityManager().addEntity("rectangle");
    rectangle.addComponent<Transform>(Transform{Vec2{50.0F, 50.0F}, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
    rectangle.addComponent<Rectangle>(Rectangle{Vec2{20.0F, 20.0F}, Color{0.0F, 1.0F, 0.0F, 1.0F}});

    CHECK(application.run(2) == EXIT_SUCCESS);
    CHECK(application.renderer().frameCount() == 5U);
}

void testAnUndeclaredTextureNameStillFailsLoudlyThroughTheApplication()
{
    // The other half of the wiring proof. If the render system were skipping
    // unresolvable names, this frame would succeed and the test would be showing
    // a silent failure instead. The error has to escape run().
    Application application;

    Entity& entity = application.entityManager().addEntity("ghost");
    entity.addComponent<Transform>(Transform{Vec2{10.0F, 10.0F}, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
    entity.addComponent<components::Texture>(components::Texture{"not_configured_anywhere"});

    bool threw = false;
    try
    {
        (void)application.run(2);
    }
    catch (const assets::AssetNotFoundError&)
    {
        threw = true;
    }

    CHECK(threw);
}

void testRenderSystemDoesNotOwnTheAssetManager()
{
    // Two independent guarantees.
    //
    // First, the type system already forbids ownership: AssetManager is abstract,
    // so it has no size and cannot be a by-value member of anything. There is no
    // way for RenderSystem to hold one, whatever it wanted to.
    static_assert(std::is_abstract_v<assets::AssetManager>, "AssetManager must stay abstract");
    static_assert(!std::is_default_constructible_v<assets::AssetManager>,
                  "an abstract interface cannot be a by-value member, which is what stops "
                  "RenderSystem owning one");

    // Second, the header says it holds a reference. A raw pointer could be null
    // again, and the "never null" guarantee is only worth something if the type
    // cannot express otherwise.
    const std::string header = readEngineSource(ENGINE_RENDER_SYSTEM_HEADER);
    CHECK(!header.empty());
    if (header.empty())
    {
        return;
    }

    CHECK(header.find("const assets::AssetManager& m_assets;") != std::string::npos);
    CHECK(header.find("const assets::AssetManager* m_assets") == std::string::npos);
    CHECK(header.find("assets::AssetManager m_assets") == std::string::npos);

    // And the temporary two-argument constructor really is gone, not just unused.
    CHECK(header.find("RenderSystem(graphics::Renderer& renderer, const graphics::Camera& camera)") ==
          std::string::npos);
}

void testApplicationMemberOrderGivesTheManagerTheLongerLife()
{
    // Destruction is the reverse of declaration, so the guarantee that the asset
    // manager outlives the render system is a property of the declaration order.
    // That is invisible to the compiler and to any runtime test, so it is checked
    // against the header text rather than trusted to a comment.
    const std::string header = readEngineSource(ENGINE_APPLICATION_HEADER);
    CHECK(!header.empty());
    if (header.empty())
    {
        return;
    }

    const std::size_t window = header.find("sf::RenderWindow m_window;");
    const std::size_t camera = header.find("graphics::Camera m_camera;");
    const std::size_t renderer = header.find("graphics::SfmlRenderer m_renderer;");
    const std::size_t assets = header.find("assets::SfmlAssetManager m_assets;");
    const std::size_t renderSystem = header.find("systems::RenderSystem m_renderSystem;");

    CHECK(window != std::string::npos);
    CHECK(camera != std::string::npos);
    CHECK(renderer != std::string::npos);
    CHECK(assets != std::string::npos);
    CHECK(renderSystem != std::string::npos);

    if (window == std::string::npos || camera == std::string::npos || renderer == std::string::npos ||
        assets == std::string::npos || renderSystem == std::string::npos)
    {
        return;
    }

    // Everything the render system borrows is declared before it, so all of it is
    // still alive when the render system is destroyed.
    CHECK(renderSystem > renderer);
    CHECK(renderSystem > camera);
    CHECK(renderSystem > assets);

    // The renderer binds to the window, so the window comes first.
    CHECK(renderer > window);

    // And Application owns a manager by value rather than holding a pointer to
    // one, which is what makes the lifetime automatic rather than a matter of
    // somebody remembering to clean up.
    CHECK(header.find("assets::AssetManager* m_assets") == std::string::npos);
}

void testThereIsNoGlobalOrStaticAssetManager()
{
    // The engine has no singletons and no global state, and an asset manager is
    // exactly the thing that would tempt someone into one. Nothing in the engine
    // may hold an AssetManager except Application's member and RenderSystem's
    // reference to it.
    //
    // A file-scope declaration is the thing being ruled out, so the scan is over
    // the engine's own sources and looks for a static or namespace-scope manager.
    const char* const files[] = {ENGINE_APPLICATION_HEADER, ENGINE_RENDER_SYSTEM_HEADER, ENGINE_APPLICATION_SOURCE,
                                ENGINE_RENDER_SYSTEM_SOURCE};

    for (const char* const file : files)
    {
        const std::string source = readEngineSource(file);
        CHECK(!source.empty());
        if (source.empty())
        {
            continue;
        }

        // No static or thread_local manager, and no instance at namespace scope.
        CHECK(source.find("static assets::AssetManager") == std::string::npos);
        CHECK(source.find("thread_local") == std::string::npos);
        CHECK(source.find("inline assets::AssetManager") == std::string::npos);
    }

    // The only place a manager is *held* is Application's member, and everywhere
    // else it is a reference parameter or a member reference.
    const std::string applicationHeader = readEngineSource(ENGINE_APPLICATION_HEADER);
    CHECK(applicationHeader.find("const assets::AssetManager& assets() const noexcept") != std::string::npos);
}

void testApplicationHandsOutTheInterfaceNotTheConcreteManager()
{
    // Application owns the concrete manager but exposes the interface, by const
    // reference. That is what stops a caller reaching past the read-only
    // contract to reload or replace an asset and invalidating references the
    // render system is holding.
    static_assert(std::is_same_v<decltype(std::declval<const Application&>().assets()),
                                 const assets::AssetManager&>,
                  "Application must expose the interface by const reference");

    Application application;
    static_assert(std::is_abstract_v<assets::AssetManager>, "the exposed type must be the abstract interface");

    // The same manager on every call: one instance, owned by the Application,
    // rather than something rebuilt per access.
    CHECK(&application.assets() == &application.assets());

    // And it really is the manager holding the loaded resources: two configured
    // names resolve to two different objects, and each is stable across calls.
    // Comparing two runtime values, rather than asking whether a reference is
    // null, which is a question with only one possible answer.
    const assets::Texture& ground = application.assets().texture("mario_ground");
    const assets::Texture& stand = application.assets().texture("megaman_megaStand");
    CHECK(&ground != &stand);
    CHECK(&ground == &application.assets().texture("mario_ground"));
}

} // namespace

int main()
{
    const std::pair<const char*, void (*)()> testCases[] = {
        {"rectangle defaults", &testRectangleDefaults},
        {"rectangle aggregate construction", &testRectangleAggregateConstruction},
        {"rectangle is stored on entities", &testRectangleIsStoredOnEntities},
        {"render transform defaults", &testRenderTransformDefaults},
        {"position mapping", &testRenderTransformPositionMapping},
        {"scale mapping", &testRenderTransformScaleMapping},
        {"angle conversion", &testRenderTransformAngleConversion},
        {"angle is not flipped", &testRenderTransformAngleIsNotFlipped},
        {"maps everything at once", &testRenderTransformMapsEverythingAtOnce},
        {"render system draws matching entities", &testRenderSystemDrawsMatchingEntities},
        {"render system ignores entities missing components", &testRenderSystemIgnoresEntitiesMissingComponents},
        {"render system ignores dead entities", &testRenderSystemIgnoresDeadEntities},
        {"render system draws many entities", &testRenderSystemDrawsManyEntities},
        {"render system maps transform into draw calls", &testRenderSystemMapsTransformIntoDrawCalls},
        {"render system applies the camera to draw calls", &testRenderSystemAppliesTheCameraToDrawCalls},
        {"render system applies camera zoom to draw calls", &testRenderSystemAppliesCameraZoomToDrawCalls},
        {"render system does not modify the transform", &testRenderSystemDoesNotModifyTheTransform},
        {"render system runs after simulation systems", &testRenderSystemRunsAfterSimulationSystems},
        {"render system does not own entities", &testRenderSystemDoesNotOwnEntities},
        {"renderer frame protocol", &testRendererFrameProtocol},
        {"sfml renderer draws pixels", &testSfmlRendererDrawsPixels},
        {"sfml renderer honours position", &testSfmlRendererHonoursPosition},
        {"sfml renderer honours scale", &testSfmlRendererHonoursScale},
        {"sfml renderer honours rotation", &testSfmlRendererHonoursRotation},
        {"sfml renderer clamps colour channels", &testSfmlRendererClampsColourChannels},
        {"camera draws at viewport centre", &testCameraDrawsAtViewportCentre},
        {"camera moves objects on screen only", &testCameraMovesObjectsOnScreenOnly},
        {"zoom doubles apparent size", &testZoomDoublesApparentSize},
        {"zoom halves apparent size", &testZoomHalvesApparentSize},
        {"zoom is about the viewport centre", &testZoomIsAboutTheViewportCentre},
        {"camera preserves rotation and scale", &testCameraPreservesRotationAndScale},
        {"application renders renderable entities", &testApplicationRendersRenderableEntities},
        {"application survives no renderable entities", &testApplicationSurvivesNoRenderableEntities},
        {"texture component stores an asset name", &testTextureComponentStoresAnAssetName},
        {"texture component is plain data with no native state", &testTextureComponentIsPlainDataWithNoNativeState},
        {"renderer interface exposes drawTexture", &testRendererInterfaceExposesDrawTexture},
        {"a renderer without sfml still satisfies the interface", &testARendererWithoutSfmlStillSatisfiesTheInterface},
        {"render system texture query draws textures", &testRenderSystemTextureQueryDrawsTextures},
        {"render system requests the name the component carries", &testRenderSystemRequestsTheNameTheComponentCarries},
        {"render system submits the manager's own handle", &testRenderSystemSubmitsTheManagersOwnHandle},
        {"transform position moves the texture", &testTransformPositionMovesTheTexture},
        {"transform scale scales the texture", &testTransformScaleScalesTheTexture},
        {"camera zoom scales the texture without moving it", &testCameraZoomScalesTheTextureWithoutMovingIt},
        {"missing asset name propagates", &testMissingAssetNamePropagates},
        {"render system has no assets-unavailable state", &testRenderSystemHasNoAssetsUnavailableState},
        {"rectangle query is unaffected by textures", &testRectangleQueryIsUnaffectedByTextures},
        {"textures are drawn after rectangles", &testTexturesAreDrawnAfterRectangles},
        {"an entity with both components draws both rectangle first", &testAnEntityWithBothComponentsDrawsBothRectangleFirst},
        {"render system does not write to the texture transform", &testRenderSystemDoesNotWriteToTheTextureTransform},
        {"a real configured texture renders pixels", &testARealConfiguredTextureRendersPixels},
        {"texture position affects rendered pixels", &testTexturePositionAffectsRenderedPixels},
        {"texture scale affects rendered pixels", &testTextureScaleAffectsRenderedPixels},
        {"camera zoom affects rendered texture pixels", &testCameraZoomAffectsRenderedTexturePixels},
        {"rectangle rendering is unchanged alongside textures", &testRectangleRenderingIsUnchangedAlongsideTextures},
        {"a whole texture draw names no source region", &testAWholeTextureDrawNamesNoSourceRegion},
        {"a source region is recorded on the draw call", &testASourceRegionIsRecordedOnTheDrawCall},
        {"two frames of one sheet submit two distinct draw calls", &testTwoFramesOfOneSheetSubmitTwoDistinctDrawCalls},
        {"a source rect defaults to an empty rect", &testSourceRegionDefaultsToAnEmptyRect},
        {"frame zero draws the first frame's pixels", &testFrameZeroDrawsTheFirstFramesPixels},
        {"frame one draws the second frame's pixels", &testFrameOneDrawsTheSecondFramesPixels},
        {"two frames of one sheet look different on screen", &testTwoFramesOfOneSheetLookDifferentOnScreen},
        {"a source region is centred on the placement not the sheet", &testASourceRegionIsCentredOnThePlacementNotTheSheet},
        {"an empty source region draws nothing", &testAnEmptySourceRegionDrawsNothing},
        {"a whole texture draw is unchanged by the region parameter", &testAWholeTextureDrawIsUnchangedByTheRegionParameter},
        {"an explicit whole image region matches no region", &testAnExplicitWholeImageRegionMatchesNoRegion},
        {"a source region honours scale", &testASourceRegionHonoursScale},
        {"a source region does not change the camera mapping", &testASourceRegionDoesNotChangeTheCameraMapping},
        {"the renderer interface names no graphics type", &testTheRendererInterfaceNamesNoGraphicsType},
        {"a whole image is not overloaded to mean everything", &testAWholeImageIsNotOverloadedToMeanEverything},
        {"application constructs with the real configuration", &testApplicationConstructsWithTheRealConfiguration},
        {"asset loading does not depend on the working directory", &testAssetLoadingDoesNotDependOnTheWorkingDirectory},
        {"application owns a loaded asset manager", &testApplicationOwnsALoadedAssetManager},
        {"a textured entity renders through the application", &testATexturedEntityRendersThroughTheApplication},
        {"an undeclared texture name still fails loudly through the application", &testAnUndeclaredTextureNameStillFailsLoudlyThroughTheApplication},
        {"render system does not own the asset manager", &testRenderSystemDoesNotOwnTheAssetManager},
        {"application member order gives the manager the longer life", &testApplicationMemberOrderGivesTheManagerTheLongerLife},
        {"there is no global or static asset manager", &testThereIsNoGlobalOrStaticAssetManager},
        {"application hands out the interface not the concrete manager", &testApplicationHandsOutTheInterfaceNotTheConcreteManager},
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

    std::cout << groupCount << " Phase 6 test groups passed\n";
    return EXIT_SUCCESS;
}
