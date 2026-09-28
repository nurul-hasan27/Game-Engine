#include "engine/systems/AnimationSystem.hpp"

#include "engine/ecs/EntityManager.hpp"

namespace engine::systems
{

void advanceAnimation(components::Animation& state, const std::uint32_t frameCount, const std::uint32_t speed) noexcept
{
    // A definition with no frames, or a speed of zero, both leave the state exactly
    // as it was. Neither is reachable from a loaded asset - the parser refuses a
    // frame count or speed below 1 - and both are handled by doing nothing rather
    // than by advancing into an out-of-range index or dividing by zero.
    if (frameCount == 0U || speed == 0U)
    {
        return;
    }

    // An ended animation is finished. Advancing it again would either restart it
    // or walk the frame index off the end, and there is nothing to observe: the
    // entity is destroyed on the same frame this becomes true.
    if (state.ended)
    {
        return;
    }

    ++state.ticksOnFrame;

    // Still counting towards the next change. This is the whole of `speed`: the
    // frame is held for exactly `speed` game frames.
    if (state.ticksOnFrame < speed)
    {
        return;
    }

    state.ticksOnFrame = 0U;

    if (state.currentFrame + 1U < frameCount)
    {
        ++state.currentFrame;
        return;
    }

    // The final frame. A repeating animation wraps to the start and is never
    // ended, however many times it has been round - that is what separates it from
    // the reference implementation, which reports "finished" whenever the last
    // frame is on screen and would have a caller delete a running character.
    if (state.repeat)
    {
        state.currentFrame = 0U;
        return;
    }

    // Not repeating, and the final frame has now been held for its full period, so
    // it has actually been seen. Only now is the animation over.
    state.ended = true;
}

void AnimationSystem::update(engine::ecs::EntityManager& entities, const input::ActionState& actions,
                             const float deltaSeconds)
{
    // An animation is driven by game frames, not by elapsed time, and it does not
    // read the keyboard. Both parameters are accepted only so every system shares
    // one signature.
    static_cast<void>(actions);
    static_cast<void>(deltaSeconds);

    for (auto&& [entity, animation] : entities.query<components::Animation>())
    {
        // Resolving first means an entity naming an animation that does not exist
        // fails here, loudly, rather than advancing a frame index against a
        // definition nobody can see. The error propagates: a silently
        // non-animating entity is much harder to notice than a refusal to run.
        const assets::Animation& definition = m_assets.animation(animation.assetName);

        advanceAnimation(animation, definition.frameCount(), definition.speed());

        // Only a finished non-repeating animation is ever `ended`, so this needs no
        // repeat check of its own: the advance rule already drew that line.
        if (animation.ended)
        {
            // Deferred, so the iteration above is unaffected. `Application` runs
            // the manager's cleanup after this system and before rendering, which
            // is what makes the final frame visible first and the removal second.
            entities.destroyEntity(entity);
        }
    }
}

} // namespace engine::systems
