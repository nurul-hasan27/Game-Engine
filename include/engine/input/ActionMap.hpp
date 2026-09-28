#pragma once

#include "engine/input/Action.hpp"
#include "engine/input/Input.hpp"

#include <array>
#include <cstddef>

namespace engine::input
{

/// The most physical keys one action may be bound to.
///
/// Two, because that is what [defaultActionMap] needs: a movement action is
/// reachable from a letter *and* an arrow key, and nothing needs more today.
///
/// It is a hard limit rather than a dynamic list so that the whole map is a fixed
/// number of bytes, allocation-free, and safe to read while a system reads it. A
/// container that can grow would invalidate that. Raising it is a deliberate
/// decision - it changes the size of every `ActionMap` - not an accident of
/// adding a call site.
inline constexpr std::size_t kMaxBindingsPerAction = 2;

/// Which physical keys drive which actions.
///
/// ### The whole point of the class
///
/// This is the *only* place in the engine where an [Action] and a [Key] are both
/// named. Everything above it - every system, every component, every test of
/// gameplay behaviour - works in actions, and everything below it is the window's
/// problem. So the physical mapping is one table, and replacing it replaces exactly
/// one thing.
///
/// ### An action can be bound to more than one key
///
/// A movement action is reachable from a letter and from an arrow key, so "is the
/// player moving left?" is a question about *either* key. The map answers
/// "which keys", and the state layer reports the action active if any of them is
/// held. A consumer cannot tell which key the player actually used, which is the
/// point: the arrow keys are a convenience, not a second meaning.
///
/// ### One key may drive more than one action
///
/// That is deliberate, and it is forced by the repository rather than chosen for
/// elegance.
///
/// Assignment 3 specifies *"Jump: W Key"*, so `W` must drive `Jump`. The camera
/// demo has always moved up on `W`, and there is a test asserting that holding `W`
/// moves the body 100 pixels up. Both cannot be true under a one-key-one-action
/// rule: honouring the assignment would silently break shipped behaviour, and
/// keeping the demo would move the assignment's jump key.
///
/// So `W` drives both `Jump` and `MoveUp`, and the two are separate questions
/// about the same key. A platformer decides that `W` means jump and that an
/// airborne `W` is not movement; a top-down game decides it means up. Neither the
/// map nor any system has to know which, because the map only reports that `W` is
/// held and each consumer asks for the action it cares about.
///
/// The alternative - rejecting a second action on an already-bound key - would
/// have forced one of the two requirements to be dropped, and would have turned a
/// legitimate design question into a startup crash.
///
/// ### What this class deliberately cannot do
///
/// It holds no state beyond the table, executes nothing, knows nothing about the
/// ECS, and has no notion of a player. It answers one question: *which keys drive
/// this action?* A manager, a store, or an entity cannot be reached from here.
///
/// ### Ownership
///
/// A plain value, like [Input](Input.hpp): no singleton, no global, no static
/// table. [engine::Application](engine/Application.hpp) owns one and hands it out
/// by const reference.
class ActionMap
{
public:
    /// A map with nothing bound.
    ///
    /// Every action reports [Key::Unknown](Input.hpp), which is untrackable, so an
    /// unbound action is permanently inactive rather than an error. A map is
    /// therefore always usable, and a forgotten binding degrades to "that action
    /// never fires" instead of crashing or reading a neighbouring key.
    ActionMap() noexcept = default;

    /// Adds a binding of `action` to `key`.
    ///
    /// This is the operation the course calls *registering an action*
    /// (`registerAction` in the Assignment 3 reference, whose note is *"Remember
    /// that you must use registerAction to register a new action for the scene"*).
    /// The name here is `registerAction` for the same reason.
    ///
    /// ### Adding, not replacing
    ///
    /// The two-argument form **adds** a binding rather than replacing the action's
    /// existing ones, because that is what "register this action as also being
    /// driven by this key" has to mean for the arrow keys. Registering the same
    /// key twice is a no-op rather than an error, so a map built twice by two
    /// pieces of code stays correct and its size bounded.
    ///
    /// Adding a third key to an action that already has
    /// [kMaxBindingsPerAction] is ignored, because the table cannot grow. That is
    /// a bound, not a failure: the action still works, through the keys it has.
    void registerAction(const Action action, const Key key) noexcept;

    /// Registers `action` as driven by two keys at once.
    ///
    /// Exactly equivalent to calling the one-key form twice, and exists because
    /// that is how every movement action in [defaultActionMap] is built, and
    /// writing it out twice at each site would invite the two calls to drift.
    void registerAction(const Action action, const Key first, const Key second) noexcept;

    /// Removes every binding of `action`, leaving it unbound.
    ///
    /// Unbinding is expressed as a count of zero rather than by writing
    /// [Key::Unknown], so "this action has no key" has exactly one spelling and a
    /// partially-filled table can never be mistaken for a deliberate `Unknown`.
    void unregisterAction(const Action action) noexcept;

    /// How many keys drive `action`: 0, 1 or [kMaxBindingsPerAction].
    [[nodiscard]] std::size_t bindingCount(const Action action) const noexcept;

    /// The `index`th key driving `action`, or [Key::Unknown](Input.hpp) when
    /// `index` is past the end or `action` is not a real action.
    ///
    /// Order is the order the bindings were registered, so `keyAt(action, 0)` is
    /// the primary binding. Nothing depends on that ordering today; it exists so a
    /// caller can name "the" key for an action without assuming.
    [[nodiscard]] Key keyAt(const Action action, const std::size_t index) const noexcept;

    /// The first key driving `action`, or [Key::Unknown](Input.hpp) if unbound.
    [[nodiscard]] Key primaryKeyOf(const Action action) const noexcept
    {
        return keyAt(action, 0U);
    }

    /// True when at least one key driving `action` is one the engine tracks.
    ///
    /// This is the check that matters for deciding whether the action can ever
    /// fire. It is not the same as "has any binding", because a binding could name
    /// a key the engine does not track.
    [[nodiscard]] bool isBound(const Action action) const noexcept;

    /// True when *any* action has at least one trackable binding.
    [[nodiscard]] bool hasAnyBinding() const noexcept;

    /// Unbinds every action. A map with nothing bound is again.
    void clear() noexcept;

    /// A map with Assignment 3's controls bound, ready to use.
    ///
    /// This is the composition root's job, not gameplay's: [engine::Application]
    /// builds one of these so the default mapping exists in exactly one place and
    /// no system has to know a key code. See [defaultActionMap] for the table.
    [[nodiscard]] static ActionMap withDefaultBindings() noexcept;

private:
    /// The keys for one action, plus how many of them are real.
    ///
    /// A count rather than a sentinel-terminated list, so a `Key::Unknown` can
    /// never be read as a binding and a full table is still unambiguous.
    struct Bindings
    {
        std::array<Key, kMaxBindingsPerAction> keys{};
        std::size_t count = 0;
    };

    /// Indexed by [toIndex](Action.hpp), so every lookup is a table read with no
    /// allocation, no map, and nothing that can rehash. A fixed table also means
    /// bindings can be read while a system reads them, which a growing container
    /// could not promise.
    std::array<Bindings, kActionCount> m_bindings{};
};

/// The engine's default action bindings, as one value.
///
/// | Action | Keys | Where it comes from |
/// | ------ | ---- | ------------------- |
/// | `MoveLeft` | `A`, `Left` | Assignment 3 controls, plus the demo's arrows |
/// | `MoveRight` | `D`, `Right` | as above |
/// | `MoveUp` | `W`, `Up` | the demo; see the note below |
/// | `MoveDown` | `S`, `Down` | the demo |
/// | `Jump` | `W` | Assignment 3: *"Jump: W Key"* |
/// | `Shoot` | `Space` | Assignment 3: *"Shoot: Space Key"* |
/// | `Pause` | `P` | Assignment 3: *"The 'P' key should pause the game"* |
/// | `ToggleTextures` | `T` | Assignment 3: *"the 'T' key toggles drawing textures"* |
/// | `ToggleBoundingBoxes` | `C` | Assignment 3: *"the 'C' key toggles drawing bounding boxes"* |
/// | `ToggleGrid` | `G` | Assignment 3: *"the 'G' key toggles drawing of the grid"* |
/// | `ZoomIn` | `X` | the camera demo |
/// | `ZoomOut` | `Z` | the camera demo |
/// | `Quit` | `Escape` | Assignment 3: *"The 'ESC' key should go 'back' ... or quit"* |
///
/// ### Why four actions have a second key
///
/// `MoveLeft`/`MoveRight`/`MoveUp`/`MoveDown` each get an arrow key so the camera
/// demo's controls keep working. The arrows are not in Assignment 3's list, and
/// adding them is a convenience rather than a requirement - so they are a *second
/// binding for the same action*, never a second action, and a consumer cannot tell
/// which key the player used.
///
/// ### Why `W` drives two actions
///
/// `W` is `Jump` because Assignment 3 says so, and `W` is also `MoveUp` because
/// the camera demo has always moved up on `W` and a test asserts it. Both are
/// kept rather than one being dropped; see the class documentation for why.
[[nodiscard]] ActionMap defaultActionMap() noexcept;

} // namespace engine::input
