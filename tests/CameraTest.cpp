#include "engine/components/Body.hpp"
#include "engine/components/Collider.hpp"
#include "engine/components/Rectangle.hpp"
#include "engine/components/Transform.hpp"
#include "engine/ecs/EntityManager.hpp"
#include "engine/ecs/SystemManager.hpp"
#include "engine/graphics/Camera.hpp"
#include "engine/graphics/RenderTransform.hpp"
#include "engine/input/Input.hpp"
#include "engine/math/Vec2.hpp"
#include "engine/input/ActionMap.hpp"
#include "engine/input/ActionState.hpp"
#include "engine/systems/CameraSystem.hpp"
#include "engine/systems/MovementSystem.hpp"
#include "engine/systems/PhysicsSystem.hpp"

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{

using engine::Color;
using engine::Vec2;
using engine::components::Body;
using engine::components::Collider;
using engine::components::Rectangle;
using engine::components::Transform;
using engine::ecs::Entity;
using engine::ecs::EntityManager;
using engine::ecs::SystemManager;
using engine::graphics::Camera;
using engine::graphics::RenderTransform;
using engine::input::ActionState;
using engine::input::Input;
using engine::input::defaultActionMap;
using engine::input::Key;
using engine::physics::BodyType;
using engine::systems::CameraSystem;
using engine::systems::MovementSystem;
using engine::systems::PhysicsSystem;

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

/// The viewport every camera test uses unless it is specifically testing the
/// viewport. 1280x720 matches the default window, and is deliberately
/// non-square so a swapped x and y cannot pass unnoticed.
constexpr Vec2 kViewport{1280.0F, 720.0F};

/// A camera at `position`, at zoom 1, looking at a 1280x720 viewport.
[[nodiscard]] Camera cameraAt(const Vec2& position)
{
    Camera camera;
    camera.setViewport(kViewport);
    camera.setPosition(position);
    return camera;
}

// ---------------------------------------------------------------------------
// Compile-time guarantees.
// ---------------------------------------------------------------------------

// The camera is plain state with no behaviour in it, and no SFML.
static_assert(std::is_trivially_copyable_v<Camera>, "Camera must be a plain value type");
static_assert(std::is_default_constructible_v<Camera>, "Camera must be default constructible");
static_assert(std::is_nothrow_copy_constructible_v<Camera>, "copying a Camera must not throw");

// The world to screen mapping is pure arithmetic, so it must be usable in a
// constant expression. That is what proves it does not touch a window, a clock
// or any global.
constexpr Camera kConstCamera{};

/// Compile-time check of the whole mapping at zoom 1 with no viewport, where
/// screen centre is the origin, so world and screen coincide.
static_assert(kConstCamera.zoom() == 1.0F, "default zoom must be 1");
static_assert(kConstCamera.position() == Vec2{0.0F, 0.0F}, "default position must be the origin");
static_assert(kConstCamera.viewport() == Vec2{0.0F, 0.0F}, "default viewport must be zero");
static_assert(kConstCamera.worldToScreen(Vec2{5.0F, -7.0F}) == Vec2{5.0F, -7.0F},
              "with no viewport and zoom 1 the camera must be the identity");
static_assert(kConstCamera.screenToWorld(Vec2{5.0F, -7.0F}) == Vec2{5.0F, -7.0F},
              "screenToWorld must invert worldToScreen");

// ---------------------------------------------------------------------------
// Defaults and the screen centre
// ---------------------------------------------------------------------------

void testCameraDefaults()
{
    const Camera camera;

    CHECK(camera.position() == Vec2(0.0F, 0.0F));
    CHECK_NEAR(camera.zoom(), 1.0F);
    CHECK(camera.viewport() == Vec2(0.0F, 0.0F));
}

void testScreenCenterIsViewportCentre()
{
    const Camera camera = cameraAt(Vec2{0.0F, 0.0F});

    CHECK_NEAR_VEC(camera.screenCenter(), Vec2(640.0F, 360.0F));

    // A non-square viewport, and a different non-square one, so a swapped x and
    // y in the centre calculation cannot pass.
    Camera wide;
    wide.setViewport(Vec2{1920.0F, 480.0F});
    CHECK_NEAR_VEC(wide.screenCenter(), Vec2(960.0F, 240.0F));

    Camera tall;
    tall.setViewport(Vec2{400.0F, 1000.0F});
    CHECK_NEAR_VEC(tall.screenCenter(), Vec2(200.0F, 500.0F));
}

void testZeroViewportIsUsable()
{
    // A zero viewport is the "no viewport configured yet" state, not an error.
    // The screen centre is then the origin, so the camera is a pure translation
    // by minus its own position and nothing else. Nothing divides by the
    // viewport, so no viewport value can produce an invalid result.
    Camera none;
    none.setPosition(Vec2{100.0F, 200.0F});

    CHECK_NEAR_VEC(none.screenCenter(), Vec2(0.0F, 0.0F));
    CHECK_NEAR_VEC(none.worldToScreen(Vec2{400.0F, 500.0F}), Vec2(300.0F, 300.0F));
    // And the round trip still holds with no viewport at all.
    CHECK_NEAR_VEC(none.screenToWorld(none.worldToScreen(Vec2{40.0F, -60.0F})), Vec2(40.0F, -60.0F));
}

// ---------------------------------------------------------------------------
// worldToScreen
// ---------------------------------------------------------------------------

void testCameraPositionMapsToScreenCentre()
{
    // The defining property of a centre-based camera: the world point the camera
    // sits on is drawn at the middle of the screen.
    const Camera camera = cameraAt(Vec2{1000.0F, 500.0F});

    CHECK_NEAR_VEC(camera.worldToScreen(Vec2{1000.0F, 500.0F}), Vec2(640.0F, 360.0F));
}

void testWorldToScreenWithCameraAtOrigin()
{
    const Camera camera = cameraAt(Vec2{0.0F, 0.0F});

    // Origin maps to the top-left of the screen, and everything else is just
    // offset by the screen centre.
    CHECK_NEAR_VEC(camera.worldToScreen(Vec2{0.0F, 0.0F}), Vec2(640.0F, 360.0F));
    CHECK_NEAR_VEC(camera.worldToScreen(Vec2{100.0F, 200.0F}), Vec2(740.0F, 560.0F));
}

void testWorldToScreenTranslatedCamera()
{
    const Camera camera = cameraAt(Vec2{-400.0F, -300.0F});

    // The probe at (100, -100) is 500 to the right of the camera at (-400, -300)
    // and 200 below it, and adding the screen centre of (640, 360) gives
    // (1140, 560).
    CHECK_NEAR_VEC(camera.worldToScreen(Vec2{100.0F, -100.0F}), Vec2(1140.0F, 560.0F));

    // A probe exactly on the camera lands on the screen centre, wherever in the
    // world that is.
    CHECK_NEAR_VEC(camera.worldToScreen(Vec2{-400.0F, -300.0F}), Vec2(640.0F, 360.0F));

    // Negative world coordinates are ordinary coordinates, not an error. This one
    // is 600 left and 700 above the camera, so it lands off the top of the screen,
    // which is correct: a camera does not clamp the world to the viewport.
    CHECK_NEAR_VEC(camera.worldToScreen(Vec2{-1000.0F, -1000.0F}), Vec2(40.0F, -340.0F));
}

void testWorldToScreenAllFourDirections()
{
    const Camera camera = cameraAt(Vec2{500.0F, 500.0F});

    // Right of the camera is right of the screen centre, and so on. The y axis
    // points down, so "above" means a smaller y.
    const Vec2 centre = camera.screenCenter();
    CHECK(camera.worldToScreen(Vec2{600.0F, 500.0F}).x > centre.x);
    CHECK(camera.worldToScreen(Vec2{400.0F, 500.0F}).x < centre.x);
    CHECK(camera.worldToScreen(Vec2{500.0F, 400.0F}).y < centre.y);
    CHECK(camera.worldToScreen(Vec2{500.0F, 600.0F}).y > centre.y);
}

// ---------------------------------------------------------------------------
// Zoom
// ---------------------------------------------------------------------------

void testZoomScalesScreenOffset()
{
    // A point 100 to the right of the camera is 100, 200 or 50 pixels right of
    // the screen centre at zoom 1, 2 or 0.5.
    Camera camera = cameraAt(Vec2{0.0F, 0.0F});
    const Vec2 probe{100.0F, 0.0F};
    const Vec2 centre = camera.screenCenter();

    camera.setZoom(1.0F);
    CHECK_NEAR(camera.worldToScreen(probe).x - centre.x, 100.0F);

    camera.setZoom(2.0F);
    CHECK_NEAR(camera.worldToScreen(probe).x - centre.x, 200.0F);

    camera.setZoom(0.5F);
    CHECK_NEAR(camera.worldToScreen(probe).x - centre.x, 50.0F);
}

void testZoomScalesScreenOffsetOnY()
{
    Camera camera = cameraAt(Vec2{0.0F, 0.0F});
    const Vec2 probe{0.0F, 100.0F};
    const Vec2 centre = camera.screenCenter();

    camera.setZoom(1.0F);
    CHECK_NEAR(camera.worldToScreen(probe).y - centre.y, 100.0F);

    camera.setZoom(2.0F);
    CHECK_NEAR(camera.worldToScreen(probe).y - centre.y, 200.0F);

    camera.setZoom(0.5F);
    CHECK_NEAR(camera.worldToScreen(probe).y - centre.y, 50.0F);
}

void testZoomAlsoScalesTheCameraTranslation()
{
    // Zoom is about the screen centre, so a point that is 100 from the camera
    // is 100 * zoom from the screen centre, whichever side it is on.
    Camera camera = cameraAt(Vec2{1000.0F, 1000.0F});
    const Vec2 centre = camera.screenCenter();

    camera.setZoom(1.0F);
    CHECK_NEAR(camera.worldToScreen(Vec2{900.0F, 1000.0F}).x - centre.x, -100.0F);
    CHECK_NEAR(camera.worldToScreen(Vec2{1100.0F, 1000.0F}).x - centre.x, 100.0F);

    camera.setZoom(3.0F);
    CHECK_NEAR(camera.worldToScreen(Vec2{900.0F, 1000.0F}).x - centre.x, -300.0F);
    CHECK_NEAR(camera.worldToScreen(Vec2{1100.0F, 1000.0F}).x - centre.x, 300.0F);
}

void testInvalidZoomIsClamped()
{
    // Zero and negative zoom have no meaningful inverse, so the setter clamps
    // rather than letting screenToWorld divide by zero.
    Camera camera = cameraAt(Vec2{0.0F, 0.0F});

    camera.setZoom(0.0F);
    CHECK(camera.zoom() > 0.0F);
    CHECK_NEAR(camera.zoom(), engine::graphics::kMinimumZoom);

    camera.setZoom(-4.0F);
    CHECK(camera.zoom() > 0.0F);
    CHECK_NEAR(camera.zoom(), engine::graphics::kMinimumZoom);

    // A positive value below the floor is clamped too, and the clamp is a floor
    // rather than a rejection: the value the caller asked for is not kept.
    camera.setZoom(engine::graphics::kMinimumZoom * 0.5F);
    CHECK_NEAR(camera.zoom(), engine::graphics::kMinimumZoom);

    // A valid value above the floor is kept exactly.
    camera.setZoom(2.5F);
    CHECK_NEAR(camera.zoom(), 2.5F);
}

void testScreenToWorldIsAlwaysFinite()
{
    // Even at the clamped minimum zoom, the inverse must produce real numbers
    // rather than infinity or NaN.
    Camera camera = cameraAt(Vec2{0.0F, 0.0F});
    camera.setZoom(0.0F);

    const Vec2 world = camera.screenToWorld(Vec2{640.0F, 360.0F});

    CHECK(std::isfinite(world.x));
    CHECK(std::isfinite(world.y));
    // The screen centre always maps back to the camera position, whatever the
    // zoom, because it is the only point with zero offset.
    CHECK_NEAR_VEC(world, Vec2(0.0F, 0.0F));
}

// ---------------------------------------------------------------------------
// screenToWorld and the round trip
// ---------------------------------------------------------------------------

void testScreenToWorldCentre()
{
    Camera camera = cameraAt(Vec2{250.0F, -125.0F});

    // The screen centre is the camera's own world position, whatever the zoom.
    CHECK_NEAR_VEC(camera.screenToWorld(camera.screenCenter()), Vec2(250.0F, -125.0F));

    camera.setZoom(4.0F);
    CHECK_NEAR_VEC(camera.screenToWorld(camera.screenCenter()), Vec2(250.0F, -125.0F));
}

void testScreenToWorldUndoesTranslationAndZoom()
{
    Camera camera = cameraAt(Vec2{-320.0F, 640.0F});
    camera.setZoom(2.0F);

    // 200 screen pixels right of centre is 100 world pixels right of the camera.
    CHECK_NEAR_VEC(camera.screenToWorld(camera.screenCenter() + Vec2{200.0F, -400.0F}),
                   Vec2(-220.0F, 440.0F));
}

void testRoundTripRecoversWorld()
{
    Camera camera = cameraAt(Vec2{137.0F, -911.0F});
    camera.setViewport(kViewport);

    const Vec2 probes[] = {
        Vec2{0.0F, 0.0F},
        Vec2{1.0F, 1.0F},
        Vec2{-1.0F, -1.0F},
        Vec2{1000.0F, 500.0F},
        Vec2{-4000.0F, 9000.0F},
        Vec2{3200.0F, 1800.0F},
        Vec2{137.0F, -911.0F}, // exactly the camera position
        Vec2{0.001F, 12345.678F},
    };

    for (const float zoom : {1.0F, 2.0F, 0.5F, 0.25F, 3.75F})
    {
        camera.setZoom(zoom);

        for (const Vec2& probe : probes)
        {
            const Vec2 recovered = camera.screenToWorld(camera.worldToScreen(probe));
            CHECK_NEAR_VEC(recovered, probe);
        }
    }
}

void testRoundTripWithNonSquareViewport()
{
    // A round trip that only holds for a square viewport would hide a swapped
    // axis somewhere, so this uses three different aspect ratios.
    const Vec2 viewports[] = {Vec2{1280.0F, 720.0F}, Vec2{800.0F, 1200.0F}, Vec2{1920.0F, 108.0F}};

    for (const Vec2& viewport : viewports)
    {
        Camera camera = cameraAt(Vec2{64.0F, -32.0F});
        camera.setViewport(viewport);
        camera.setZoom(1.75F);

        const Vec2 probe{700.0F, 250.0F};
        CHECK_NEAR_VEC(camera.screenToWorld(camera.worldToScreen(probe)), probe);

        // And the screen centre really is this viewport's centre.
        CHECK_NEAR_VEC(camera.screenCenter(), viewport * 0.5F);
    }
}

// ---------------------------------------------------------------------------
// toRenderTransform: world in, screen out
// ---------------------------------------------------------------------------

void testRenderTransformAppliesCamera()
{
    Camera camera = cameraAt(Vec2{1000.0F, 500.0F});

    Transform transform;
    transform.position = Vec2{1000.0F, 500.0F};

    const RenderTransform placement = engine::graphics::toRenderTransform(transform, camera);

    // The world point the camera sits on is drawn at the middle of the screen.
    CHECK_NEAR_VEC(placement.position, Vec2(640.0F, 360.0F));
}

void testRenderTransformLeavesWorldPositionAlone()
{
    // The camera must only change where something is *drawn*. The Transform it
    // was given is passed by const reference and must be untouched.
    Camera camera = cameraAt(Vec2{1000.0F, 500.0F});

    Transform transform;
    transform.position = Vec2{1234.0F, 567.0F};
    const Transform& before = transform;

    static_cast<void>(engine::graphics::toRenderTransform(transform, camera));

    CHECK(transform.position == before.position);
    CHECK(transform.velocity == before.velocity);
    CHECK(transform.scale == before.scale);
    CHECK_NEAR(transform.angle, before.angle);
}

void testRenderTransformCarriesZoomInScale()
{
    Camera camera = cameraAt(Vec2{0.0F, 0.0F});

    Transform transform;
    transform.scale = Vec2{2.0F, 3.0F};

    camera.setZoom(1.0F);
    CHECK_NEAR_VEC(engine::graphics::toRenderTransform(transform, camera).scale, Vec2(2.0F, 3.0F));

    camera.setZoom(2.0F);
    CHECK_NEAR_VEC(engine::graphics::toRenderTransform(transform, camera).scale, Vec2(4.0F, 6.0F));

    camera.setZoom(0.5F);
    CHECK_NEAR_VEC(engine::graphics::toRenderTransform(transform, camera).scale, Vec2(1.0F, 1.5F));
}

void testRenderTransformZoomDoesNotTouchTheTransform()
{
    // Zoom is a rendering operation. It must not write back into the Transform,
    // which is where scale lives.
    Camera camera = cameraAt(Vec2{0.0F, 0.0F});
    camera.setZoom(4.0F);

    Transform transform;
    transform.scale = Vec2{1.0F, 1.0F};

    static_cast<void>(engine::graphics::toRenderTransform(transform, camera));

    CHECK_NEAR_VEC(transform.scale, Vec2(1.0F, 1.0F));
}

void testRenderTransformDoesNotRotateWithTheCamera()
{
    // The camera has no rotation at all, so the angle conversion is unchanged.
    Camera camera = cameraAt(Vec2{1000.0F, 500.0F});
    camera.setZoom(2.0F);

    Transform transform;
    transform.angle = 1.5707963267948966F; // pi/2

    const RenderTransform placement = engine::graphics::toRenderTransform(transform, camera);

    CHECK_NEAR(placement.rotationDegrees, 90.0F);
    // Zoom makes the object bigger, it does not turn it.
    CHECK_NEAR_VEC(placement.scale, Vec2(2.0F, 2.0F));
}

// ---------------------------------------------------------------------------
// Camera follow
// ---------------------------------------------------------------------------

Entity& addTarget(EntityManager& manager, const Vec2& position, const std::string_view tag = "player")
{
    Entity& entity = manager.addEntity(std::string{tag});
    entity.addComponent<Transform>(Transform{position, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
    return entity;
}

void testCameraFollowsTargetPosition()
{
    EntityManager world;
    Camera camera = cameraAt(Vec2{0.0F, 0.0F});
    CameraSystem follow{camera, "player"};

    addTarget(world, Vec2{1500.0F, 900.0F});

    Input input;
    ActionState actions;
    actions.update(defaultActionMap(), input);
    follow.update(world, actions, 1.0F / 60.0F);

    CHECK_NEAR_VEC(camera.position(), Vec2(1500.0F, 900.0F));
}

void testCameraFollowsMovement()
{
    // The full chain: input -> movement -> physics -> camera. The camera should
    // end up exactly where the player ended up.
    EntityManager world;
    Input input;
    ActionState actions;
    SystemManager systems;
    Camera camera = cameraAt(Vec2{0.0F, 0.0F});

    systems.add<MovementSystem>(300.0F);
    systems.add<PhysicsSystem>();
    systems.add<CameraSystem>(camera, "player");

    Entity& player = world.addEntity("player");
    player.addComponent<Transform>(Transform{Vec2{100.0F, 100.0F}, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
    player.addComponent<Body>(Body{BodyType::Dynamic});

    input.processKeyDown(Key::D);
    for (int frame = 0; frame < 120; ++frame)
    {
        actions.update(defaultActionMap(), input);
    systems.update(world, actions, 1.0F / 60.0F);
    }

    // Two seconds at 300 px/s.
    CHECK_NEAR(player.getComponent<Transform>().position.x, 700.0F);
    CHECK_NEAR_VEC(camera.position(), player.getComponent<Transform>().position);
}

void testCameraDoesNotModifyTheTarget()
{
    // The one rule this whole phase exists to protect.
    EntityManager world;
    Camera camera = cameraAt(Vec2{0.0F, 0.0F});
    CameraSystem follow{camera, "player"};

    Entity& target = addTarget(world, Vec2{640.0F, 480.0F});
    const Transform before = target.getComponent<Transform>();

    Input input;
    ActionState actions;
    actions.update(defaultActionMap(), input);
    follow.update(world, actions, 1.0F / 60.0F);

    const Transform& after = target.getComponent<Transform>();
    CHECK(after.position == before.position);
    CHECK(after.velocity == before.velocity);
    CHECK(after.scale == before.scale);
    CHECK_NEAR(after.angle, before.angle);
    // And the camera did move, so the test is not passing vacuously.
    CHECK_NEAR_VEC(camera.position(), Vec2(640.0F, 480.0F));
}

void testCameraWithMissingTargetKeepsItsPosition()
{
    // No entity carries the tag. The camera must hold still, not snap to the
    // origin, and must not crash.
    EntityManager world;
    Camera camera = cameraAt(Vec2{321.0F, 654.0F});
    CameraSystem follow{camera, "player"};

    Input input;
    ActionState actions;
    actions.update(defaultActionMap(), input);
    follow.update(world, actions, 1.0F / 60.0F);

    CHECK_NEAR_VEC(camera.position(), Vec2(321.0F, 654.0F));
}

void testCameraWithDeadTargetKeepsItsPosition()
{
    // The target existed last frame and no longer does. destroyEntity() flags it
    // dead immediately, so getEntities() stops yielding it even before
    // EntityManager::update() erases it.
    EntityManager world;
    Camera camera = cameraAt(Vec2{100.0F, 200.0F});
    CameraSystem follow{camera, "player"};

    Entity& target = addTarget(world, Vec2{500.0F, 500.0F});

    Input input;
    ActionState actions;
    actions.update(defaultActionMap(), input);
    follow.update(world, actions, 1.0F / 60.0F);
    CHECK_NEAR_VEC(camera.position(), Vec2(500.0F, 500.0F));

    world.destroyEntity(target);
    actions.update(defaultActionMap(), input);
    follow.update(world, actions, 1.0F / 60.0F);

    CHECK_NEAR_VEC(camera.position(), Vec2(500.0F, 500.0F));

    // And after the deferred cleanup actually runs, still no crash.
    world.update();
    actions.update(defaultActionMap(), input);
    follow.update(world, actions, 1.0F / 60.0F);
    CHECK_NEAR_VEC(camera.position(), Vec2(500.0F, 500.0F));
}

void testCameraIgnoresTargetWithoutTransform()
{
    // The tag matches an entity that is not something that can be followed.
    EntityManager world;
    Camera camera = cameraAt(Vec2{7.0F, 9.0F});
    CameraSystem follow{camera, "player"};

    world.addEntity("player"); // no Transform

    Input input;
    ActionState actions;
    actions.update(defaultActionMap(), input);
    follow.update(world, actions, 1.0F / 60.0F);

    CHECK_NEAR_VEC(camera.position(), Vec2(7.0F, 9.0F));
}

void testCameraFollowsFirstOfDuplicateTags()
{
    // A duplicate tag must not make the camera fight itself between two
    // entities; the first in creation order wins, deterministically.
    EntityManager world;
    Camera camera = cameraAt(Vec2{0.0F, 0.0F});
    CameraSystem follow{camera, "player"};

    addTarget(world, Vec2{10.0F, 20.0F});
    addTarget(world, Vec2{30.0F, 40.0F});

    Input input;
    ActionState actions;
    actions.update(defaultActionMap(), input);
    follow.update(world, actions, 1.0F / 60.0F);

    CHECK_NEAR_VEC(camera.position(), Vec2(10.0F, 20.0F));
}

void testCameraSurvivesTargetBeingRespawned()
{
    // The reason the target is resolved by tag every frame rather than cached:
    // an Entity& is invalidated by EntityManager::update(), so a system holding
    // one across a destroy and respawn would be reading freed memory.
    EntityManager world;
    Camera camera = cameraAt(Vec2{0.0F, 0.0F});
    CameraSystem follow{camera, "player"};

    Entity& first = addTarget(world, Vec2{50.0F, 60.0F});
    Input input;
    ActionState actions;

    actions.update(defaultActionMap(), input);
    follow.update(world, actions, 1.0F / 60.0F);
    CHECK_NEAR_VEC(camera.position(), Vec2(50.0F, 60.0F));

    world.destroyEntity(first);
    world.update();
    addTarget(world, Vec2{700.0F, 800.0F});

    actions.update(defaultActionMap(), input);
    follow.update(world, actions, 1.0F / 60.0F);
    CHECK_NEAR_VEC(camera.position(), Vec2(700.0F, 800.0F));
}

void testCameraSystemIgnoresInputAndDelta()
{
    // Following a target needs neither the keyboard nor a duration. Running with
    // no input and a zero delta must still place the camera on the target, and
    // running it twice in a frame must be harmless.
    EntityManager world;
    Camera camera = cameraAt(Vec2{0.0F, 0.0F});
    CameraSystem follow{camera, "player"};

    addTarget(world, Vec2{111.0F, 222.0F});

    Input input;
    ActionState actions;
    actions.update(defaultActionMap(), input);
    follow.update(world, actions, 0.0F);
    CHECK_NEAR_VEC(camera.position(), Vec2(111.0F, 222.0F));

    actions.update(defaultActionMap(), input);
    follow.update(world, actions, 1000.0F);
    CHECK_NEAR_VEC(camera.position(), Vec2(111.0F, 222.0F));
}

// ---------------------------------------------------------------------------
// System ordering
// ---------------------------------------------------------------------------

void testCameraRunsAfterPhysics()
{
    // If the camera ran before physics it would follow where the player was at
    // the start of the frame, lagging by exactly one frame. Registering it after
    // is what makes it follow this frame's position.
    EntityManager world;
    Input input;
    ActionState actions;
    SystemManager systems;
    Camera camera = cameraAt(Vec2{0.0F, 0.0F});

    systems.add<MovementSystem>(100.0F);
    systems.add<PhysicsSystem>();
    systems.add<CameraSystem>(camera, "player");

    Entity& player = world.addEntity("player");
    player.addComponent<Transform>(Transform{Vec2{0.0F, 0.0F}, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
    player.addComponent<Body>(Body{BodyType::Dynamic});

    input.processKeyDown(Key::D);
    actions.update(defaultActionMap(), input);
    systems.update(world, actions, 1.0F);

    // One second at 100 px/s. The camera must see the moved position, not zero.
    CHECK_NEAR(camera.position().x, 100.0F);
    CHECK_NEAR_VEC(camera.position(), player.getComponent<Transform>().position);
}

void testCameraRegisteredFirstWouldLagOneFrame()
{
    // The counter-example, written out so the ordering requirement is pinned
    // rather than merely asserted in a comment. Registering the camera first
    // makes it follow the pre-physics position.
    //
    // Each arrangement gets its own world, because they are separate runs of the
    // simulation and sharing one world would give both systems two players to
    // move and quietly invalidate the comparison.
    const auto runTwoFrames = [](const bool cameraFirst) {
        EntityManager world;
        Input input;
    ActionState actions;
        SystemManager systems;
        Camera camera = cameraAt(Vec2{0.0F, 0.0F});

        if (cameraFirst)
        {
            systems.add<CameraSystem>(camera, "player"); // wrong: before physics
            systems.add<MovementSystem>(100.0F);
            systems.add<PhysicsSystem>();
        }
        else
        {
            systems.add<MovementSystem>(100.0F);
            systems.add<PhysicsSystem>();
            systems.add<CameraSystem>(camera, "player");
        }

        Entity& player = world.addEntity("player");
        player.addComponent<Transform>(Transform{Vec2{0.0F, 0.0F}, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
        player.addComponent<Body>(Body{BodyType::Dynamic});

        // Frame 1: movement sets velocity, physics moves the player to 100.
        input.processKeyDown(Key::D);
        actions.update(defaultActionMap(), input);
    systems.update(world, actions, 1.0F);

        // Frame 2: physics moves the player to 200.
        input.beginFrame();
        input.processKeyDown(Key::D);
        actions.update(defaultActionMap(), input);
    systems.update(world, actions, 1.0F);

        return std::pair<float, float>{camera.position().x, player.getComponent<Transform>().position.x};
    };

    const auto lagging = runTwoFrames(true);
    const auto correct = runTwoFrames(false);

    // The player really did end up at 200 in both runs, so the cameras are
    // being compared against the same simulation rather than two different ones.
    CHECK_NEAR(lagging.second, 200.0F);
    CHECK_NEAR(correct.second, 200.0F);

    // The correct order reports the position the player has now.
    CHECK_NEAR(correct.first, 200.0F);
    // The wrong order reports the position it had before this frame's physics,
    // which is exactly one frame of lag.
    CHECK_NEAR(lagging.first, 100.0F);
}

// ---------------------------------------------------------------------------
// Physics and input independence
// ---------------------------------------------------------------------------

void testCollisionIsIdenticalAtAnyCameraPosition()
{
    // The regression this phase most needs to guard: a collision must come out
    // the same whatever the camera is looking at, because physics never sees the
    // camera.
    //
    // Player at (1000, 500) driving right into a wall at (1100, 500), run twice
    // with wildly different cameras.
    const auto run = [](const Vec2& cameraPosition) {
        EntityManager world;
        Input input;
    ActionState actions;
        SystemManager systems;
        Camera camera = cameraAt(cameraPosition);

        systems.add<PhysicsSystem>();
        systems.add<CameraSystem>(camera, "player");

        Entity& wall = world.addEntity("wall");
        wall.addComponent<Transform>(Transform{{1100.0F, 500.0F}, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
        wall.addComponent<Collider>(Collider{Vec2{50.0F, 200.0F}});
        wall.addComponent<Body>(Body{BodyType::Static});

        Entity& player = world.addEntity("player");
        player.addComponent<Transform>(Transform{{1000.0F, 500.0F}, {300.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
        player.addComponent<Collider>(Collider{Vec2{50.0F, 50.0F}});
        player.addComponent<Body>(Body{BodyType::Dynamic});

        for (int frame = 0; frame < 60; ++frame)
        {
            actions.update(defaultActionMap(), input);
    systems.update(world, actions, 1.0F / 60.0F);
        }

        const Transform& result = player.getComponent<Transform>();
        return std::pair<Vec2, Vec2>{result.position, result.velocity};
    };

    const auto atOrigin = run(Vec2{0.0F, 0.0F});
    const auto onTarget = run(Vec2{1000.0F, 500.0F});
    const auto farAway = run(Vec2{-9999.0F, 4321.0F});

    // Identical to the last bit, not merely close.
    CHECK_NEAR_VEC(atOrigin.first, onTarget.first);
    CHECK_NEAR_VEC(atOrigin.first, farAway.first);
    CHECK_NEAR_VEC(atOrigin.second, onTarget.second);
    CHECK_NEAR_VEC(atOrigin.second, farAway.second);

    // And the answer is the one Phase 8 already established: the player's right
    // face against the wall's left face. The wall spans 1075..1125, so the
    // player's centre rests at 1075 - 25.
    CHECK_NEAR(atOrigin.first.x, 1050.0F);
    CHECK_NEAR(atOrigin.second.x, 0.0F);
}

void testColliderIsUnaffectedByZoom()
{
    // Zoom is a rendering operation, so a body that is 50 wide is 50 wide and
    // collides the same way whatever the zoom. Changing the zoom between two
    // identical runs must produce an identical resting position.
    const auto run = [](const float zoom) {
        EntityManager world;
        Input input;
    ActionState actions;
        SystemManager systems;
        Camera camera = cameraAt(Vec2{0.0F, 0.0F});
        camera.setZoom(zoom);

        systems.add<PhysicsSystem>();
        static_cast<void>(camera); // physics is not given the camera at all

        Entity& wall = world.addEntity("wall");
        wall.addComponent<Transform>(Transform{{300.0F, 0.0F}, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
        wall.addComponent<Collider>(Collider{Vec2{40.0F, 400.0F}});
        wall.addComponent<Body>(Body{BodyType::Static});

        Entity& player = world.addEntity("player");
        player.addComponent<Transform>(Transform{{0.0F, 0.0F}, {200.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
        player.addComponent<Collider>(Collider{Vec2{50.0F, 50.0F}});
        player.addComponent<Body>(Body{BodyType::Dynamic});

        // Two seconds at 200 px/s is 400 pixels, comfortably past the wall, so
        // the player must end up resting against it rather than short of it.
        for (int frame = 0; frame < 120; ++frame)
        {
            actions.update(defaultActionMap(), input);
    systems.update(world, actions, 1.0F / 60.0F);
        }
        return player.getComponent<Transform>().position;
    };

    // The wall spans 280..320, so the player's centre rests at 280 - 25.
    CHECK_NEAR(run(1.0F).x, 255.0F);
    CHECK_NEAR(run(2.0F).x, 255.0F);
    CHECK_NEAR(run(0.5F).x, 255.0F);
}

void testMovementDirectionIsUnaffectedByCamera()
{
    // W is world up, D is world right, whatever the camera is doing. The camera
    // must not rotate or skew the input direction.
    const auto run = [](const Vec2& cameraPosition, const float zoom) {
        EntityManager world;
        Input input;
    ActionState actions;
        SystemManager systems;
        Camera camera = cameraAt(cameraPosition);
        camera.setZoom(zoom);

        systems.add<MovementSystem>(100.0F);
        systems.add<PhysicsSystem>();
        systems.add<CameraSystem>(camera, "player");

        Entity& player = world.addEntity("player");
        player.addComponent<Transform>(Transform{{0.0F, 0.0F}, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
        player.addComponent<Body>(Body{BodyType::Dynamic});

        input.processKeyDown(Key::D);
        actions.update(defaultActionMap(), input);
    systems.update(world, actions, 1.0F);
        return player.getComponent<Transform>().position;
    };

    const Vec2 origin = run(Vec2{0.0F, 0.0F}, 1.0F);
    const Vec2 elsewhere = run(Vec2{5000.0F, -3000.0F}, 3.0F);

    CHECK_NEAR_VEC(origin, Vec2(100.0F, 0.0F));
    CHECK_NEAR_VEC(elsewhere, Vec2(100.0F, 0.0F));
}

} // namespace

int main()
{
    const std::pair<const char*, void (*)()> testCases[] = {
        {"camera defaults", &testCameraDefaults},
        {"screen center is viewport centre", &testScreenCenterIsViewportCentre},
        {"zero viewport is usable", &testZeroViewportIsUsable},
        {"camera position maps to screen centre", &testCameraPositionMapsToScreenCentre},
        {"world to screen with camera at origin", &testWorldToScreenWithCameraAtOrigin},
        {"world to screen translated camera", &testWorldToScreenTranslatedCamera},
        {"world to screen all four directions", &testWorldToScreenAllFourDirections},
        {"zoom scales screen offset", &testZoomScalesScreenOffset},
        {"zoom scales screen offset on y", &testZoomScalesScreenOffsetOnY},
        {"zoom also scales the camera translation", &testZoomAlsoScalesTheCameraTranslation},
        {"invalid zoom is clamped", &testInvalidZoomIsClamped},
        {"screen to world is always finite", &testScreenToWorldIsAlwaysFinite},
        {"screen to world centre", &testScreenToWorldCentre},
        {"screen to world undoes translation and zoom", &testScreenToWorldUndoesTranslationAndZoom},
        {"round trip recovers world", &testRoundTripRecoversWorld},
        {"round trip with non-square viewport", &testRoundTripWithNonSquareViewport},
        {"render transform applies camera", &testRenderTransformAppliesCamera},
        {"render transform leaves world position alone", &testRenderTransformLeavesWorldPositionAlone},
        {"render transform carries zoom in scale", &testRenderTransformCarriesZoomInScale},
        {"render transform zoom does not touch the transform", &testRenderTransformZoomDoesNotTouchTheTransform},
        {"render transform does not rotate with the camera", &testRenderTransformDoesNotRotateWithTheCamera},
        {"camera follows target position", &testCameraFollowsTargetPosition},
        {"camera follows movement", &testCameraFollowsMovement},
        {"camera does not modify the target", &testCameraDoesNotModifyTheTarget},
        {"camera with missing target keeps its position", &testCameraWithMissingTargetKeepsItsPosition},
        {"camera with dead target keeps its position", &testCameraWithDeadTargetKeepsItsPosition},
        {"camera ignores target without transform", &testCameraIgnoresTargetWithoutTransform},
        {"camera follows first of duplicate tags", &testCameraFollowsFirstOfDuplicateTags},
        {"camera survives target being respawned", &testCameraSurvivesTargetBeingRespawned},
        {"camera system ignores input and delta", &testCameraSystemIgnoresInputAndDelta},
        {"camera runs after physics", &testCameraRunsAfterPhysics},
        {"camera registered first would lag one frame", &testCameraRegisteredFirstWouldLagOneFrame},
        {"collision is identical at any camera position", &testCollisionIsIdenticalAtAnyCameraPosition},
        {"collider is unaffected by zoom", &testColliderIsUnaffectedByZoom},
        {"movement direction is unaffected by camera", &testMovementDirectionIsUnaffectedByCamera},
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

    std::cout << groupCount << " Phase 9 test groups passed\n";
    return EXIT_SUCCESS;
}
