#pragma once

#include "engine/assets/AssetManager.hpp"
#include "engine/components/Animation.hpp"
#include "engine/ecs/System.hpp"

#include <cstdint>

namespace engine::ecs
{
class EntityManager;
}

namespace engine::input
{
class Input;
}

namespace engine::systems
{

/// Advances every animated entity by one game frame, and removes the ones whose
/// animation has finished playing.
///
/// ### What a game frame is, exactly
///
/// **One call to `AnimationSystem::update()` is one game frame.** That is the
/// whole definition, and it is the definition the course's `speed` needs.
///
/// The course says an animation's speed is "number of game frames between anim
/// frames". This engine has no fixed timestep - `Time` reports the *real* elapsed
/// time of each frame and every other system is handed `deltaSeconds` - so
/// "game frame" cannot mean a fixed slice of wall-clock time, because there is
/// none. What it can mean, and does, is *one turn of the main loop*.
///
/// So an animation with `speed` 8 changes frame on every eighth call to
/// `update()`, and that is true whether the machine is running at 144 FPS or 30.
/// The alternative - converting `speed` into seconds and integrating against
/// `deltaSeconds` - would make an animation's frame rate depend on the frame rate
/// limit, which is a rendering decision and has no business reaching into
/// gameplay. The number in the file is not reinterpreted anywhere.
///
/// ### What it does
///
/// For each entity holding a `components::Animation`:
///
/// 1. Resolve the name to an [assets::Animation](assets/Animation.hpp). The
///    definition is read, never written; the asset is shared and immutable.
/// 2. Advance the playback state by one game frame, using
///    [advanceAnimation] for the rule.
/// 3. If the animation has finished and does not repeat, destroy the entity.
///
/// ### Why it destroys entities
///
/// The course is explicit: "any entity with a non-repeating animation should be
/// destroyed once its Animation's hasEnded() returns true", and it gives the
/// example - a brick that turns into an explosion - where the entity is meant to
/// disappear. This is not speculative lifecycle machinery; the course asks for it
/// now, and `EntityManager::destroyEntity()` already exists and is safe to call
/// during iteration because it only sets a flag.
///
/// ### The final frame really is shown
///
/// Getting this right was the reason the advance rule is shaped the way it is.
/// The course's reference implementation updates, checks for the end, and destroys
/// all before rendering, so a non-repeating animation's last frame is selected and
/// the entity is removed in the same frame - the last frame is never drawn.
///
/// Here `advanceAnimation` only sets `ended` once the final frame has been on
/// screen for its full `speed` period. `Application` runs this system, then
/// performs the deferred erase, and only then renders, so an entity is always
/// removed *after* its last frame has been presented. There is no flag to turn
/// this off, because an effect whose final frame is skipped is not the effect the
/// course described.
///
/// ### Destroyed entities are not modified
///
/// The query skips entities already flagged dead, so an entity destroyed this
/// frame is not also advanced. An animation cannot outlive its entity by
/// accident.
///
/// ### The asset manager is required, by const reference
///
/// There is no overload without one and no "assets unavailable" state. A system
/// that could not resolve a name would have to decide at runtime what to do about
/// it, and every answer is wrong somewhere: skipping hides the problem and
/// failing mid-frame is a worse place to find out. `Application` owns the manager
/// and passes it here.
class AnimationSystem final : public engine::ecs::System
{
public:
    /// `assets` is referenced, not owned: the manager belongs to `Application`,
    /// which declares it before this system so it outlives it. It is only read, to
    /// resolve a name to a definition, and the interface it arrives through is
    /// const, so this system has no way to change what is loaded.
    explicit AnimationSystem(const assets::AssetManager& assets) noexcept : m_assets{assets} {}

    /// Advances every animated entity by one game frame.
    ///
    /// Takes `input` only because every system shares one signature; reading the
    /// keyboard is not something an animation does.
    void update(engine::ecs::EntityManager& entities, const input::ActionState& actions, float deltaSeconds) override;

    [[nodiscard]] const char* name() const override { return "AnimationSystem"; }

private:
    /// A reference, not a nullable pointer, because there is no valid empty state
    /// to represent: the manager is a construction requirement.
    const assets::AssetManager& m_assets;
};

/// Advances one entity's playback state by a single game frame.
///
/// This is the whole of the course's frame-advancement rule, as a free function
/// so it can be tested with no entity manager, no asset manager and no graphics
/// library. `AnimationSystem` is a thin wrapper around it.
///
/// ### The rule
///
/// With `speed` game frames per animation frame:
///
/// ```text
/// game frame 0   showing frame 0
/// game frame 1   showing frame 0
/// game frame 2   showing frame 1     (speed = 2)
/// game frame 3   showing frame 1
/// game frame 4   showing frame 2
/// ...
/// ```
///
/// So frame `n` is showing on game frames `n * speed` through
/// `(n + 1) * speed - 1`, and the first change happens on game frame `speed`.
/// `speed` 1 changes every game frame, which is what the course's one-frame
/// animations declare.
///
/// ### On reaching the end
///
/// A **repeating** animation wraps to frame 0 and is never ended.
///
/// A **non-repeating** animation stays on its final frame and sets `ended`, but
/// only once that final frame has been on screen for its full `speed` period.
/// The last frame is therefore genuinely shown before the entity goes, which is
/// what the course's explosion example needs and what the reference
/// implementation loses by destroying before it renders.
///
/// Once `ended` is set it stays set, and the state is not touched again, so an
/// ended animation is stable rather than oscillating.
///
/// ### Preconditions and what happens without them
///
/// @param state The entity's playback state. Only this is written.
/// @param frameCount The definition's frame count. A value of 0 leaves `state`
///        untouched: a definition with no frames has nothing to show, and
///        advancing would eventually index past it.
/// @param speed The definition's speed, in game frames. The parser guarantees at
///        least 1, so 0 is unreachable from a loaded asset; if it ever happened,
///        the state is left alone rather than advanced every game frame by
///        accident, because the alternative is dividing by zero.
void advanceAnimation(components::Animation& state, std::uint32_t frameCount, std::uint32_t speed) noexcept;

} // namespace engine::systems
