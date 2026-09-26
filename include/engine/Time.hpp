#pragma once

#include <chrono>
#include <cstdint>

namespace engine
{

/// Frame timing for the main loop.
///
/// This is the engine's only window onto the system clock, and its whole job is
/// to answer one question each frame: *how much real time passed since the
/// previous frame?* That answer, `deltaSeconds()`, is what every system is
/// given, so gameplay never touches std::chrono and never assumes a frame rate.
///
/// ### Units
///
/// Everything here is in **seconds**, as a `float`, matching the rest of the
/// engine's math. One frame at 60 FPS is about `0.0167`; `0.016f` is roughly
/// 16 milliseconds. Milliseconds are never exposed.
///
/// ### Variable frame delta
///
/// Systems receive the *real* elapsed time of the last frame. This is a
/// variable timestep: two machines running at different speeds will integrate
/// movement at the same real-world rate, but the arithmetic will differ slightly
/// between them. A fixed physics timestep may be introduced in a later physics
/// phase if simulation determinism turns out to matter; that is deliberately not
/// done here.
///
/// ### Clamping
///
/// A single delta is clamped to `kMaxDeltaSeconds` (0.1 s, i.e. 10 FPS). Without
/// it, a debugger pause, a minimised window, a breakpoint, or a stalled frame
/// would hand systems a multi-second delta and fling everything across the
/// screen, then cascade into further stalls. Clamping is a deliberate trade:
/// real elapsed time is still reported by `elapsedSeconds()`, but simulation is
/// protected from a single pathological frame.
///
/// ### Lifetime
///
/// A Time is a plain value with no global state and no singleton. Constructing
/// one starts the measurement. The first `tick()` after construction or `reset()`
/// reports a delta of exactly `0.0f`, so a slow first frame (level loading, say)
/// cannot jolt the simulation on frame one.
class Time
{
public:
    using Clock = std::chrono::steady_clock;
    using Duration = Clock::duration;

    /// The longest delta a system will ever be told about: 0.1 seconds, or a
    /// 10 FPS frame. Anything slower than that is reported as exactly this.
    static constexpr float kMaxDeltaSeconds = 0.1f;

    /// Starts measuring. The next tick() reports a delta of zero.
    Time() noexcept;

    /// Advances to the current instant and records the frame delta.
    void tick();

    /// Records a frame that lasted `sinceLastTick`.
    ///
    /// `tick()` is exactly this with a duration read from the clock. Exposing it
    /// separately is what makes the clamping and accounting rules testable
    /// without sleeping, and it is also the seam a fixed-step or recorded
    /// playback clock would drive in a later phase.
    void advance(const Duration sinceLastTick) noexcept;

    /// Clears the delta, elapsed time and frame count, and restarts measuring.
    /// The next tick() again reports a delta of zero.
    void reset() noexcept;

    /// Time the previous frame took, in seconds, clamped to kMaxDeltaSeconds.
    [[nodiscard]] float deltaSeconds() const noexcept { return m_deltaSeconds; }

    /// Real time since construction or the last reset(), in seconds.
    ///
    /// This accumulates the *unclamped* durations, so it keeps tracking wall
    /// clock time even while the reported delta is being clamped. It can
    /// therefore be greater than the sum of the deltas systems were given.
    [[nodiscard]] float elapsedSeconds() const noexcept { return m_elapsedSeconds; }

    /// How many frames have been advanced since construction or the last reset.
    [[nodiscard]] std::uint64_t frameCount() const noexcept { return m_frameCount; }

private:
    Clock::time_point m_previousFrame{};
    float m_deltaSeconds = 0.0f;
    float m_elapsedSeconds = 0.0f;
    std::uint64_t m_frameCount = 0;
    bool m_hasTicked = false;
};

} // namespace engine
