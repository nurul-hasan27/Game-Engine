#include "engine/scene/PlayScene.hpp"

#include "engine/EngineConfig.hpp"
#include "engine/components/Text.hpp"
#include "engine/ecs/System.hpp"
#include "engine/components/Transform.hpp"
#include "engine/input/Action.hpp"
#include "engine/level/LevelFile.hpp"
#include "engine/level/LevelLoader.hpp"
#include "engine/systems/AnimationSystem.hpp"
#include "engine/systems/CameraSystem.hpp"
#include "engine/systems/LifetimeSystem.hpp"
#include "engine/systems/PhysicsSystem.hpp"
#include "engine/systems/PlayerStateSystem.hpp"
#include "engine/systems/PlayerSystem.hpp"
#include "engine/systems/ShootSystem.hpp"
#include "engine/systems/TileSystem.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <utility>

namespace engine::scene
{

namespace
{

/// The committed pixel font, at the size the course reference draws debug text.
constexpr std::string_view kLabelFont = "fonts_pixeled";
constexpr std::uint32_t kLabelSize = 12U;

/// Adds one world-space string, centred on `position`.
///
/// Deliberately **not** screen space, unlike the menu's: this label describes where
/// the player started in the level, so it belongs to the world and scrolls and
/// zooms with everything else. It is also the contrast that proves screen space is
/// a choice rather than a new default - the same font, the same renderer, the same
/// component, one marked and one not.
void addTextLabel(ecs::EntityManager& world, std::string content, const Vec2 position)
{
    ecs::Entity& label = world.addEntity("level.label");
    label.addComponent<components::Transform>(
        components::Transform{position, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F}, 0.0F});

    components::Text text;
    text.content = std::move(content);
    // `std::string`, not `std::string_view`: the component owns the name, and
    // `string_view` does not convert implicitly.
    text.fontAssetName = std::string{kLabelFont};
    text.characterSize = kLabelSize;
    label.addComponent<components::Text>(std::move(text));
}


/// The zoom demonstration, moved here from `main.cpp` with the rest of the level.
///
/// Its own documentation says why it is not in the engine, and that reason still
/// holds: `graphics::Camera` is a value with a `setZoom`, and deciding that `Z`
/// means "zoom out" is a *game* decision. What changed is only which file the game
/// lives in - before Phase 15 it was `main.cpp`, and `main` no longer knows what a
/// level is.
///
/// Registering it in the same order `main` used to, between `CameraSystem` and
/// `AnimationSystem`, is what keeps the camera's zoom available in the level at
/// all. Dropping it would have been the quiet kind of regression: the level would
/// still load and still draw, and `ZoomIn`/`ZoomOut` would simply be two actions
/// nothing read.
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
        explicit ZoomKeysSystem(graphics::Camera& camera) noexcept : m_camera{&camera} {}

        void update(engine::ecs::EntityManager& entities, const input::ActionState& actions,
                const float deltaSeconds) override
        {
            // Neither is needed; the zoom is a per-press action, not an integration.
            static_cast<void>(entities);
            static_cast<void>(deltaSeconds);

            // `wasPressed` rather than `isActive`, and that is the whole reason the
            // action layer keeps both: a per-press action asked "is it held?" would
            // change the zoom every frame the key is down, and holding Z to zoom out
            // would divide the zoom by kZoomStep sixty times a second.
            if (actions.wasPressed(input::Action::ZoomIn))
            {
                m_camera->setZoom(m_camera->zoom() * kZoomStep);
            }
            if (actions.wasPressed(input::Action::ZoomOut))
            {
                m_camera->setZoom(m_camera->zoom() / kZoomStep);
            }
        }

        [[nodiscard]] const char* name() const override { return "ZoomKeysSystem"; }

    private:
        graphics::Camera* m_camera = nullptr;
    };

} // namespace

PlayScene::PlayScene(const SceneContext& context)
    : Scene{context}, m_renderSystem{context.renderer(), context.camera(), context.assets()},
      // The one place an integer cell count becomes the float a grid is built from.
      // The count is whole because a cell is, and the multiplication by kCellSize
      // that follows is the grid's business, not the configuration's.
      m_cellsTall{static_cast<float>(config::kLevelCellsTall)}
{
    // Registration order is the update order, and every line below is load bearing.
    //
    //   PlayerSystem     actions       -> intent, jump, gravity, respawn, facing
    //   ShootSystem      Shoot press   -> a bullet entity, in the direction the player
    //                                     already faces, moving this same frame
    //   LifetimeSystem   frame counter -> "your last frame was this one"
    //   PhysicsSystem    velocity      -> position, then collisions, then reports
    //                                     what overlapped and how it arrived
    //   PlayerStateSystem that report  -> grounded, Stand/Run/Air, and the picture
    //   TileSystem       that report    -> bricks explode, question blocks are used,
    //                                     coins appear, bullets stop
    //   CameraSystem     target        -> camera position, *after* physics so it
    //                                     follows where the player ended up
    //   ZoomKeysSystem   zoom actions  -> camera zoom
    //   AnimationSystem  game frame    -> which animation frame is showing, last
    //
    // The order is the whole design and each line constrains the next.
    //
    // **PlayerSystem first**, because it writes the velocity that PhysicsSystem
    // integrates in the *same* frame. After physics would mean the player's walk
    // starts a frame late, and the lateness would differ on the first frame after a
    // scene transition than on the hundredth, which reads as an inconsistency rather
    // than as input lag.
    //
    // **ShootSystem immediately after it**, for two reasons that are both about this frame.
    // It reads `scale.x` to decide which way the bullet goes, and PlayerSystem is what
    // writes it - the authority for facing is the system that last set it, so firing
    // before that would shoot the direction the player faced a frame ago. And it must
    // create the bullet *before* the integration, or the new entity waits a frame for its
    // first move, is absent from this frame's collision pass, and cannot be in this
    // frame's report.
    //
    // **LifetimeSystem before the physics step**, so an entity whose last frame is this one
    // neither moves nor collides this frame. Counting it down afterwards would leave a
    // question about whether an entity that is already dead still gets to blow up the
    // brick it hit, and the answer would be a fact about the order rather than about the
    // game. It also puts the countdown ahead of the coin that TileSystem creates later in
    // the same frame, which is what makes "thirty frames" mean thirty.
    //
    // **PhysicsSystem before CameraSystem**, which predates this phase and still
    // holds: the camera follows where the player *ended* up, not where they started.
    //
    // **TileSystem straight after PlayerStateSystem**, because both read the same report and
    // neither writes anything the other reads. It is before AnimationSystem because the
    // explosion it starts is advanced the same frame it starts, so a brick breaks and
    // explodes on one frame rather than two.
    //
    // **AnimationSystem last**, so the animation PlayerSystem chose this frame is the
    // one advanced this frame. Reversed, every state change would be a frame late and
    // the walk cycle would stutter at each boundary.
    //
    // No MovementSystem, and the reason is in the class documentation: this engine's
    // movement system acts on *every* transform, so registering it would set a
    // velocity on every ground tile and every cloud. PlayerSystem is the replacement
    // and the difference is the whole point - it acts on entities carrying
    // components::Player, of which the level contains exactly one.
    //
    // The fall limit is the world's bottom edge, measured from the grid this scene
    // built. It is an argument rather than a constant because it is a property of the
    // level: a world four cells tall respawns the player sooner, and a magic number
    // in PlayerSystem would respawn them at the same place in every world.
    m_systems.add<engine::systems::PlayerSystem>(m_cellsTall * level::LevelGrid::kCellSize);

    // A bullet's animation comes from the level and its box comes from that animation's
    // frame size, so the shoot system needs the asset manager for exactly the same reason
    // AnimationSystem does. Borrowed, from the context the scene already holds.
    m_systems.add<engine::systems::ShootSystem>(context.assets());

    // See the ordering note above: deliberately ahead of the integration.
    m_systems.add<engine::systems::LifetimeSystem>();

    // `add` hands back a reference, and it stays valid: the system manager holds systems
    // through `unique_ptr` precisely so that registering another one does not move this
    // one. So the two consumers of its report can be given the physics system's own report
    // rather than reaching for a global - two scenes in one process, or a test with two
    // worlds, each see only their own collisions.
    const engine::systems::PhysicsSystem& physics = m_systems.add<engine::systems::PhysicsSystem>();

    // Immediately after physics, and that position is the phase. The player's `grounded`
    // flag is read from the collisions this frame resolved, so registering it earlier
    // would put it a frame behind and reintroduce exactly the lag the collision report
    // exists to remove. See PlayerStateSystem's own documentation.
    m_systems.add<engine::systems::PlayerStateSystem>(physics.collisions());

    // The other reader of the same report. It needs the asset manager as well, to resolve
    // the used question block's and the coin's animation and to read the coin's frame size
    // for the scale it is drawn at.
    m_systems.add<engine::systems::TileSystem>(physics.collisions(), context.assets());

    m_systems.add<engine::systems::CameraSystem>(context.camera(), std::string{level::kPlayerTag});
    // Local class, not an engine one - see its own documentation above.
    m_systems.add<ZoomKeysSystem>(context.camera());
    m_systems.add<engine::systems::AnimationSystem>(context.assets());

    // Read, parse, spawn: three steps that never appear together anywhere else, and
    // the seam between them is why the parser is testable with a string literal and
    // the loader with a hand-built level.
    const level::Level level = level::loadLevelFile(std::filesystem::path{config::kLevelFile});

    // The height is the game's, not the level's. See LevelGrid for why the format
    // carries none.
    const level::LevelGrid grid = level::LevelGrid::withCellsTall(m_cellsTall);

    const level::LevelLoader loader{context.assets()};
    const std::size_t spawned = loader.spawn(level, m_world, grid);

    // A label proving the text path end to end, through the real font and a real
    // window, reporting something the level really carries rather than a caption.
    addTextLabel(m_world, "SPAWN " + std::to_string(static_cast<int>(level.player().gridX)) + "," +
                              std::to_string(static_cast<int>(level.player().gridY)),
                 Vec2{64.0F, m_cellsTall * level::LevelGrid::kCellSize - 48.0F});

    if (level.requiredCellsTall() > m_cellsTall)
    {
        // Reported, not fatal. A level taller than the world is *mostly* right, and
        // refusing to run it would be worse than running it with the overflow
        // visible. The level designer sees this at startup, which is where they
        // would look.
        std::cerr << "Warning: this level needs " << level.requiredCellsTall()
                  << " cells of height but the game world is only " << m_cellsTall
                  << " cells tall; anything above that is off the top of the world.\n";
    }

    std::cerr << "Loaded " << level.tiles().size() << " tiles, " << level.decorations().size()
              << " decorations, 1 player and 1 text label (" << spawned + 1 << " entities).\n";
}

void PlayScene::onUpdate(const input::ActionState& actions, const float deltaSeconds)
{
    // Back to the menu, on the press edge. The course's Assignment 3 says "The 'ESC'
    // key should go 'back' to the Main Menu, or quit if on the Main Menu", and the
    // menu is where "back" goes - so this is `Quit`, an action the action layer
    // already bound to `Escape`.
    //
    // Recorded as a request rather than performed: the scene cannot replace itself.
    if (actions.wasPressed(input::Action::Quit))
    {
        requestTransition(SceneTransition::to(SceneId::Menu));
    }

    m_systems.update(m_world, actions, deltaSeconds);

    // Deferred destruction cleanup, after the systems have run, for the same reason
    // `Application` does it after its own: `AnimationSystem` flags entities for
    // destruction and something has to erase them once nothing is iterating. A
    // scene owns its world, so the scene flushes it - the owner's `EntityManager`
    // is a *different* world and would never see these entities.
    m_world.update();
}

void PlayScene::render()
{
    // The empty snapshot again, because a render pass reads no input and the render
    // system reads none either.
    m_renderSystem.update(m_world, m_noActions, 0.0F);
}

} // namespace engine::scene
