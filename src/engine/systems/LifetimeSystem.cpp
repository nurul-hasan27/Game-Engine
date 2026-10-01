#include "engine/systems/LifetimeSystem.hpp"

#include "engine/components/Lifetime.hpp"
#include "engine/ecs/EntityManager.hpp"
#include "engine/input/ActionState.hpp"

namespace engine::systems
{

void LifetimeSystem::update(engine::ecs::EntityManager& entities, const input::ActionState& actions,
                            const float deltaSeconds)
{
    // A lifetime is a count of game frames, and a game frame is one call to this function.
    // It reads no input and no elapsed time, and both parameters are accepted only so that
    // every system in the engine shares one signature.
    static_cast<void>(actions);
    static_cast<void>(deltaSeconds);

    for (auto&& [entity, lifetime] : entities.query<components::Lifetime>())
    {
        // Already expired, or spawned with no lifetime at all.
        //
        // The guard is what makes this idempotent. Without it, an entity whose count had
        // reached zero would ask to be destroyed again on the next frame - and
        // `destroyEntity` throws on an entity that is already dead rather than ignoring it,
        // so the second frame would crash rather than quietly do nothing.
        //
        // Nothing reaches this in practice, because the entity is destroyed on the frame the
        // count reaches zero and the manager erases it before the next frame runs. It is
        // here so that the rule is "expires once" rather than "expires once unless
        // something unusual happens first".
        if (lifetime.remainingFrames == 0U)
        {
            continue;
        }

        --lifetime.remainingFrames;

        // Exactly on reaching zero, and not before. `remainingFrames` counts the frame it is
        // on, so an entity with 30 spends frame one at 30, frame thirty at 1, and is
        // destroyed during the frame that takes it from 1 to 0.
        if (lifetime.remainingFrames == 0U)
        {
            // Deferred, so the walk above is unaffected: the entity stops appearing in
            // queries immediately and is erased by the owner at the end of the frame. The
            // scene - or a test - runs `EntityManager::update` after the systems, which is
            // what makes the final frame visible first and the removal second.
            entities.destroyEntity(entity);
        }
    }
}

} // namespace engine::systems