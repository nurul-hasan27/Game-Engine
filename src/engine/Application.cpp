#include "engine/Application.hpp"
#include "engine/EngineConfig.hpp"

#include <SFML/Graphics/Color.hpp>

#include <cstdlib>

namespace engine
{

Application::Application()
    : m_window(sf::VideoMode{config::kWindowWidth, config::kWindowHeight}, config::kWindowTitle)
{
    m_window.setFramerateLimit(config::kFramerateLimit);
}

int Application::run(const std::optional<std::size_t> maxFrameCount)
{
    std::size_t frameCount = 0;

    while (m_isRunning)
    {
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
                m_isRunning = false;
                break;

            default:
                // Every other event is ignored in Phase 1. Input handling
                // arrives with the input system in a later phase.
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

    // Behaviour: every system gets the same delta for this frame.
    m_systemManager.update(m_entityManager, m_time.deltaSeconds());

    // Deferred destruction cleanup, after systems have run, so an entity a
    // system flagged this frame is erased only once nothing is iterating.
    m_entityManager.update();
}

void Application::render()
{
    // sf::Color is not a literal type in SFML 2.6, so it cannot be constexpr.
    const sf::Color background{config::kBackgroundColorRed,
                               config::kBackgroundColorGreen,
                               config::kBackgroundColorBlue};

    m_window.clear(background);
    m_window.display();
}

} // namespace engine
