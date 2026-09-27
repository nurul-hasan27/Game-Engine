#include "engine/Application.hpp"
#include "engine/components/Body.hpp"
#include "engine/components/Collider.hpp"
#include "engine/components/Rectangle.hpp"
#include "engine/components/Transform.hpp"
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
using engine::physics::BodyType;
using engine::systems::MovementSystem;
using engine::systems::PhysicsSystem;

/// The player's top speed, in pixels per second.
constexpr float kPlayerSpeed = 300.0F;

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

/// Builds the Phase 8 demonstration: one dynamic player in a walled arena with a
/// couple of obstacles in the middle.
///
/// This is a hardcoded setup on purpose. There is no scene system, no level
/// format and no level editor yet, because none of them are needed to show that
/// collision works. `main` is where the game assembles its world; the engine
/// itself stays unaware that a player or a wall exists.
void buildCollisionDemo(EntityManager& world, SystemManager& systems)
{
    // Registration order is the update order, and it matters here: movement turns
    // input into velocity, then physics integrates that velocity into position
    // and resolves the collisions. Registered the other way round, physics would
    // integrate the previous frame's velocity and input would lag by a frame.
    systems.add<MovementSystem>(kPlayerSpeed);
    systems.add<PhysicsSystem>();

    // A slate grey for the arena structure.
    const engine::Color wallColor{0.42F, 0.45F, 0.52F, 1.0F};
    // The obstacles are tinted differently so it is obvious at a glance which
    // walls are the boundary and which ones are in the middle.
    const engine::Color obstacleColor{0.36F, 0.58F, 0.42F, 1.0F};

    // The four edges of the window, so the player cannot leave the screen. This
    // is what makes "walk into a wall" testable in every direction.
    addStaticWall(world, Vec2{640.0F, 700.0F}, Vec2{1320.0F, 40.0F}, wallColor);  // floor
    addStaticWall(world, Vec2{640.0F, 20.0F}, Vec2{1320.0F, 40.0F}, wallColor);   // ceiling
    addStaticWall(world, Vec2{20.0F, 360.0F}, Vec2{40.0F, 760.0F}, wallColor);    // left
    addStaticWall(world, Vec2{1260.0F, 360.0F}, Vec2{40.0F, 760.0F}, wallColor);  // right

    // Two obstacles in the middle. The player starts to the left of the first,
    // so walking right runs straight into it.
    addStaticWall(world, Vec2{640.0F, 300.0F}, Vec2{240.0F, 60.0F}, obstacleColor);
    addStaticWall(world, Vec2{900.0F, 500.0F}, Vec2{120.0F, 120.0F}, obstacleColor);

    // The player. Dynamic, so physics moves it and collisions can push it back.
    // Its collider and its drawn rectangle are the same size here for
    // simplicity, but they are separate components and nothing depends on them
    // agreeing.
    Entity& player = world.addEntity("player");
    player.addComponent<Transform>(Transform{Vec2{200.0F, 300.0F}, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});
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
        buildCollisionDemo(application.entityManager(), application.systemManager());
        return application.run(frameLimit);
    }
    catch (const std::exception& error)
    {
        std::cerr << "Fatal error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
