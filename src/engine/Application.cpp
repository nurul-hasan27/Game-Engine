#include "engine/Application.hpp"
#include "engine/EngineConfig.hpp"
#include "engine/input/SfmlKeyMap.hpp"
#include "engine/math/Vec2.hpp"

#include <cstdlib>
#include <filesystem>

namespace engine
{
namespace
{

/// Builds an engine colour from the configured 0-255 background channels.
[[nodiscard]] constexpr Color toColor(const unsigned char red, const unsigned char green,
                                       const unsigned char blue) noexcept
{
    return Color{static_cast<float>(red) / 255.0F, static_cast<float>(green) / 255.0F,
                 static_cast<float>(blue) / 255.0F, 1.0F};
}

/// The background the renderer clears to, still driven by the Phase 1 config.
constexpr Color kWindowBackground = toColor(config::kBackgroundColorRed, config::kBackgroundColorGreen,
                                            config::kBackgroundColorBlue);

} // namespace

Application::Application()
    : m_window(sf::VideoMode{config::kWindowWidth, config::kWindowHeight}, config::kWindowTitle), m_time{},
      m_input{}, m_entityManager{}, m_systemManager{}, m_camera{}, m_renderer{m_window},
      m_assets{std::filesystem::path{config::kAssetsConfig}},
      // Borrowed by const reference, and declared after all three so they are
      // already alive. See the member ordering note in Application.hpp for why the
      // declaration order, and not just this line, is what makes it safe.
      m_renderSystem{m_renderer, m_camera, m_assets}, m_isRunning{true}
{
    m_window.setFramerateLimit(config::kFramerateLimit);

    // The camera's viewport is the window's size in pixels. Position and zoom
    // keep their defaults, so a default camera looks at the world origin at 1:1.
    //
    // This is read once, at construction. The camera is deliberately not tied to
    // the window object and there is no resize handling yet: a fixed viewport is
    // enough for this phase, and wiring resize events in would mean deciding
    // what a camera should do when the window changes shape, which is a design
    // question rather than a plumbing one.
    m_camera.setViewport(Vec2{static_cast<float>(config::kWindowWidth), static_cast<float>(config::kWindowHeight)});
}

int Application::run(const std::optional<std::size_t> maxFrameCount)
{
    std::size_t frameCount = 0;

    while (m_isRunning)
    {
        // Clear only the frame-local input transients, and do it before events
        // are processed. Held keys survive; a press seen during this frame's
        // processEvents() is still visible to systems further down the frame.
        m_input.beginFrame();

        processEvents();

        if (!m_isRunning)
        {
            break; // the window was closed while we were handling events
        }

        update();
        render();
        ++frameCount;

        if (maxFrameCount.has_value() && frameCount >= *maxFrameCount)
        {
            break;
        }
    }

    return EXIT_SUCCESS;
}

void Application::processEvents()
{
    sf::Event event{};

    while (m_window.pollEvent(event))
    {
        switch (event.type)
        {
            case sf::Event::Closed:
                // Application lifecycle, and only Application. A window close is
                // never turned into a keyboard event, and keyboard Escape is
                // never turned into a window close: Input merely reports it, and
                // deciding what Escape means is a policy the game makes.
                m_isRunning = false;
                break;

            default:
                // The adapter recognises keyboard presses and releases and
                // ignores everything else, so the filter lives in one place
                // rather than in the case labels.
                input::applyKeyboardEvent(event, m_input);
                break;
        }
    }
}

void Application::update()
{
    // Measure the frame that just happened. This is real elapsed time, not
    // 1 / targetFps: the frame rate cap is a rendering decision and gameplay
    // correctness must not depend on it.
    m_time.tick();

    // Behaviour: every system gets the same input state and the same delta for
    // this frame, and none of them can reach either another way.
    m_systemManager.update(m_entityManager, m_input, m_time.deltaSeconds());

    // Deferred destruction cleanup, after systems have run, so an entity a
    // system flagged this frame is erased only once nothing is iterating.
    m_entityManager.update();
}

void Application::render()
{
    // The render pass: begin, clear to the configured background, let the render
    // system submit its draws, then present. Application performs no drawing
    // itself; it only sequences the pass.
    m_renderer.beginFrame();
    m_renderer.clear(kWindowBackground);
    m_renderSystem.update(m_entityManager, m_input, 0.0F);
    m_renderer.endFrame();
}

} // namespace engine
