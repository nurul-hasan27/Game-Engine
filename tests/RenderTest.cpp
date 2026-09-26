#include "engine/Application.hpp"
#include "engine/Color.hpp"
#include "engine/components/Rectangle.hpp"
#include "engine/components/Transform.hpp"
#include "engine/ecs/EntityManager.hpp"
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
using engine::graphics::RenderTransform;
using engine::graphics::Renderer;
using engine::systems::RenderSystem;

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

    const RenderTransform placement = engine::graphics::toRenderTransform(transform);

    // Position passes through untouched, in pixels, no conversion or flipping.
    CHECK(placement.position == Vec2(320.0F, 180.0F));
}

void testRenderTransformScaleMapping()
{
    Transform transform;
    transform.scale = Vec2(2.0F, 0.5F);

    const RenderTransform placement = engine::graphics::toRenderTransform(transform);

    // Scale passes through per axis, unswapped and unswizzled.
    CHECK(placement.scale == Vec2(2.0F, 0.5F));
}

void testRenderTransformAngleConversion()
{
    Transform transform;

    transform.angle = 0.0F;
    CHECK_NEAR(engine::graphics::toRenderTransform(transform).rotationDegrees, 0.0F);

    transform.angle = kPi / 2.0F;
    CHECK_NEAR(engine::graphics::toRenderTransform(transform).rotationDegrees, 90.0F);

    transform.angle = kPi;
    CHECK_NEAR(engine::graphics::toRenderTransform(transform).rotationDegrees, 180.0F);

    transform.angle = -kPi / 2.0F;
    CHECK_NEAR(engine::graphics::toRenderTransform(transform).rotationDegrees, -90.0F);

    transform.angle = kPi / 4.0F;
    CHECK_NEAR(engine::graphics::toRenderTransform(transform).rotationDegrees, 45.0F);
}

void testRenderTransformAngleIsNotFlipped()
{
    // Engine and screen space share the same axes in this phase, so the
    // conversion is a plain unit change. A negative here would be a double flip
    // and would rotate the wrong way on screen.
    Transform transform;
    transform.angle = kPi / 2.0F;

    const float degrees = engine::graphics::toRenderTransform(transform).rotationDegrees;

    CHECK(degrees > 0.0F);
    CHECK_NEAR(degrees, 90.0F);
    CHECK_NEAR(engine::graphics::kDegreesPerRadian, 57.295779513082320876798154814105F);
}

void testRenderTransformMapsEverythingAtOnce()
{
    Transform transform{Vec2{10.0F, 20.0F}, Vec2{0.0F, 0.0F}, Vec2{3.0F, 4.0F}, kPi};

    const RenderTransform placement = engine::graphics::toRenderTransform(transform);

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
    RenderSystem renderSystem{renderer};

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
    RenderSystem renderSystem{renderer};

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
    RenderSystem renderSystem{renderer};

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
    RenderSystem renderSystem{renderer};

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
    RenderSystem renderSystem{renderer};

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

void testRenderSystemRunsAfterSimulationSystems()
{
    EntityManager manager;
    [[maybe_unused]] engine::input::Input input;
    RecordingRenderer renderer;

    engine::ecs::SystemManager systems;
    systems.add<DriftSystem>();
    RenderSystem renderSystem{renderer};

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
    RenderSystem renderSystem{renderer};

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
    RenderSystem renderSystem{renderer};

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
        {"render system runs after simulation systems", &testRenderSystemRunsAfterSimulationSystems},
        {"render system does not own entities", &testRenderSystemDoesNotOwnEntities},
        {"renderer frame protocol", &testRendererFrameProtocol},
        {"sfml renderer draws pixels", &testSfmlRendererDrawsPixels},
        {"sfml renderer honours position", &testSfmlRendererHonoursPosition},
        {"sfml renderer honours scale", &testSfmlRendererHonoursScale},
        {"sfml renderer honours rotation", &testSfmlRendererHonoursRotation},
        {"sfml renderer clamps colour channels", &testSfmlRendererClampsColourChannels},
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
