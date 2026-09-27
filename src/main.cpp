#include "engine/Application.hpp"
#include "engine/components/Body.hpp"
#include "engine/components/Collider.hpp"
#include "engine/components/Rectangle.hpp"
#include "engine/components/Transform.hpp"
#include "engine/graphics/Camera.hpp"
#include "engine/systems/CameraSystem.hpp"
#include "engine/systems/MovementSystem.hpp"
#include "engine/systems/PhysicsSystem.hpp"

#include <charconv>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace
{

using engine::Vec2;
using engine::components::Body;
using engine::components::Collider;
using engine::components::Rectangle;
using engine::components::Transform;
using engine::ecs::Entity;
using engine::ecs::EntityManager;
using engine::ecs::SystemManager;
using engine::graphics::Camera;
using engine::input::Input;
using engine::input::Key;
using engine::physics::BodyType;
using engine::systems::CameraSystem;
using engine::systems::MovementSystem;
using engine::systems::PhysicsSystem;

/// The player's top speed, in pixels per second.
constexpr float kPlayerSpeed = 300.0F;

/// The size of the world the demo builds, in pixels.
///
/// Deliberately much larger than the 1280x720 viewport, because a world that
/// fits on screen cannot demonstrate a camera: there would be nothing to scroll.
constexpr Vec2 kWorldSize{3200.0F, 1800.0F};

/// The tag `CameraSystem` looks for. A tag rather than a `Player` class, because
/// a player here is just an entity that happens to carry this tag.
constexpr std::string_view kPlayerTag = "player";

/// How much one press of the zoom keys changes the zoom, as a factor.
constexpr float kZoomStep = 1.25F;

/// A static wall: it is drawn, and it is collided with, and it never moves.
///
/// A wall needs a `Transform` (where it is), a `Rectangle` (how it looks), a
/// `Collider` (how big it is to physics) and a `Body` (that it is static). Those
/// four are all separate on purpose. The wall happens to use the same size for
/// its collider and its rectangle, but nothing requires it to, and this takes
/// them as one argument only because a plain wall has no reason to differ.
void addStaticWall(EntityManager& world, const Vec2& center, const Vec2& size, const engine::Color& color)
{
    Entity& wall = world.addEntity("wall");
    wall.addComponent<Transform>(Transform{center, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
    wall.addComponent<Rectangle>(Rectangle{size, color});
    wall.addComponent<Collider>(Collider{size});
    wall.addComponent<Body>(Body{BodyType::Static});
}

/// Demo-only: lets `Z` and `X` change the camera's zoom so a human can see it.
///
/// This lives in `main.cpp` and not in the engine, on purpose. `graphics::Camera`
/// is a value with a `setZoom`; deciding that `Z` means "zoom out" is a **game**
/// decision, and putting it in `CameraSystem` would make the engine's camera
/// system have an opinion about the keyboard. The engine provides the knob; the
/// demo turns it.
///
/// It reads `isKeyPressed` rather than `isKeyDown` so one press is one step and
/// holding the key does not run the zoom away.
class ZoomKeysSystem final : public engine::ecs::System
{
public:
    explicit ZoomKeysSystem(Camera& camera) noexcept : m_camera{&camera} {}

    void update(engine::ecs::EntityManager& entities, Input& input, const float deltaSeconds) override
    {
        // Neither is needed; the zoom is a per-press action, not an integration.
        static_cast<void>(entities);
        static_cast<void>(deltaSeconds);

        if (input.isKeyPressed(Key::X))
        {
            m_camera->setZoom(m_camera->zoom() * kZoomStep);
        }
        if (input.isKeyPressed(Key::Z))
        {
            m_camera->setZoom(m_camera->zoom() / kZoomStep);
        }
    }

    [[nodiscard]] const char* name() const override { return "ZoomKeysSystem"; }

private:
    Camera* m_camera = nullptr;
};

/// Builds the Phase 9 demonstration: a world larger than the screen, a player,
/// a boundary, and scattered obstacles to show the world scrolling past.
///
/// This is a hardcoded setup on purpose. There is no scene system, no level
/// format and no level editor yet, because none of them are needed to show that
/// a camera works. `main` is where the game assembles its world; the engine
/// itself stays unaware that a player, a wall or a camera exists.
void buildCameraDemo(EntityManager& world, SystemManager& systems, Camera& camera)
{
    // Registration order is the update order, and every arrow below matters:
    //
    //   MovementSystem  input          -> velocity
    //   PhysicsSystem   velocity       -> position, then collisions
    //   CameraSystem    target position -> camera position
    //   ZoomKeysSystem  key presses    -> camera zoom
    //
    // The camera must come after physics, or it would follow where the player
    // was at the start of the frame rather than where it ended up. The zoom keys
    // come last because they only read the keyboard.
    systems.add<MovementSystem>(kPlayerSpeed);
    systems.add<PhysicsSystem>();
    systems.add<CameraSystem>(camera, kPlayerTag);
    systems.add<ZoomKeysSystem>(camera);

    // A slate grey for the boundary, a green for obstacles, and a dim blue for
    // the scattered pillars, so the eye can tell structure from scenery.
    const engine::Color wallColor{0.42F, 0.45F, 0.52F, 1.0F};
    const engine::Color obstacleColor{0.36F, 0.58F, 0.42F, 1.0F};
    const engine::Color pillarColor{0.24F, 0.32F, 0.46F, 1.0F};

    const float halfWidth = kWorldSize.x * 0.5F;
    const float halfHeight = kWorldSize.y * 0.5F;
    constexpr float kThickness = 40.0F;

    // The boundary of the world, well outside the viewport so the player cannot
    // walk off the edge of what the camera can show.
    addStaticWall(world, Vec2{halfWidth, kWorldSize.y - kThickness * 0.5F},
                  Vec2{kWorldSize.x, kThickness}, wallColor);  // floor
    addStaticWall(world, Vec2{halfWidth, kThickness * 0.5F}, Vec2{kWorldSize.x, kThickness}, wallColor);  // ceiling
    addStaticWall(world, Vec2{kThickness * 0.5F, halfHeight}, Vec2{kThickness, kWorldSize.y}, wallColor);  // left
    addStaticWall(world, Vec2{kWorldSize.x - kThickness * 0.5F, halfHeight}, Vec2{kThickness, kWorldSize.y},
                  wallColor);  // right

    // A few larger obstacles, placed so the player cannot simply walk in a
    // straight line across the world.
    addStaticWall(world, Vec2{900.0F, 400.0F}, Vec2{300.0F, 60.0F}, obstacleColor);
    addStaticWall(world, Vec2{1900.0F, 1200.0F}, Vec2{200.0F, 400.0F}, obstacleColor);
    addStaticWall(world, Vec2{2500.0F, 600.0F}, Vec2{120.0F, 120.0F}, obstacleColor);

    // A grid of small pillars. These are scenery: their only job is to make the
    // world visibly scroll past the camera, which is the whole point of the
    // phase. A loop in main is not a level loader.
    for (float x = 300.0F; x < kWorldSize.x - 200.0F; x += 500.0F)
    {
        for (float y = 250.0F; y < kWorldSize.y - 200.0F; y += 500.0F)
        {
            addStaticWall(world, Vec2{x, y}, Vec2{60.0F, 60.0F}, pillarColor);
        }
    }

    // The player. Dynamic, so physics moves it and collisions can push it back.
    // Its collider and its drawn rectangle are the same size here for
    // simplicity, but they are separate components and nothing depends on them
    // agreeing.
    // `addEntity` takes a `std::string` by value and `std::string` has no
    // implicit conversion from `string_view` in C++17, so the tag is
    // materialised here rather than passed straight through.
    Entity& player = world.addEntity(std::string{kPlayerTag});
    player.addComponent<Transform>(
        Transform{Vec2{300.0F, 900.0F}, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
    player.addComponent<Rectangle>(Rectangle{Vec2{50.0F, 50.0F}, engine::Color{0.95F, 0.78F, 0.25F, 1.0F}});
    player.addComponent<Collider>(Collider{Vec2{50.0F, 50.0F}});
    player.addComponent<Body>(Body{BodyType::Dynamic});
}

/// Reads the optional `--frames <count>` argument.
///
/// This exists so the automated smoke test can run the main loop a fixed number
/// of times and shut down on its own, rather than waiting for a human to close
/// the window. Without the flag the application runs until the window closes.
std::optional<std::size_t> parseFrameLimit(const int argc, char* const argv[])
{
    constexpr std::string_view kFrameLimitFlag = "--frames";

    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument{argv[index]};

        if (argument != kFrameLimitFlag)
        {
            continue;
        }

        if (index + 1 >= argc)
        {
            throw std::invalid_argument{"--frames requires a value"};
        }

        const std::string_view value{argv[++index]};
        std::size_t frameCount = 0;
        const std::from_chars_result result =
            std::from_chars(value.data(), value.data() + value.size(), frameCount);

        if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || frameCount == 0)
        {
            throw std::invalid_argument{"--frames expects a positive integer"};
        }

        return frameCount;
    }

    return std::nullopt;
}

} // namespace

int main(const int argc, char* const argv[])
{
    try
    {
        const std::optional<std::size_t> frameLimit = parseFrameLimit(argc, argv);

        engine::Application application;
        buildCameraDemo(application.entityManager(), application.systemManager(), application.camera());
        return application.run(frameLimit);
    }
    catch (const std::exception& error)
    {
        std::cerr << "Fatal error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
