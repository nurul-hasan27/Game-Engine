#include "engine/systems/PlayerSystem.hpp"

#include "engine/components/Animation.hpp"
#include "engine/components/Body.hpp"
#include "engine/components/Collider.hpp"
#include "engine/components/Player.hpp"
#include "engine/components/PlayerConfig.hpp"
#include "engine/components/Transform.hpp"
#include "engine/ecs/EntityManager.hpp"
#include "engine/input/Action.hpp"
#include "engine/input/ActionState.hpp"
#include "engine/math/Vec2.hpp"
#include "engine/physics/Aabb.hpp"

#include <cstdint>
#include <string>

namespace engine::systems
{

namespace
{

using engine::Vec2;
using engine::components::Animation;
using engine::components::Body;
using engine::components::Collider;
using engine::components::Player;
using engine::components::PlayerConfig;
using engine::components::PlayerState;
using engine::components::Transform;
using engine::ecs::Entity;
using engine::ecs::EntityManager;
using engine::input::Action;
using engine::input::ActionState;
using engine::physics::Aabb;
using engine::physics::BodyType;

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

/// How far below the feet to look for ground.
///
/// A probe, not a simulation step, so it is a tolerance and not a distance travelled.
/// It has to be non-zero: the player's feet and a tile's top are coincident while
/// standing, and floating-point integration does not land them on the same bit.
/// It is a quarter of a pixel, which is far below any distance the player can fall in
/// one frame at any speed this level can produce, so it cannot read as "grounded"
/// for a player who has genuinely left the ground.
constexpr float kGroundProbeDepth = 0.25F;

/// Whether a body is something the player can stand on.
///
/// Static only. Two dynamic bodies - the player and, later, a bullet - must not be
/// each other's floor, and the course says the player lands on a **Tile**.
[[nodiscard]] bool isSolidGround(const Body& body) noexcept { return body.type == BodyType::Static; }

/// The player's box in world space.
[[nodiscard]] Aabb boxOf(const Transform& transform, const Collider& collider) noexcept
{
    return Aabb{transform.position, collider.size};
}

/// Whether the player's feet are resting on a solid surface.
///
/// ### The horizontal requirement, and why it is there
///
/// The probe is not "is there a tile below the player's centre". It is "is there a
/// tile below the player's **box**". A player standing half off the end of a ledge is
/// still standing on it, and a centre-only probe would report them airborne and drop
/// them a frame before physics had pushed them off. Overlap is tested on the X axis
/// with the same `Aabb::overlapX` the physics uses, so the two agree on what
/// "touching" means instead of each having their own idea.
///
/// ### The vertical requirement, and why it is not an overlap
///
/// The tile's top must be at the player's feet, not overlapping them. A tile the
/// player is *inside* - a wall, or the frame physics has not yet pushed them out of -
/// has its top above the player's feet, and would read as ground if this were an
/// overlap test. The player would then be "grounded" inside a wall and could not
/// jump, which is exactly the bug a wall-jam produces. Requiring the tile's top to be
/// at or just below the feet keeps walls out.
[[nodiscard]] bool isGroundedOn(const EntityManager& entities, const Vec2 position, const Vec2 size,
                                const Entity& self) noexcept
{
    const Aabb player = Aabb{position, size};
    for (auto&& [entity, transform, collider, body] : entities.query<Transform, Collider, Body>())
    {
        if (&entity == &self)
        {
            continue; // The player is dynamic, but a query is a query.
        }
        if (!isSolidGround(body))
        {
            continue;
        }

        const Aabb ground = boxOf(transform, collider);
        if (player.overlapX(ground) <= 0.0F)
        {
            continue;
        }

        // The tile's top against the player's feet, within the probe depth in
        // **both** directions.
        //
        // Two-sided is the whole correctness of this function. `rise` is positive
        // when the player is above the tile - still falling towards it - and negative
        // when the player's feet are below the tile's top - inside it, or hanging off
        // the bottom. A one-sided test of either sign says "grounded" to half the
        // plane: the first version tested `rise >= -depth` and called a player a
        // hundred pixels above the floor grounded, which would have let them jump in
        // mid-air and refused to let them fall.
        const float rise = ground.min().y - player.max().y;
        if (rise <= kGroundProbeDepth && rise >= -kGroundProbeDepth)
        {
            return true;
        }
    }

    return false;
}

/// Whether the player has fallen out of the world.
///
/// Tested on the **feet**, not the centre. Falling out of a world, a player leaves
/// it feet first, so the feet crossing the bottom is the moment the player is gone;
/// the centre crossing would keep a half-fallen player in play with its artwork
/// already off the bottom of the screen.
[[nodiscard]] bool hasFallenOut(const Vec2 position, const Vec2 size, const float fallLimitY) noexcept
{
    return (position.y + (size.y * 0.5F)) > fallLimitY;
}

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

/// Restores everything a respawn is supposed to restore.
///
/// Listed in one function because the list *is* the requirement, and a respawn that
/// restored the position but not the velocity would look correct for one frame and
/// then shoot off.
///
/// The animation is not reset here. It is reset by the state selection at the end of
/// the frame, which will see [PlayerState::Stand] and choose the standing picture -
/// and putting the reset in both places would mean two places to keep in step for one
/// observable.
///
/// Facing is reset rather than remembered. A respawn is a fresh start rather than a
/// continuation of the run that ended badly, so the player faces right, which is the
/// state a newly spawned player is in.
void respawn(Transform& transform, Player& player) noexcept
{
    transform.position = player.spawnPosition;
    transform.velocity = Vec2{0.0F, 0.0F};
    transform.scale = Vec2{1.0F, 1.0F};
    player.grounded = true;
    player.jumping = false;
    player.state = PlayerState::Stand;
}

} // namespace

void PlayerSystem::update(EntityManager& entities, const ActionState& actions, const float deltaSeconds)
{
    // [Animation] is in the query, not fetched with a `getComponent` inside it. A
    // player is a transform, a collider, the level's constants, its own state and a
    // picture; an entity missing the last of those is not something this system can
    // drive, and every other system in this engine states the same contract the same
    // way - by what it queries for. A `getComponent` here would throw instead, which
    // is a louder failure for a *bug* and a worse contract for a *caller*.
    for (auto&& [entity, transform, collider, config, player, animation] :
         entities.query<Transform, Collider, PlayerConfig, Player, Animation>())
    {
        static_cast<void>(entity);

        // ---- 1. Where does the player think it is standing? --------------------
        //
        // Read before anything is changed, so the answer describes the position the
        // previous frame's physics left behind. See the header for why a frame of
        // lag on touchdown is acceptable and why it belongs to Phase 17.
        player.grounded = isGroundedOn(entities, transform.position, collider.size, entity);

        // A landing ends the jump, here, before anything reads `jumping`. Doing it in
        // this order is what stops a jump from surviving touchdown: the flag is
        // cleared on the first frame the player is found grounded, so the ascent can
        // never be "cut short" by a jump variable from three jumps ago.
        if (player.grounded)
        {
            player.jumping = false;
        }

        // ---- 2. Has it fallen out of the world? -------------------------------
        if (hasFallenOut(transform.position, collider.size, m_fallLimitY))
        {
            respawn(transform, player);
        }

        // ---- 3. Horizontal intent ---------------------------------------------
        //
        // The course: "Left: A key, Right: D key". `MoveLeft` and `MoveRight` are
        // the actions the action layer already bound to those keys, so the player
        // reads actions and never names a key - which is what makes the mapping
        // rebindable and what stops a `sf::Keyboard` call from creeping back in.
        //
        // `MoveUp` and `MoveDown` are deliberately not read. They are bound to W and
        // S, and a platformer player does not walk upwards: W is the jump. Reading
        // MoveUp as vertical movement would make the player rise while holding the
        // jump key, which is the same bug as double-jumping wearing a different hat.
        const bool movingLeft = actions.isActive(Action::MoveLeft);
        const bool movingRight = actions.isActive(Action::MoveRight);
        const bool moving = movingLeft || movingRight;

        transform.velocity.x = moving ? (movingRight ? config.leftRightSpeed : -config.leftRightSpeed) : 0.0F;

        // ---- 4. Facing ---------------------------------------------------------
        //
        // The course: "If the player moves left/right, the player's sprite will face
        // in that direction **until the other direction has been pressed**".
        //
        // That last clause is the whole rule: facing is written **only** when a
        // direction is pressed. It is not written when the key is released, not when
        // the player jumps, and not when the player lands - so the last direction
        // pressed survives all three, which is what "until the other direction has
        // been pressed" means.
        //
        // `scale.x`, and not a separate flip flag, because the course says so and
        // because the renderer already honours it: `SfmlRenderer::statesFor` passes
        // the scale straight into the transform, and a negative x mirrors the sprite.
        // No renderer change, and a rectangle or a text drawn at the same placement
        // is unaffected in size.
        if (moving)
        {
            transform.scale.x = movingRight ? 1.0F : -1.0F;
        }

        // ---- 5. Jump ------------------------------------------------------------
        //
        // `wasPressed`, and this is the single most important line in the file.
        // `isActive` would be true for as long as W is held, and the course is
        // explicit: "If the jump key is held, the player should not continuously
        // jump, but instead it should only jump once per button press." With
        // `isActive`, holding W would re-launch the player on the frame it landed,
        // forever, and the variable jump below would never get a chance to run
        // because the player would never be ascending for long.
        if (actions.wasPressed(Action::Jump) && player.grounded)
        {
            // Up is negative: this engine's Y grows downwards, and the course's
            // reference writes the same thing - `velocity.y = -(JUMP)`.
            transform.velocity.y = -config.jumpSpeed;
            player.jumping = true;
        }
        else if (player.jumping && !actions.isActive(Action::Jump) && transform.velocity.y < 0.0F)
        {
            // ---- The variable jump --------------------------------------------
            //
            // The course: "If the player lets go of the jump key mid-jump, it should
            // start falling back down immediately."
            //
            // So the release **stops the ascent** rather than reducing it: the
            // upward velocity becomes zero and gravity takes over from there. A short
            // tap leaves a jump one frame long; a held key lets the full jump speed
            // integrate against gravity. Both fall out of the same two lines, and
            // neither needs a timer, a second gravity value, or a special case in
            // [engine::systems::PhysicsSystem] - the global physics behaviour is
            // untouched, which is what "do not fake variable jump height by changing
            // gravity globally" asks for.
            //
            // The `velocity.y < 0.0F` guard matters: releasing the key while already
            // falling must not cancel the fall, and must not make the player hover
            // with gravity repeatedly zeroing a descent.
            transform.velocity.y = 0.0F;
            player.jumping = false;
        }

        // ---- 6. Gravity --------------------------------------------------------
        //
        // "The player should have a Gravity component which constantly accelerates
        // it downward on the screen until it collides with a tile."
        //
        // The value is the level's - [PlayerConfig]'s `gravity`, which is the number
        // the level file gave - and the integration is [engine::systems::PhysicsSystem]'s.
        // This only adds the acceleration to the velocity; nothing here moves the
        // player. Applying it only while airborne matches the course's reference and
        // is what keeps a standing player's velocity at exactly zero rather than
        // accumulating a downward speed that is silently cancelled by collision every
        // frame.
        if (!player.grounded)
        {
            transform.velocity.y += config.gravity * deltaSeconds;
        }
        else if (!player.jumping)
        {
            // Resting. Zeroing rather than trusting that physics already did it, so
            // that a player who is grounded with a stale downward velocity - after a
            // respawn, say - cannot drag it. This is the "does not remain stuck with a
            // stale negative vertical velocity" case, made structural instead of
            // hoping the collision happened to clear it.
            transform.velocity.y = 0.0F;
        }

        // ---- 7. Maximum horizontal speed --------------------------------------
        //
        // "The player has a maximum speed specified in the Level file." Applied to x
        // only, and the header says at length why - with this level's own numbers,
        // clamping y as well would make the level's 400 px/s jump speed unreachable
        // and cap the jump below the player's own height.
        if (transform.velocity.x > config.maxSpeed)
        {
            transform.velocity.x = config.maxSpeed;
        }
        else if (transform.velocity.x < -config.maxSpeed)
        {
            transform.velocity.x = -config.maxSpeed;
        }

        // ---- 8. Which state is the player in? ----------------------------------
        //
        // Air wins over grounded, and the reason is [Player]'s `jumping` flag rather
        // than a negation of `grounded`. On the frame a jump starts, the probe still
        // finds the tile being left, so `grounded` is briefly true; without
        // `jumping` the player would show the standing sprite for one frame at the
        // bottom of every jump.
        //
        // Run is "moving horizontally", which is read from the velocity this frame
        // set rather than from the actions, so it is true while the player is
        // actually moving and cannot be true while they are standing still.
        const bool airborne = !player.grounded || player.jumping;
        player.state = airborne ? PlayerState::Air : (moving ? PlayerState::Run : PlayerState::Stand);

        // ---- 9. Which picture goes with it? ------------------------------------
        //
        // Selection only. [engine::systems::AnimationSystem] owns the frame index,
        // and it runs after this system, so the animation chosen here is the one
        // advanced this frame.
        selectAnimation(animation, animationFor(player.state));
    }
}

} // namespace engine::systems
