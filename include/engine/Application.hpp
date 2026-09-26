#pragma once

#include <SFML/Graphics/RenderWindow.hpp>
#include <SFML/Window/Event.hpp>

#include <cstddef>
#include <optional>

namespace engine
{

/// Owns the game window and drives the main loop: process events, update, render.
///
/// The window is an sf::RenderWindow, which is SFML's renderable window: it
/// combines sf::Window (events, timing) with sf::RenderTarget (clear, draw,
/// display). Later phases need the drawing half for sprites, text, vertex arrays
/// and particles, so this is the type the engine is built around.
///
/// Phase 1 keeps this class deliberately minimal. The update/render split
/// already exists so that later phases can slot in input, ECS, physics and
/// scene systems without having to restructure the loop.
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

private:
    void processEvents();
    void update();
    void render();

    sf::RenderWindow m_window;
    bool m_isRunning = true;
};

} // namespace engine
