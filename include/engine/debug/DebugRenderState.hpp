#pragma once

namespace engine::debug
{

/// What a scene is currently drawing that the game itself does not ask for.
///
/// ### What this is
///
/// The course's Assignment 3 names three rendering toggles - *"Pressing the 'T' key
/// toggles drawing textures ... the 'C' key toggles drawing bounding boxes ... the
/// 'G' key toggles drawing of the grid"* - and one that is not about drawing at all,
/// the pause. This type is the three that are, gathered into one value, because
/// they have three things in common and nothing else does:
///
/// - They are **asked about at draw time**, so the answer has to reach the render
///   pass.
/// - They are **not gameplay**. Nothing about them changes what is simulated, and
///   no system may read them.
/// - They belong to **a scene**, not to the world. Two [engine::scene::PlayScene]
///   objects in one process each have their own, which is what makes "returning to
///   the menu and starting again gives a fresh game" true rather than approximately
///   true.
///
/// ### What this is not
///
/// Not a component, not a global, and not a static. All three were available and
/// all three are wrong here:
///
/// - A component would make the debug state part of the *world*, so a level's own
///   data would carry "this game has textures switched off", and the state would
///   need a system to write it - which is the "scattered debug conditionals in
///   gameplay systems" this exists to avoid.
/// - A global would make a second scene in the same process share it, and would make
///   the two scenes' tests depend on the order they run in.
/// - A `static` would be the same sharing with less visibility.
///
/// So it is a plain value, owned by the object whose render configuration it is, and
/// handed to the systems that read it. See
/// [engine::systems::DebugRenderSystem](engine/systems/DebugRenderSystem.hpp) and
/// [engine::systems::RenderSystem](engine/systems/RenderSystem.hpp).
///
/// ### Why pause is not here
///
/// `paused` is a property of the **simulation**, and this is a property of the
/// **picture**. Mixing them would put a gameplay fact in a rendering header and
/// invite a render system to reason about time. [engine::scene::PlayScene] keeps
/// `paused` beside its own systems, which is where the decision to stop them is
/// made.
///
/// ### Ownership
///
/// A plain aggregate: no methods, no globals, no singletons. Deliberately trivial,
/// so that "the flags are three bools and nothing else" is checked by the compiler
/// rather than by a comment - the test suite asserts the aggregate property at
/// compile time, exactly as it does for every other component.
struct DebugRenderState
{
    /// Whether entity artwork is drawn, which is what the course's `T` key toggles.
    ///
    /// **True by default**, and that is a decision rather than a default value: a
    /// build that has just started draws the game the way the game looks, and `T`
    /// switches *away* from it. A renderer that started with textures off would be a
    /// game nobody could see until they found the key.
    ///
    /// It suppresses **images only** - a [engine::components::Texture] and an
    /// animation frame, which are the same operation with a source region. It does
    /// not suppress rectangles, and it does not suppress text: a label is not an
    /// entity's texture, and the level's own spawn label is useful precisely when
    /// the artwork is gone.
    bool showTextures = true;

    /// Whether every entity carrying a [engine::components::Collider] is drawn a
    /// box the size of that collider, which is what the course's `C` key toggles.
    ///
    /// **False by default**: the game is meant to look like the game, and a world
    /// covered in magenta boxes is a diagnostic view rather than a picture.
    ///
    /// The box is derived from the collider and the transform rather than from a
    /// size the debug code chose, which is the whole of why it is honest: a
    /// 70x70 pipe shows a 70x70 box, a 40x60 player shows a 40x60 box, and an
    /// entity with no collider shows nothing at all rather than a made-up one.
    bool showBoundingBoxes = false;

    /// Whether the 64-pixel level grid is drawn, which is what the course's `G`
    /// key toggles.
    ///
    /// **False by default**, for the same reason: it is an overlay.
    ///
    /// Purely a picture. It is not collision geometry, it is not part of any
    /// entity, and no gameplay system reads it - see
    /// [engine::systems::DebugRenderSystem](engine/systems/DebugRenderSystem.hpp),
    /// which is the only thing that draws it.
    bool showGrid = false;
};

} // namespace engine::debug