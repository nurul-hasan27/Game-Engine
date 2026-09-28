#pragma once

#include "engine/input/Action.hpp"
#include "engine/input/ActionMap.hpp"
#include "engine/input/Input.hpp"

#include <array>
#include <cstddef>

namespace engine::input
{

/// What the actions are doing this frame.
///
/// ### A snapshot, not a log
///
/// `update()` **overwrites** every field from the [ActionMap] and the [Input], so
/// the state is always exactly what the current frame says and can never hold a
/// value from a frame that has gone. There is deliberately no incremental
/// "processActionDown" path and no `beginFrame()`.
///
/// That is a different shape from [Input](Input.hpp), which is incremental, and
/// the difference is deliberate:
///
/// - [Input](Input.hpp) is fed by the operating system, which sends *transitions*
///   and repeats, so it has to track held state itself and swallow the repeats.
/// - `ActionState` is *derived*. The held state already exists in
///   [Input](Input.hpp) by the time this runs, so there is nothing to accumulate
///   and nothing that can drift.
///
/// The payoff is that `update()` is idempotent. Calling it twice with the same map
/// and input gives the same state, calling it with a *later* input overwrites
/// cleanly, and there is no ordering requirement between clearing and filling -
/// so the frame cannot be left half-updated by a mistake.
///
/// ### An action with several keys
///
/// An action may be bound to more than one key, and is **active if any of them is
/// held**, **pressed if any of them transitioned down this frame**, and
/// released if any transitioned up. A consumer cannot tell which key produced
/// it, which is the whole point of binding two keys to one action.
///
/// ### Ownership
///
/// A plain value, like [Input](Input.hpp) and [ActionMap](ActionMap.hpp): no
/// singleton, no global, no static state. [engine::Application] owns one, updates
/// it once per frame, and hands systems a const reference to it.
///
/// This header has no SFML, so a gameplay system can include it and be compiled
/// with no graphics library in reach.
class ActionState
{
public:
    /// Recomputes every action from `map` and `input`.
    ///
    /// Called exactly once per game frame, by the composition root, after events
    /// have been recorded and before any system runs. See
    /// [engine::Application] for the exact position in the frame.
    ///
    /// An action bound to no trackable key becomes inactive rather than an error:
    /// `update` never throws and never reads out of range, whatever the map holds.
    void update(const ActionMap& map, const Input& input) noexcept;

    /// Clears every action, as if nothing were held.
    ///
    /// Useful before the first `update()`, and for a test that wants an all-false
    /// state without constructing a map. A default-constructed `ActionState` is
    /// already all-false, so this is only needed to return to that state.
    void reset() noexcept;

    /// True while the action is being held, this frame and every frame until
    /// released.
    ///
    /// This is the question a movement system asks: *"is the player still asking
    /// to go left?"*
    [[nodiscard]] bool isActive(const Action action) const noexcept
    {
        return isValid(action) && m_active[toIndex(action)];
    }

    /// True only on the frame the action transitioned from inactive to active.
    ///
    /// This is the question a per-press action asks: *"did the player just tap
    /// this?"* It stays false for as long as the key is held, which is what stops
    /// a held key from repeating the action every frame.
    ///
    /// Assignment 3 needs exactly this for shooting: *"If the jump key is held,
    /// the player should not continuously jump, but instead it should only jump
    /// once per button press."* Whether to use it for a given action is the
    /// consumer's decision, not this class's.
    [[nodiscard]] bool wasPressed(const Action action) const noexcept
    {
        return isValid(action) && m_pressed[toIndex(action)];
    }

    /// True only on the frame the action transitioned from active to inactive.
    ///
    /// The same edge question on the other side. Assignment 3's variable jump
    /// height needs it: *"If the player lets go of the jump key mid-jump, it
    /// should start falling back down immediately."*
    [[nodiscard]] bool wasReleased(const Action action) const noexcept
    {
        return isValid(action) && m_released[toIndex(action)];
    }

    /// True if any action is currently held.
    ///
    /// Cheap way to skip action-driven work when the player is touching nothing,
    /// and the action-layer counterpart of [Input::anyKeyDown](Input.hpp).
    [[nodiscard]] bool anyActionActive() const noexcept;

private:
    std::array<bool, kActionCount> m_active{};
    std::array<bool, kActionCount> m_pressed{};
    std::array<bool, kActionCount> m_released{};
};

} // namespace engine::input
