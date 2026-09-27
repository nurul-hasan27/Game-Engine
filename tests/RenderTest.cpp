#include "engine/Application.hpp"
#include "engine/Color.hpp"
#include "engine/components/Rectangle.hpp"
#include "engine/components/Transform.hpp"
#include "engine/ecs/EntityManager.hpp"
#include "engine/graphics/Camera.hpp"
#include "engine/graphics/RenderTransform.hpp"
#include "engine/graphics/Renderer.hpp"
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
#include <cstddef>
#include <cstdlib>
#include <iostream>
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

class RecordingRenderer final : public engine::graphics::Renderer
{
public:
    void beginFrame() override
    {
        ++m_beginFrames;
        m_cleared = false;
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
    }

    void endFrame() override { ++m_endFrames; }

    [[nodiscard]] std::uint64_t frameCount() const noexcept override { return m_endFrames; }

    [[nodiscard]] const std::vector<DrawCall>& draws() const noexcept { return m_draws; }
    [[nodiscard]] bool cleared() const noexcept { return m_cleared; }
    [[nodiscard]] const engine::Color& clearColor() const noexcept { return m_clearColor; }
    [[nodiscard]] std::size_t beginFrameCount() const noexcept { return m_beginFrames; }
    [[nodiscard]] std::size_t endFrameCount() const noexcept { return m_endFrames; }

    void reset()
    {
        m_draws.clear();
        m_cleared = false;
        m_beginFrames = 0;
        m_endFrames = 0;
    }

private:
    std::vector<DrawCall> m_draws;
    engine::Color m_clearColor{};
    std::size_t m_beginFrames = 0;
    std::size_t m_endFrames = 0;
    bool m_cleared = false;
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

using engine::Color;
using engine::Vec2;
using engine::components::Rectangle;
using engine::components::Transform;
using engine::ecs::Entity;
using engine::ecs::EntityManager;
using engine::graphics::Camera;
using engine::graphics::RenderTransform;
using engine::graphics::Renderer;
using engine::systems::RenderSystem;

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
    RenderSystem renderSystem{renderer, camera};

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
    RenderSystem renderSystem{renderer, camera};

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
    RenderSystem renderSystem{renderer, camera};

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
    RenderSystem renderSystem{renderer, camera};

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
    RenderSystem renderSystem{renderer, camera};

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
    RenderSystem renderSystem{renderer, camera};

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
    RenderSystem renderSystem{renderer, camera};

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
    RenderSystem renderSystem{renderer, camera};

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
    RenderSystem renderSystem{renderer, camera};

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
    RenderSystem renderSystem{renderer, camera};

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
    RenderSystem renderSystem{renderer, camera};

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
