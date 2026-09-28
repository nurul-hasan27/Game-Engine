#pragma once

#include <cstdint>
#include <string>

namespace engine::components
{

/// Which frame of an animation an entity is showing, and how that changes.
///
/// This is **pure data**, like every other component. It holds the *name* of an
/// animation asset and the per-entity playback state for it, and nothing else.
///
/// ### Why playback state is here and not on the asset
///
/// The course's reference implementation stores an `Animation` - including an
/// `sf::Sprite` and a mutable current frame - inside every entity's animation
/// component. That is how it gets per-entity playback, and it is exactly what
/// this engine's components-as-data rule exists to avoid: a component would own a
/// graphics object, and a copy of the same artwork would exist once per entity.
///
/// Here the asset is shared and immutable, and this component holds only what
/// differs between two entities playing it. Two entities naming
/// `mario_GoombaWalk_walk` share one loaded texture and one definition, and can
/// be on completely different frames at the same time.
///
/// ### `repeat` is here, and not in the asset file
///
/// The course's asset format is `Animation <name> <texture> <frames> <speed>`
/// and has no repeat field, and that is not an oversight: repeat is a property of
/// the *entity*, not of the artwork. A brick's explosion animation plays once and
/// the entity is then removed; the identical animation attached to a spinning coin
/// loops forever. Nothing about the four numbers in the file can say which, so
/// the field cannot be there, and putting it on the shared asset would make every
/// entity playing it agree.
///
/// ### One game frame is one `AnimationSystem::update()` call
///
/// `speed` is measured in **game frames**, as the course defines it, and this
/// engine never converts it to seconds. See
/// [engine::systems::advanceAnimation] for exactly what counts as one.
///
/// ### What this component does not hold
///
/// No texture, no source rectangle, no frame size and no asset manager. The size
/// belongs to the animation asset, the rectangle is derived from the asset and
/// the frame index at draw time, and the manager is a dependency of the systems
/// that use this data rather than something an entity carries. Holding any of
/// them here would either duplicate a fact that could then disagree with the asset
/// or drag a resource type into the ECS.
class Animation
{
public:
    /// Name of the `Animation` asset to play, exactly as declared in the
    /// configuration file.
    ///
    /// Note this is the name of an **animation**, not of a texture. The animation
    /// names its own texture, and the renderer follows that reference, so an
    /// entity never has to know which image it is drawn from.
    ///
    /// A name that is not declared is not silently ignored: the systems let the
    /// lookup's `AssetNotFoundError` propagate, because an entity that quietly
    /// draws nothing is far harder to notice than one that refuses to run.
    std::string assetName;

    /// Which frame is showing. Zero-based, and always less than the asset's frame
    /// count.
    ///
    /// Start at frame zero and never negative: the first frame is what a sprite
    /// shows before anything has been animated, so a one-frame animation and an
    /// unstarted one look the same rather than an entity being briefly invisible.
    std::uint32_t currentFrame = 0;

    /// Game frames spent on the current frame so far, always less than the asset's
    /// `speed`.
    ///
    /// Separate from `currentFrame` because "which frame" and "how long have we
    /// been on it" are different questions, and a single counter cannot answer
    /// both. A single counter is also what makes an animation's playback depend on
    /// how long it has been running rather than on when it last changed, which is
    /// what the course's `speed` means.
    std::uint32_t ticksOnFrame = 0;

    /// Whether playback wraps around forever, or plays once and stops.
    ///
    /// True is the default because a looping animation is the ordinary case - a
    /// running character, a spinning coin, a shimmering block. An effect that
    /// plays once has to say so.
    bool repeat = true;

    /// Whether a non-repeating animation has finished.
    ///
    /// Always false for a repeating one, forever, no matter how many times it has
    /// looped. That is the whole point of the field: the course's reference
    /// implementation reports "finished" whenever the last frame happens to be on
    /// screen, which for a looping animation is once per cycle, so a caller that
    /// destroyed on it would delete a running character. Here the two are separate
    /// questions and only this one is ever true.
    ///
    /// Once set it stays set, and the entity is destroyed by
    /// [engine::systems::AnimationSystem], so nothing observes it for long.
    bool ended = false;
};

} // namespace engine::components
