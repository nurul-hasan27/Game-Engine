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
    // What a player is *for this system*: a transform, a collider, the level's
    // constants and its own state. [Animation] is deliberately **not** in the query -
    // choosing the picture is [engine::systems::PlayerStateSystem]'s job now, because
    // the state it chooses from depends on a collision this system has not seen yet.
    for (auto&& [entity, transform, collider, config, player] :
         entities.query<Transform, Collider, PlayerConfig, Player>())
    {
        static_cast<void>(entity);
        static_cast<void>(collider);

        // ---- 1. Has it fallen out of the world? -------------------------------
        if (hasFallenOut(transform.position, collider.size, m_fallLimitY))
        {
            respawn(transform, player);
        }

        // ---- 2. Horizontal intent ---------------------------------------------
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

        // ---- 3. Facing ---------------------------------------------------------
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

        // ---- 4. Jump ------------------------------------------------------------
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

        // ---- 5. Gravity --------------------------------------------------------
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
        // ### Applied every frame, and that reverses a Phase 16 decision
        //
        // Phase 16 applied gravity only while `!grounded`, so that a standing player
        // carried no downward speed instead of accumulating one that collision
        // cancelled every frame. Sound in itself, and it stops working the moment
        // `grounded` is answered by a real collision rather than a probe.
        //
        // The two are mutually exclusive. With gravity withheld while grounded, a
        // resting player does not move at all, so on the following frame its box is
        // exactly *touching* the tile rather than overlapping it. The course's overlap
        // arithmetic makes touching zero, and zero is not a collision - so there is no
        // support to report, `grounded` goes false, gravity resumes, and the flag
        // flips on every other frame. A player whose grounded state flickers cannot be
        // jumped from reliably, because the jump gate reads it.
        //
        // Unconditionally removes the cycle. Gravity gives the player a quarter-pixel
        // of downward velocity, the physics step integrates that into a real
        // penetration, the resolver pushes them back out and zeroes the component, and
        // the step ends with the player in contact *and* a support collision on the
        // record - every frame, with no tolerance anywhere in the engine.
        //
        // It is also the course's sentence read literally: a gravity component that
        // "constantly accelerates it downward on the screen **until it collides with a
        // tile**". The "until it collides" is the resolver zeroing the velocity, and
        // that now happens in one step rather than a frame later.
        transform.velocity.y += config.gravity * deltaSeconds;

        // ---- 6. Maximum horizontal speed --------------------------------------
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

    }
}

} // namespace engine::systems
