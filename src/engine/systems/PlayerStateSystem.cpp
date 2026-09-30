#include "engine/systems/PlayerStateSystem.hpp"

#include "engine/components/Animation.hpp"
#include "engine/components/Player.hpp"
#include "engine/components/Transform.hpp"
#include "engine/ecs/EntityManager.hpp"
#include "engine/input/ActionState.hpp"
#include "engine/math/Vec2.hpp"
#include "engine/physics/Collision.hpp"

#include <cmath>
#include <string>
#include <string_view>

namespace engine::systems
{

namespace
{

using engine::Vec2;
using engine::components::Animation;
using engine::components::Player;
using engine::components::PlayerState;
using engine::components::Transform;
using engine::ecs::Entity;
using engine::ecs::EntityManager;
using engine::input::ActionState;
using engine::physics::Collision;
using engine::physics::landedOn;

// ---------------------------------------------------------------------------
// The player's animations.
//
// ### Which artwork exists, and which does not
//
// The course names three: Stand, Run and Air. Two of the three textures in the
// committed library are single-frame and can be declared honestly:
//
//   megaman_megaStand   190 x 208   1 frame
//   megaman_megaJump    279 x 266   1 frame
//
// The third, `megaman_megaRun`, **cannot be declared at all**, and that is not an
// oversight this phase is choosing to make. It is 733 pixels wide and the course
// declares it as 3 frames, so 733 / 3 is 244.33. 733 is prime, so the only frame
// counts that divide it are 1 and 733. This engine's standing rule - decided in the
// asset phase and written into `assets/assets.txt` - is that a frame count which
// does not divide the texture width exactly is **rejected, not rounded**, and
// `megaman_megaRun` is deliberately absent from the animation declarations for
// exactly that reason. Rounding to 244 would misregister each of the three run
// sprites by 2 to 6 pixels and silently discard the last column of the strip.
//
// So the Run **state** is fully implemented and fully observable, and the Run
// *artwork* is the standing sprite. The two are separated deliberately: the state
// machine, the speed, the facing and the jump are gameplay and they are all real;
// the picture is an asset problem, and the honest fixes for it are re-authoring the
// strip or adding a per-frame offset field to the animation format. Both are
// changes to the animation core, which is not this phase's to make.
//
// Declaring the 733-pixel strip as a *single* frame was considered and rejected: it
// satisfies "three distinct animation names" while drawing three run sprites side by
// side, centred on a 40-pixel-wide player. That is a visibly broken picture
// introduced to satisfy a naming requirement, which is worse than an honest
// limitation.
// ---------------------------------------------------------------------------

/// The standing animation, declared in `assets/assets.txt`.
constexpr std::string_view kStandAnimation = "megaman_megaStand_stand";

/// The airborne animation, declared in `assets/assets.txt`. Named "air" rather than
/// "jump" because the course calls the state Air, and the state is what selects it -
/// the player is also in the air while falling, which is not a jump.
constexpr std::string_view kAirAnimation = "megaman_megaJump_air";

/// The animation the player uses while running.
///
/// The standing animation, and the reason is the block comment above: the run strip
/// cannot be loaded. It is named separately so that the one place that would have to
/// change when the artwork is fixed is a single named constant, and so that a reader
/// who greps for the run animation finds this explanation rather than a bare reuse.
constexpr std::string_view kRunAnimation = kStandAnimation;

/// Selects the animation for the state, resetting the frame if the animation changed.
///
/// ### Why the reset is not optional
///
/// [engine::components::Animation] carries `currentFrame`, `ticksOnFrame` and
/// `ended` alongside the name, and all three belong to the *previous* animation.
/// Carrying them across a change would start the new animation on whatever frame
/// the old one happened to be on, and - worse - would carry `ended` across with them,
/// so an animation that had finished would be immediately `ended` in its new
/// incarnation and [engine::systems::AnimationSystem] would destroy the player on
/// the frame the state changed. The reset is three fields and it is the difference
/// between "the player switches picture" and "the player vanishes".
///
/// `repeat` is set on every change, not only when it differs, because the player's
/// animations loop forever and a carried-over `false` from any other animation would
/// end the player's own.
void selectAnimation(Animation& animation, const std::string_view wanted) noexcept
{
    if (animation.assetName == wanted)
    {
        animation.repeat = true;
        return;
    }

    animation.assetName = wanted;
    animation.currentFrame = 0U;
    animation.ticksOnFrame = 0U;
    animation.repeat = true;
    animation.ended = false;
}

/// The animation for a state.
[[nodiscard]] constexpr std::string_view animationFor(const PlayerState state) noexcept
{
    switch (state)
    {
        case PlayerState::Stand:
            return kStandAnimation;
        case PlayerState::Run:
            return kRunAnimation;
        case PlayerState::Air:
            return kAirAnimation;
    }

    return kStandAnimation;
}


} // namespace

void PlayerStateSystem::update(EntityManager& entities, const ActionState& actions, const float deltaSeconds)
{
    // Nothing here reads input or a frame length. The system's whole input is the
    // report, and reading the parameters would only invite somebody to reintroduce the
    // ordering dependency this split exists to remove.
    static_cast<void>(actions);
    static_cast<void>(deltaSeconds);

    for (auto&& [entity, transform, player, animation] : entities.query<Transform, Player, Animation>())
    {
        // ---- Is anything holding this player up? ------------------------------
        //
        // One definition, from this frame's own report: a support collision was
        // resolved. [engine::physics::landedOn] is the whole test - it requires the
        // player to have been beside the partner and *not* vertically overlapping it,
        // to be penetrating it now, to have been pushed upward, and for the partner to
        // be static.
        //
        // Those five clauses are what keep a wall from reading as a floor. A player
        // pressed against a wall is already inside the wall's vertical extent, so its
        // previous vertical overlap was positive and the landing test is refused -
        // which is the case Phase 16 could not express at all, and the reason it had to
        // probe for a floor instead of asking.
        //
        // Every collision this player took part in is examined, not just the first and
        // not "is there anything nearby". A player landing on a floor while brushing a
        // wall has two records; the floor one answers the question and the wall one is
        // correctly ignored.
        bool supported = false;
        for (const Collision& collision : m_collisions->collisions())
        {
            if (landedOn(collision, entity.id()))
            {
                supported = true;
                break;
            }
        }

        player.grounded = supported;

        // A landing ends the jump, here, where the landing is actually known. Clearing
        // it in [PlayerSystem] instead would mean clearing it from the *previous*
        // frame's support, which is the lag this phase removed.
        if (player.grounded)
        {
            player.jumping = false;
        }

        // ---- Which of the course's three states is the player in? --------------
        //
        // Air wins over grounded, and the reason is [Player]'s `jumping` flag rather
        // than a negation of `grounded`. On the frame a jump starts, physics has
        // already carried the player up and out of the tile, so `grounded` is false by
        // then; the flag is what makes the launch frame count as airborne even if a
        // low ceiling or a neighbouring box leaves a record behind.
        //
        // Run is "moving horizontally", read from the velocity rather than from the
        // actions, and read *after* physics so that a player stopped dead against a
        // wall is standing rather than running. It is the post-resolution velocity
        // because that is the one that describes the frame.
        const bool moving = std::fabs(transform.velocity.x) > 0.0F;
        const bool airborne = !player.grounded || player.jumping;
        player.state = airborne ? PlayerState::Air : (moving ? PlayerState::Run : PlayerState::Stand);

        // ---- Which picture goes with it? --------------------------------------
        //
        // Selection only. [engine::systems::AnimationSystem] owns the frame index and
        // runs after this system, so the animation chosen here is the one advanced
        // this frame - a state change is visible on the frame it happens rather than
        // the frame after, which is what stops the walk cycle stuttering at every
        // boundary.
        selectAnimation(animation, animationFor(player.state));
    }
}

} // namespace engine::systems
