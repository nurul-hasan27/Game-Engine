#pragma once

#include "engine/Time.hpp"
#include "engine/ecs/EntityManager.hpp"
#include "engine/ecs/SystemManager.hpp"
#include "engine/graphics/Camera.hpp"
#include "engine/graphics/SfmlRenderer.hpp"
#include "engine/input/Input.hpp"
#include "engine/systems/RenderSystem.hpp"

#include <SFML/Graphics/RenderWindow.hpp>
#include <SFML/Window/Event.hpp>

#include <cstddef>
#include <optional>

namespace engine
{

/// Owns the window and drives the main loop, and is the composition root for
/// the runtime.
///
/// ### What Application owns
///
/// | Member | Responsibility |
/// | ------ | -------------- |
/// | `sf::RenderWindow` | the window, its events, and presentation |
/// | `Time` | measures how long each frame actually took |
/// | `input::Input` | keyboard state for the current frame |
/// | `EntityManager` | the world: every entity and its components |
/// | `SystemManager` | the simulation systems, run in registration order |
/// | `graphics::Camera` | where in the world the view is looking |
/// | `SfmlRenderer` | the graphics boundary; references the window |
/// | `RenderSystem` | draws the world through the renderer |
///
/// Application is the only place these are wired together. It implements no
/// behaviour of its own: there is no physics, no gameplay and no drawing code
/// here, only ordering. The simulation systems themselves are registered by the
/// game, through `systemManager()`.
///
/// ### The camera is a plain owned value
///
/// `graphics::Camera` is a value, not a component and not a singleton, exactly
/// like `Input` and `Time`. It is view state rather than world state: it
/// describes where the *player of the game* is looking, which is a property of
/// the program, not of any entity. Making it a component would mean inventing a
/// singleton entity to hold three numbers and would give every system a way to
/// reach the camera by querying, which is the coupling this design avoids.
///
/// Systems that need the camera are handed a reference to it. `RenderSystem`
/// takes a `const&` and can only read it; `CameraSystem` takes a `&` and drives
/// it. Neither owns it.
///
/// ### Window ownership
///
/// The window belongs to Application and nobody else. `SfmlRenderer` holds a
/// **reference** to it rather than owning it, and `RenderSystem` holds a
/// reference to the renderer. Ownership therefore runs in one direction only,
/// Application to window, with everything else borrowing. The window's
/// destructor still closes it, so shutdown is still plain RAII: no global
/// window, no static window, no singleton renderer, no global graphics context.
///
/// Member declaration order matters here: the window is declared first so it is
/// constructed first, and the renderer and render system bind to an already
/// constructed window. Destruction runs in reverse, so they go before the window
/// closes. The camera is declared before `RenderSystem` for the same reason.
///
/// ### The frame
///
/// ```text
/// input.beginFrame()                 clear pressed/released, keep held state
/// processEvents()                    window close, and keyboard into Input
/// time.tick()                        measures this frame's real duration
/// systemManager.update(world, in, dt) simulation systems read input, write data
///   ├─ MovementSystem                 input -> Transform::velocity
///   ├─ PhysicsSystem                  velocity -> position, then collisions
///   └─ CameraSystem                   target's world position -> Camera
/// entityManager.update()             deferred destruction cleanup
/// renderer.beginFrame()              RENDER: start the frame
/// renderer.clear(background)         RENDER: configured background colour
/// renderSystem.update(world, in, 0)  RENDER: world -> screen, submit draws
/// renderer.endFrame()                RENDER: present
/// ```
///
/// The event, update, render, display high-level order from Phase 1 is intact.
/// Systems run before cleanup, so an entity a system flagged this frame is
/// erased only once nothing is iterating.
///
/// The systems shown inside `systemManager.update` are registered by whoever
/// builds the game, not here: `Application` owns the loop and orders the pass,
/// but it does not decide what simulates. `PhysicsSystem` must be registered
/// after `MovementSystem`, and `CameraSystem` after both, because registration
/// order is update order and the camera should follow where the target ended up
/// this frame.
///
/// The camera is written during simulation and read during rendering. That is
/// the whole of its involvement in the frame: it changes what you see, never
/// what is simulated.
///
/// `input.beginFrame()` runs **before** `processEvents()` on purpose. It clears
/// only the frame-local transients, so clearing can never discard an event that
/// arrived this frame: a key pressed during `processEvents()` is still readable
/// by every system later in the same frame. Held state survives the clear.
///
/// `RenderSystem` is deliberately **not** registered in the `SystemManager`. It
/// is an ordinary `System` with the same interface and lifetime rules, but the
/// render pass has to be bracketed by `beginFrame()` and `endFrame()` and has to
/// run after every simulation system, so `Application` drives it in its own
/// pass. Folding it into the simulation list would mean either clearing mid
/// simulation or presenting before the last system ran. This is recorded in
/// [docs/rendering.md](docs/rendering.md).
class Application
{
public:
    /// Opens the game window using the settings from engine/EngineConfig.hpp.
    Application();

    /// Closes the window. sf::Window cleans itself up, so nothing else is needed.
    ~Application() = default;

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    Application(Application&&) = delete;
    Application& operator=(Application&&) = delete;

    /// Runs the main loop until the window is closed and returns EXIT_SUCCESS.
    ///
    /// @param maxFrameCount Optional cap on the number of frames to run. The
    ///        automated smoke test uses it so the loop terminates on its own
    ///        instead of waiting for a user to close the window.
    int run(std::optional<std::size_t> maxFrameCount = std::nullopt);

    /// The world. Entities are added here by main(), by an example, or by a
    /// future scene, never by a system.
    [[nodiscard]] ecs::EntityManager& entityManager() noexcept { return m_entityManager; }

    /// The view onto the world.
    ///
    /// The camera is mutable because the game configures it, and a
    /// `CameraSystem` drives it. It is handed out by reference rather than
    /// const so the viewport can be set once the window size is known.
    [[nodiscard]] graphics::Camera& camera() noexcept { return m_camera; }

    /// The behaviour. Systems are registered here and run in the order they
    /// were added.
    [[nodiscard]] ecs::SystemManager& systemManager() noexcept { return m_systemManager; }

    /// Frame timing, mainly so a debug overlay could read it. Systems are handed
    /// the delta as a parameter and should not need this.
    [[nodiscard]] const Time& time() const noexcept { return m_time; }

    /// The graphics boundary, for a debug overlay or a tool. Systems should be
    /// handed an `engine::graphics::Renderer&` instead of reaching for this.
    [[nodiscard]] graphics::Renderer& renderer() noexcept { return m_renderer; }

    /// Keyboard state for the current frame, mainly for tools. Systems are
    /// handed the same object by reference and should not reach for this.
    [[nodiscard]] const input::Input& input() const noexcept { return m_input; }

private:
    void processEvents();
    void update();
    void render();

    // Declaration order is load bearing: the window is constructed first, then
    // the renderer binds to it, then the render system binds to the renderer and
    // the already-configured camera. Destruction is the reverse, so no borrower
    // outlives what it points at.
    sf::RenderWindow m_window;
    Time m_time;
    input::Input m_input;
    ecs::EntityManager m_entityManager;
    ecs::SystemManager m_systemManager;
    graphics::Camera m_camera;
    graphics::SfmlRenderer m_renderer;
    systems::RenderSystem m_renderSystem;
    bool m_isRunning = true;
};

} // namespace engine
