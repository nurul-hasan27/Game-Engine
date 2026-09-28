#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::input
{

/// A gameplay-meaningful intent: something the game can ask about without knowing
/// which key produced it.
///
/// ### The distinction this type exists for
///
/// The course separates three things that are easy to conflate:
///
/// ```text
/// physical input      a key went down
///       ↓
/// action              "move left"
///       ↓
/// gameplay            set this entity's velocity
/// ```
///
/// A player does not press `A` because pressing `A` is the goal. They press it to
/// move left. Gameplay code should therefore ask about [Action] and never about
/// [Key](Input.hpp), and this enum is the vocabulary it asks in.
///
/// [Key](Input.hpp) stays the engine's name for a physical key and
/// [ActionMap](ActionMap.hpp) is what translates one into the other. That
/// translation is the *only* place the two vocabularies meet, which is what makes
/// the physical mapping replaceable: a controller, a replay file or a rebinding
/// screen changes the map and nothing else in the engine notices.
///
/// ### What is here, and why
///
/// The set is deliberately small and every entry has a consumer the repository
/// can point at:
///
/// | Action | Consumer | Bound to |
/// | ------ | -------- | -------- |
/// | `MoveLeft` / `MoveRight` / `MoveUp` / `MoveDown` | `MovementSystem` | A/D, arrow keys, W/S |
/// | `Jump` | Assignment 3's player, Phase 16 | W |
/// | `Shoot` | Assignment 3's player, Phase 18 | Space |
/// | `Pause` | Assignment 3's P key | P |
/// | `ToggleTextures` | Assignment 3's T key | T |
/// | `ToggleBoundingBoxes` | Assignment 3's C key | C |
/// | `ToggleGrid` | Assignment 3's G key | G |
/// | `ZoomIn` / `ZoomOut` | the camera demo's zoom system | X / Z |
/// | `Quit` | the game's Escape policy, Phase 15 | Escape |
///
/// `Jump`, `Shoot`, `Pause` and `Quit` are Assignment 3's specification, quoted:
/// *"Left: A key, Right: D key, Jump: W Key, Shoot: Space Key"* and *"The 'P'
/// key should pause the game ... Pressing the 'T' key toggles drawing textures
/// ... the 'C' key toggles drawing bounding boxes ... the 'G' key toggles
/// drawing of the grid ... The 'ESC' key should go 'back' to the Main Menu, or
/// quit if on the Main Menu"*. They exist with no consumer yet on purpose: the
/// assignment is the specification, and an action with no consumer today is
/// cheaper than retrofitting one in later.
///
/// `MoveUp`, `MoveDown`, `ZoomIn` and `ZoomOut` are not in that quote, and they are
/// here anyway because the repository already has live consumers that would
/// otherwise keep reading physical keys - which is the exact thing this phase
/// removes. See [ActionMap](ActionMap.hpp) for the W-key collision that makes one
/// key drive two actions.
///
/// ### No SFML
///
/// This header mentions no graphics or window type, and neither does
/// [ActionState](ActionState.hpp). The action vocabulary is engine language, and
/// the only place a physical key appears is the binding table in
/// [ActionMap](ActionMap.hpp), which is exactly where the translation belongs.
enum class Action : std::uint8_t
{
    /// Steer towards the left edge of the screen. `MoveRight` and this are the
    /// horizontal pair, and are the Assignment 3 movement controls.
    MoveLeft,

    /// Steer towards the right edge of the screen.
    MoveRight,

    /// Steer towards the top of the screen. Not an Assignment 3 control - that
    /// game is a platformer with no downward movement - but the camera demo has
    /// always moved in four directions and this is what it now asks for.
    MoveUp,

    /// Steer towards the bottom of the screen. See [MoveUp](Action.hpp).
    MoveDown,

    /// Leave the ground. Assignment 3: *"Jump: W Key"*.
    ///
    /// Held, because the assignment requires a variable-height jump: holding the
    /// key rises further than tapping it. Whether a jump is *allowed* from the
    /// ground is gameplay, and belongs to the player, not here.
    Jump,

    /// Fire forwards. Assignment 3: *"Shoot: Space Key"*.
    ///
    /// The assignment is explicit that holding the key must not produce
    /// uncontrolled repeated shots, so gameplay must ask
    /// [wasPressed](ActionState.hpp) rather than [isActive](ActionState.hpp). The
    /// distinction is available; using it correctly is Phase 18's job.
    Shoot,

    /// Stop and resume the game. Assignment 3's P key.
    ///
    /// A per-press action, so gameplay wants
    /// [wasPressed](ActionState.hpp): holding P must not toggle pause every frame.
    Pause,

    /// Show or hide entity textures. Assignment 3's T key.
    ToggleTextures,

    /// Show or hide entity bounding boxes. Assignment 3's C key.
    ToggleBoundingBoxes,

    /// Show or hide the grid. Assignment 3's G key.
    ToggleGrid,

    /// Increase the camera zoom. The camera demo, not the assignment.
    ZoomIn,

    /// Decrease the camera zoom. The camera demo, not the assignment.
    ZoomOut,

    /// Leave the current level or quit the game. Assignment 3's Escape key.
    ///
    /// Deliberately **not** acted on by [engine::Application](engine/Application.hpp).
    /// What Escape means - back to the menu, or quit - is a policy the game makes,
    /// and `Application` already documents that a keyboard Escape is never turned
    /// into a window close behind the game's back. This action exists so the game
    /// can be told about it, and only the game may.
    Quit,

    /// Sentinel used to size the state tables. Not an action.
    Count
};

/// Number of entries in the action state tables. Excludes `Count`.
inline constexpr std::size_t kActionCount = static_cast<std::size_t>(Action::Count);

/// Maps an action to its slot. Action values are contiguous by construction.
[[nodiscard]] constexpr std::size_t toIndex(const Action action) noexcept
{
    return static_cast<std::size_t>(action);
}

/// True if an action is one the engine actually knows.
///
/// `Action::Count` is deliberately not a valid action: it is a table size, and
/// returning a real slot for it would let a loop bound read one past the end of
/// the state arrays.
[[nodiscard]] constexpr bool isValid(const Action action) noexcept
{
    return toIndex(action) < kActionCount;
}

} // namespace engine::input
