#include "engine/Application.hpp"
#include "engine/components/Animation.hpp"
#include "engine/components/Body.hpp"
#include "engine/components/Collider.hpp"
#include "engine/components/Rectangle.hpp"
#include "engine/components/Transform.hpp"
#include "engine/EngineConfig.hpp"
#include "engine/graphics/Camera.hpp"
#include "engine/level/LevelFile.hpp"
#include "engine/level/LevelGrid.hpp"
#include "engine/level/LevelLoader.hpp"
#include "engine/input/Action.hpp"
#include "engine/input/ActionState.hpp"
#include "engine/systems/AnimationSystem.hpp"
#include "engine/systems/CameraSystem.hpp"
#include "engine/systems/MovementSystem.hpp"
#include "engine/systems/PhysicsSystem.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
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
using engine::components::Animation;
using engine::components::Body;
using engine::components::Collider;
using engine::components::Rectangle;
using engine::components::Transform;
using engine::ecs::Entity;
using engine::ecs::EntityManager;
using engine::ecs::SystemManager;
using engine::graphics::Camera;
using engine::level::Level;
using engine::level::LevelGrid;
using engine::level::LevelLoader;
using engine::input::Action;
using engine::input::ActionState;
using engine::physics::BodyType;
using engine::systems::AnimationSystem;
using engine::systems::CameraSystem;
using engine::systems::MovementSystem;
using engine::systems::PhysicsSystem;

/// How much one press of the zoom keys changes the zoom, as a factor.
constexpr float kZoomStep = 1.25F;

/// Demo-only: lets `Z` and `X` change the camera's zoom so a human can see it.
///
/// This lives in `main.cpp` and not in the engine, on purpose. `graphics::Camera`
/// is a value with a `setZoom`; deciding that `Z` means "zoom out" is a **game**
/// decision, and putting it in `CameraSystem` would make the engine's camera
/// system have an opinion about the keyboard. The engine provides the knob; the
/// demo turns it.
///
/// It asks the action layer for `wasPressed` rather than `isActive`, so one press
/// is one step and holding the key does not run the zoom away - and it cannot ask
/// about a key at all, because a system is handed the action snapshot and not the
/// keyboard.
class ZoomKeysSystem final : public engine::ecs::System
{
public:
    explicit ZoomKeysSystem(Camera& camera) noexcept : m_camera{&camera} {}

    void update(engine::ecs::EntityManager& entities, const ActionState& actions, const float deltaSeconds) override
    {
        // Neither is needed; the zoom is a per-press action, not an integration.
        static_cast<void>(entities);
        static_cast<void>(deltaSeconds);

        // `wasPressed` rather than `isActive`, and that is the whole reason the
        // action layer keeps both: a per-press action asked "is it held?" would
        // change the zoom every frame the key is down, and holding Z to zoom out
        // would divide the zoom by kZoomStep sixty times a second.
        if (actions.wasPressed(Action::ZoomIn))
        {
            m_camera->setZoom(m_camera->zoom() * kZoomStep);
        }
        if (actions.wasPressed(Action::ZoomOut))
        {
            m_camera->setZoom(m_camera->zoom() / kZoomStep);
        }
    }

    [[nodiscard]] const char* name() const override { return "ZoomKeysSystem"; }

private:
    Camera* m_camera = nullptr;
};
/// How tall the game's world is, in 64-pixel cells.
///
/// **The level format has no height field**, so this cannot come from the level
/// file - `Tile`, `Dec` and `Player` all describe positions, and none of them
/// describes the size of the world. A height is a property of the *game*, and this
/// is the game stating it.
///
/// It has to be at least [Level::requiredCellsTall] for the level being loaded. If
/// it is smaller, the flip is taken about the wrong axis and the entities that
/// should be near the top of the level end up above the top of the world, which is
/// visible but easy to misread as a conversion bug. Sixteen clears
/// `assets/levels/level1.txt`, whose highest thing is a decoration at 700 pixels,
/// i.e. 10.94 cells.
constexpr float kLevelCellsTall = 16.0F;

/// Builds the world from the level file, and the systems that run in it.
///
/// ### This replaced a hardcoded demonstration
///
/// Until Phase 13 this function assembled a hand-written world of walls and
/// pillars, and said why: *"There is no scene system, no level format and no level
/// editor yet, because none of them are needed to show that a camera works."*
/// Phase 13 added the level format, so that reason expired and the hand-written
/// world went with it. Leaving it would have meant shipping two worlds, or
/// shipping a level file that nothing reads - which is the same mistake Phase 11
/// avoided when it declined to declare thirteen single-frame animations that
/// nothing used.
///
/// The camera is still demonstrated: the level is 32 cells by 16, which is 2048 by
/// 1024 pixels against a 1280 by 720 window, so the view genuinely has somewhere to
/// scroll. Animation is still demonstrated too, by the Goomba tiles in the level
/// file, which carry the multi-frame `mario_GoombaWalk_walk` animation.
///
/// ### Loading a level is the *game's* job, not the engine's
///
/// [engine::Application] is the composition root and stays ignorant of levels. It
/// owns the asset manager, the world and the systems and wires them together, but
/// it does not know what a level is, and adding that would make the engine a game.
/// The engine provides the parser, the grid and the loader; *which* level to load
/// and how tall a world to put it in are decisions, and they are made here.
///
/// ### Why there is no movement system
///
/// The engine's [MovementSystem] is a demonstration system, not the Assignment 3
/// player: it sets a velocity on **every** entity that has a transform. The course
/// is explicit that *"All movement logic should be in the movement system"* and
/// that it must move the player and nothing else.
///
/// Registering it here would give every ground tile and every cloud a velocity.
/// Tiles would be saved by [PhysicsSystem] forcing a static body's velocity to
/// zero, but a decoration has no body at all - the course requires it to have no
/// bounding box and no interactions - so it would be integrated and slide around the
/// screen at whatever speed the player was moving.
///
/// So the player does not move yet, and that is not an omission. The level loads,
/// the world exists, it draws, and it collides. Making the player move needs a
/// movement system that knows which entity is the player, and writing one is the
/// next phase's job, not this one's.
void buildLevel(EntityManager& world, SystemManager& systems, Camera& camera,
                const engine::assets::AssetManager& assets)
{
    // Registration order is the update order, and the reasoning is Phase 9's, which
    // still holds for a world that came from a file:
    //
    //   PhysicsSystem    velocity     -> position, then collisions
    //   CameraSystem     target       -> camera position, after physics so it follows
    //                                     where the player *ended* up
    //   ZoomKeysSystem   zoom actions -> camera zoom
    //   AnimationSystem  game frame   -> which frame is showing, last
    //
    // No MovementSystem, for the reason above.
    systems.add<PhysicsSystem>();
    systems.add<CameraSystem>(camera, std::string{engine::level::kPlayerTag});
    systems.add<ZoomKeysSystem>(camera);
    systems.add<AnimationSystem>(assets);

    // Read, then parse, then spawn: three steps that never appear in one place
    // anywhere else, and the seam between them is what lets the parser be tested
    // with a string literal and the loader with a hand-built `Level`.
    //
    // Both the open and the parse can throw, and both are allowed to: a level that
    // does not load is a fatal error the game reports and exits on, not something
    // to load half of and carry on. `main` catches it and says what it was.
    const Level loadedLevel = engine::level::loadLevelFile(std::filesystem::path{engine::config::kLevelFile});

    // The height is the game's, not the level's - see [kLevelCellsTall].
    const LevelGrid grid = LevelGrid::withCellsTall(kLevelCellsTall);

    const LevelLoader loader{assets};
    const std::size_t spawned = loader.spawn(loadedLevel, world, grid);

    // Reported rather than asserted. A level taller than the world it is loaded into
    // still loads, and still draws - the entities that overflow simply sit above the
    // top edge - so throwing here would refuse to run a level that is only *mostly*
    // wrong. Printing it makes the mismatch visible at startup, which is where a
    // level designer will actually see it, and leaves the decision to them.
    if (loadedLevel.requiredCellsTall() > kLevelCellsTall)
    {
        std::cerr << "Warning: this level needs " << loadedLevel.requiredCellsTall()
                  << " cells of height but the game "
                  << "world is only " << kLevelCellsTall << " cells tall; anything above that is off the top of "
                  << "the world.\n";
    }

    std::cerr << "Loaded " << loadedLevel.tiles().size() << " tiles, " << loadedLevel.decorations().size()
              << " decorations and 1 player (" << spawned << " entities).\n";
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
        buildLevel(application.entityManager(), application.systemManager(), application.camera(),
                   application.assets());
        return application.run(frameLimit);
    }
    catch (const std::exception& error)
    {
        std::cerr << "Fatal error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
