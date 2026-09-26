#include "engine/Time.hpp"

#include <algorithm>

namespace engine
{

Time::Time() noexcept : m_previousFrame{Clock::now()}, m_deltaSeconds{0.0f}, m_elapsedSeconds{0.0f}, m_frameCount{0},
                          m_hasTicked{false}
{
}

void Time::tick()
{
    const Clock::time_point now = Clock::now();
    advance(now - m_previousFrame);
    m_previousFrame = now;
}

void Time::advance(const Duration sinceLastTick) noexcept
{
    // duration_cast to a float-based duration keeps the value in seconds, which
    // is the unit this whole class speaks.
    const float elapsed =
        std::chrono::duration_cast<std::chrono::duration<float>>(sinceLastTick).count();

    // Real time keeps accumulating even when the reported delta is clamped, so
    // elapsedSeconds() stays a truthful wall clock reading.
    m_elapsedSeconds += elapsed;
    ++m_frameCount;

    if (!m_hasTicked)
    {
        // The first frame after construction or reset has no "previous frame" to
        // compare against, so it reports nothing rather than the whole
        // construction time.
        m_deltaSeconds = 0.0f;
        m_hasTicked = true;
        return;
    }

    m_deltaSeconds = std::min(elapsed, kMaxDeltaSeconds);
}

void Time::reset() noexcept
{
    m_previousFrame = Clock::now();
    m_deltaSeconds = 0.0f;
    m_elapsedSeconds = 0.0f;
    m_frameCount = 0;
    m_hasTicked = false;
}

} // namespace engine
