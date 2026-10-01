#pragma once

#include <cstdint>

namespace engine::components
{

/// How many more game frames this entity has before it is destroyed.
///
/// ### Frames, not seconds, and not a wall clock
///
/// The course gives a spawned entity a lifespan and measures it with an `sf::Clock` -
/// `CLifeSpan(1.8231)`, compared against `clock.getElapsedTime().asSeconds()`. That is
/// wall-clock time, which is the wrong quantity for anything gameplay-visible in this
/// engine, and it is wrong in a way that is easy to miss:
///
/// - A frame is capped at 0.1 s, so a stalled frame stretches real time while the
///   simulation barely advances. An entity could outlive its own animation, or expire
///   before it has been seen, depending on how the machine was behaving.
/// - The same number of frames must produce the same result on a 60 Hz laptop and a
///   144 Hz monitor, or a test would only pass on the machine that wrote it.
///
/// The course already measures **animation speed** in game frames rather than seconds -
/// `Animation <name> <texture> <frames> <speed>`, where `speed` is "how many game frames
/// pass between animation frames" - so frames are already the engine's unit for "how long
/// does this take to play". This component extends that to "how long does this entity
/// last", which is the same question. The course states the question block's coin as a
/// frame count too.
///
/// ### What one frame is
///
/// One call to [engine::systems::LifetimeSystem]'s `update`. Which is the same definition
/// [engine::components::Animation] already uses for its frame index, deliberately: an
/// entity's lifetime and its animation are both counted in the same currency, so "the
/// coin lasts thirty frames" and "the explosion lasts twelve frames of eight ticks" are
/// comparable statements.
///
/// ### Deliberately not a timer framework
///
/// There is one number here and no scheduling, no callbacks, no repeat and no
/// wall-clock conversion. [engine::systems::LifetimeSystem] counts it down and asks the
/// engine's own [engine::ecs::EntityManager] to destroy the entity when it reaches zero.
/// Anything richer would be a second scheduler for behaviour that does not exist yet.
struct Lifetime
{
    /// Game frames remaining, including the next one.
    ///
    /// `0` means "expired", and [engine::systems::LifetimeSystem] destroys an entity whose
    /// count has reached it. A freshly spawned entity with `30` therefore exists for
    /// exactly thirty frames and is gone on the thirty-first.
    std::uint32_t remainingFrames = 0U;
};

} // namespace engine::components