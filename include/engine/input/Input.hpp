#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace engine::input
{

/// An engine-level physical key.
///
/// Gameplay code names keys with this enum, never with `sf::Keyboard::Key`, so
/// no SFML type reaches a system. The enum is deliberately small: it holds the
/// keys this phase needs and nothing more, because a full table would be a
/// mapping nobody has yet been asked to maintain.
///
/// `Unknown` stands for a physical key the engine does not track. The SFML
/// adapter maps anything it does not recognise to `Unknown`, and `Unknown` is
/// never recorded as down, so an unmapped key is inert rather than an error.
enum class Key : std::uint8_t
{
    Unknown = 0,
    A,
    D,
    S,
    W,
    Left,
    Right,
    Up,
    Down,
    Space,
    Escape,

    /// Sentinel used to size the state tables. Not a key.
    Count
};

/// Number of entries in the key state tables, including `Unknown`.
inline constexpr std::size_t kKeyCount = static_cast<std::size_t>(Key::Count);

/// Maps a key to its slot. Key values are contiguous by construction.
[[nodiscard]] constexpr std::size_t toIndex(const Key key) noexcept
{
    return static_cast<std::size_t>(key);
}

/// True if a key is one the engine actually tracks.
///
/// `Key::Unknown` is deliberately untrackable: the SFML adapter maps every key
/// it does not recognise to `Unknown`, so an unmapped key has to be inert. If
/// `Unknown` behaved like a normal slot, holding a key the engine has never
/// heard of would report `anyKeyDown() == true`, which is a lie.
[[nodiscard]] constexpr bool isTrackable(const Key key) noexcept
{
    return key != Key::Unknown && toIndex(key) < kKeyCount;
}

/// Keyboard state for the current frame.
///
/// ### Three questions, three different meanings
///
/// | Query | True when |
/// | ----- | --------- |
/// | `isKeyDown(k)` | the key is held right now, this frame and every frame until released |
/// | `isKeyPressed(k)` | the key transitioned from up to down **on this frame only** |
/// | `isKeyReleased(k)` | the key transitioned from down to up **on this frame only** |
///
/// Held, pressed and released are genuinely different questions. "Is the player
/// holding right?" is `isKeyDown`. "Did the player just tap right?" is
/// `isKeyPressed`, which stays false for as long as the key is held so a single
/// long press does not fire an action every frame.
///
/// ### Frame-local transients
///
/// `isKeyPressed` and `isKeyReleased` describe an edge, so they are true for
/// exactly one frame. `beginFrame()` clears them, and it must run **before**
/// events are processed, so an event seen during this frame is still observable
/// by systems later in the same frame. The lifecycle is:
///
/// ```text
/// input.beginFrame()   clear pressed and released
/// process events       record transitions
/// systems run          read down, pressed and released
/// ```
///
/// ### Key repeat
///
/// A held key makes the operating system emit repeated press events. Those are
/// deliberately swallowed: `processKeyDown` on a key that is already down is a
/// no-op, so `isKeyPressed` stays true only for the real physical transition.
/// Without that, a key held for a second would report "pressed" on every frame.
///
/// ### Ownership
///
/// A plain value with no singleton, no global and no static state. `Application`
/// owns one, and systems receive a reference. Nothing reaches for a global
/// keyboard.
///
/// This header has no dependency beyond the standard library, and in particular
/// no SFML: feeding it is done through `processKeyDown` and `processKeyUp`,
/// which any caller can drive, which is also how the tests exercise the real
/// state machine without a keyboard.
class Input
{
public:
    /// Starts a new frame by clearing the frame-local transients.
    ///
    /// `isKeyDown` is untouched: a key held across frames stays held.
    void beginFrame() noexcept
    {
        m_pressed.fill(false);
        m_released.fill(false);
    }

    /// Records that `key` has physically gone down.
    ///
    /// Sets the down flag, and sets pressed only if the key was not already
    /// down. A repeated event for a held key, which is what the operating
    /// system sends while a key is held, therefore does not retrigger pressed.
    void processKeyDown(const Key key) noexcept
    {
        if (!isTrackable(key))
        {
            return;
        }

        const std::size_t index = toIndex(key);

        if (m_down[index])
        {
            return;
        }

        m_down[index] = true;
        m_pressed[index] = true;
    }

    /// Records that `key` has physically come up.
    ///
    /// Clears the down flag, and sets released only if the key was down. A
    /// spurious release for a key that was not held is ignored.
    void processKeyUp(const Key key) noexcept
    {
        if (!isTrackable(key))
        {
            return;
        }

        const std::size_t index = toIndex(key);

        if (!m_down[index])
        {
            return;
        }

        m_down[index] = false;
        m_released[index] = true;
    }

    /// Clears every key, as if nothing were held and no frame were in progress.
    void reset() noexcept
    {
        m_down.fill(false);
        m_pressed.fill(false);
        m_released.fill(false);
    }

    [[nodiscard]] bool isKeyDown(const Key key) const noexcept
    {
        return isTrackable(key) && m_down[toIndex(key)];
    }

    [[nodiscard]] bool isKeyPressed(const Key key) const noexcept
    {
        return isTrackable(key) && m_pressed[toIndex(key)];
    }

    [[nodiscard]] bool isKeyReleased(const Key key) const noexcept
    {
        return isTrackable(key) && m_released[toIndex(key)];
    }

    /// True if any tracked key is currently held. Cheap way to skip input-driven
    /// work when the player is touching nothing.
    [[nodiscard]] bool anyKeyDown() const noexcept
    {
        for (bool down : m_down)
        {
            if (down)
            {
                return true;
            }
        }

        return false;
    }

private:
    std::array<bool, kKeyCount> m_down{};
    std::array<bool, kKeyCount> m_pressed{};
    std::array<bool, kKeyCount> m_released{};
};

} // namespace engine::input
