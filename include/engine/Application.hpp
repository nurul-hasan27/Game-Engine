#pragma once

#include "engine/Time.hpp"
#include "engine/ecs/EntityManager.hpp"
#include "engine/ecs/SystemManager.hpp"

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
/// | `EntityManager` | the world: every entity and its components |
/// | `SystemManager` | the behaviour, run in registration order |
///
/// Application is the only place these four are wired together. It does not
/// implement any behaviour of its own: there is no physics, no rendering logic
/// and no gameplay here, only ordering. The window is the one genuinely
/// SFML-dependent member, which is what keeps SFML confined to this boundary.
///
/// ### The frame
///
/// ```text
/// processEvents()                  -> window.isOpen() becomes false on close
/// time.tick()                      -> measures this frame's real duration
/// systemManager.update(world, dt)  -> behaviour writes component data
/// entityManager.update()           -> deferred destruction cleanup
/// render()                         -> draw
/// ```
///
/// Systems run before cleanup on purpose. A system that asks for an entity to
/// die only sets a flag, and the erase happens in `entityManager.update()`
/// afterwards, so nothing is removed from underneath a running system.
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

    /// The behaviour. Systems are registered here and run in the order they
    /// were added.
    [[nodiscard]] ecs::SystemManager& systemManager() noexcept { return m_systemManager; }

    /// Frame timing, mainly so a debug overlay could read it. Systems are handed
    /// the delta as a parameter and should not need this.
    [[nodiscard]] const Time& time() const noexcept { return m_time; }

private:
    void processEvents();
    void update();
    void render();

    sf::RenderWindow m_window;
    Time m_time;
    ecs::EntityManager m_entityManager;
    ecs::SystemManager m_systemManager;
    bool m_isRunning = true;
};

} // namespace engine
