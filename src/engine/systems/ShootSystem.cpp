#include "engine/systems/ShootSystem.hpp"

#include "engine/components/Animation.hpp"
#include "engine/components/Body.hpp"
#include "engine/components/Bullet.hpp"
#include "engine/components/Collider.hpp"
#include "engine/components/Lifetime.hpp"
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
#include <vector>

namespace engine::systems
{

namespace
{

using engine::Vec2;
using engine::components::Animation;
using engine::components::Body;
using engine::components::Bullet;
using engine::components::Collider;
using engine::components::Lifetime;
using engine::components::Player;
using engine::components::PlayerConfig;
using engine::components::Transform;
using engine::ecs::Entity;
using engine::ecs::EntityManager;
using engine::input::Action;
using engine::input::ActionState;
using engine::physics::BodyType;

/// How far in front of the player's centre a bullet is created, in world pixels.
///
/// The course's reference writes this as a literal:
/// `Vec2 entityPos = ...; (scale.x < 0) ? entityPos.x -= 64 : entityPos.x += 64;`
///
/// 64 is also [engine::level::LevelGrid::kCellSize], so the number is not arbitrary even
/// though the reference does not say why: a bullet appears one grid cell in front of the
/// player, which is far enough to clear the player's own collider and near enough to look
/// like it came out of them.
///
/// ### Why it is measured from the centre, and what that costs
///
/// From the player's **centre**, not from the edge of its collider. With the level's own
/// 40-pixel collider, the player's half-width is 20 and the bullet's is 8, so a
/// centre-to-centre distance of 64 leaves 36 pixels of clear air between the two boxes -
/// comfortably more than one frame of movement, which is what stops a bullet from
/// destroying itself on the frame it was created.
///
/// Measuring from the edge instead would be a smaller offset (36) and would be wrong in a
/// way this could not detect: a bullet spawned 36 from the centre has its left edge 28 from
/// the centre and 8 from the player's right edge, so it starts adjacent to the player and
/// the two overlap again the moment the player moves towards it.
constexpr float kBulletSpawnOffset = 64.0F;

/// How much faster than the player a bullet travels, as a multiple of the level's own
/// left/right speed.
///
/// The course's reference is one expression:
/// `bullet->get<CTransform>().velocity.x = entity->get<CTransform>().scale.x * m_playerConfig.SPEED * 6;`
///
/// Three things are read out of that, and all three are kept:
///
/// - It is **the level's speed times a factor**, not a constant. So the relationship
///   between the two survives a level that asks for a different one, which is what the
///   course's own level format implies when `SX` is a per-level field.
/// - The factor is `6`, and it is applied on top of the player's speed rather than
///   replacing it. A bullet is six times faster than the thing that fired it, which is
///   fast enough to cross the level and slow enough to aim at it.
/// - It is signed by the player's **facing**, which is [engine::components::Transform]'s
///   `scale.x` - the course's own convention, and the one
///   [engine::systems::PlayerSystem] established in Phase 16.
///
/// The committed level's `SX` is 200, so a bullet crosses 1200 pixels a second, or 20
/// pixels per 60 Hz frame.
constexpr float kBulletSpeedFactor = 6.0F;

/// How many game frames a bullet has before it is removed, having missed everything.
///
/// The course gives bullets a wall-clock lifespan - `CLifeSpan(1.8231)` compared against an
/// `sf::Clock` - and this engine measures the same quantity in frames for the reasons
/// [engine::components::Lifetime] gives. 1.8231 seconds is 109.4 frames at the engine's
/// configured 60 FPS, so 109 frames is that lifespan stated in the currency the engine
/// already uses for how long anything takes.
///
/// ### Why a bullet needs one at all
///
/// Without it a bullet that misses leaves the level and stays in the world forever,
/// integrated and collision-tested against every other entity for the rest of the session.
/// The physics broad phase is already documented as deliberately naive - O(N^2), no spatial
/// index - so an unbounded number of projectiles is the one thing that turns a stated
/// simplification into a real cost. The lifetime bounds the population without changing
/// anything about how a bullet behaves while it is alive.
constexpr std::uint32_t kBulletLifetimeFrames = 109U;

/// Which way the player is facing: -1 when its sprite is mirrored, +1 otherwise.
///
/// The course's Phase 16 rule, read literally: `scale.x < 0` faces left, anything else
/// faces right. `>= 0` rather than `> 0` so a player whose scale has somehow reached zero
/// is treated as facing right rather than as neither, which would leave the bullet
/// stationary with a direction nobody asked for.
[[nodiscard]] float facingOf(const Vec2& scale) noexcept { return scale.x < 0.0F ? -1.0F : 1.0F; }

/// Everything a bullet needs, gathered during the walk over the players.
struct BulletRequest
{
    Vec2 position{0.0F, 0.0F};
    Vec2 velocity{0.0F, 0.0F};
    std::string animationName;
};

} // namespace

void ShootSystem::update(EntityManager& entities, const ActionState& actions, const float deltaSeconds)
{
    static_cast<void>(deltaSeconds);

    // The whole system is behind one edge. Everything below - gathering requests, resolving
    // the asset, creating entities - is skipped on every frame the player is not firing,
    // which is every frame but the ones that cost anything.
    if (!actions.wasPressed(Action::Shoot))
    {
        return;
    }

    // ### Gathering first, creating after
    //
    // `Query` borrows the manager's own storage, and `addEntity` invalidates iterators
    // into it. So a spawn inside this walk would be a use-after-invalidate, however
    // tempting it is to spawn as soon as the player is found. Collecting first costs one
    // small vector and makes the rule structural rather than a comment.
    std::vector<BulletRequest> requests;

    for (auto&& [entity, transform, config, player] : entities.query<Transform, PlayerConfig, Player>())
    {
        static_cast<void>(entity);
        static_cast<void>(player);

        // Facing, captured **now**. A bullet's direction is fixed at the moment it is
        // created: the player turning around afterwards does not bend a shot already in
        // flight, and nothing downstream reads the player's scale again. The whole of the
        // bullet's direction lives in the velocity written below, which is why
        // `components::Bullet` has no direction field to fall out of step.
        const float facing = facingOf(transform.scale);

        BulletRequest request;
        request.position = Vec2{transform.position.x + (facing * kBulletSpawnOffset), transform.position.y};
        request.velocity = Vec2{facing * (config.leftRightSpeed * kBulletSpeedFactor), 0.0F};
        request.animationName = config.bulletAnimationName;
        requests.push_back(std::move(request));
    }

    for (const BulletRequest& request : requests)
    {
        // Resolved through the manager rather than stored, for the same reason the level
        // loader resolves it: the frame size is a property of the artwork, and the artwork
        // belongs to the asset system. A name that does not resolve throws, which is
        // correct - a level whose bullet animation is missing is a broken level, and the
        // loader already refuses to load one.
        const engine::assets::Animation& definition = m_assets->animation(request.animationName);

        // Half the animation's frame size, which is the course's own line:
        // `bullet->addComponents<CBoundingBox>(animation.getSize() / 2);`
        //
        // Worth being precise about, because it is the one place this engine's bullet box is
        // *not* the size of its artwork. A tile's box is its animation's size, because the
        // course says so and because a tile is the thing you see. A bullet's artwork is a
        // sprite with transparent margins around the projectile, and the course halves it so
        // the collision box is the shot rather than the sheet it is drawn on.
        //
        // Half of 32x26 is 16x13, which is also comfortably smaller than the 20 pixels a
        // 60 Hz frame moves a 1200 px/s bullet - so a bullet cannot tunnel through a
        // 64-pixel tile in one step, which the box being a quarter the area helps with.
        const Vec2 frameSize{static_cast<float>(definition.frameWidth()), static_cast<float>(definition.frameHeight())};

        EntityManager& world = entities;
        Entity& bullet = world.addEntity(std::string{components::kBulletTag});

        bullet.addComponent<Transform>(Transform{request.position, request.velocity, Vec2{1.0F, 1.0F}, 0.0F});

        // `repeat` is **true**, and that is not a default being left alone. A one-frame
        // animation with `repeat` false would be advanced once by
        // [engine::systems::AnimationSystem], marked `ended` immediately, and destroyed -
        // so the bullet would vanish on the frame it was created and the whole phase would
        // look like it worked for one frame. A bullet's lifetime is
        // [engine::components::Lifetime]'s business, not the animation's, and a static
        // picture is exactly what `repeat = true` says.
        bullet.addComponent<Animation>(Animation{request.animationName, 0U, 0U, true, false});

        bullet.addComponent<Collider>(Collider{frameSize * 0.5F});
        bullet.addComponent<Body>(Body{BodyType::Dynamic});
        bullet.addComponent<Bullet>();

        bullet.addComponent<Lifetime>(Lifetime{kBulletLifetimeFrames});
    }
}

} // namespace engine::systems