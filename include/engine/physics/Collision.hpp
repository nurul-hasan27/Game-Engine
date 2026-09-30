#pragma once

#include "engine/components/Collider.hpp"
#include "engine/components/Transform.hpp"
#include "engine/ecs/Entity.hpp"
#include "engine/math/Vec2.hpp"
#include "engine/physics/Aabb.hpp"
#include "engine/components/Body.hpp"

#include <cstddef>
#include <vector>

namespace engine::physics
{

/// One overlapping pair, as it was found and as it was resolved, for one frame.
///
/// ### Why this is a flat value type and not a pair of entity references
///
/// A collision outlives nothing. It is produced inside
/// [engine::systems::PhysicsSystem] and consumed by whatever runs after it, and
/// between those two moments an entity may be destroyed, a component removed, or the
/// storage that held it reallocated. A record holding `ecs::Entity&` would be a
/// dangling-reference generator with a plausible-looking interface; holding
/// [engine::ecs::EntityId] and a copy of every number a consumer needs means the
/// record stays true even if the world moves on underneath it.
///
/// The cost is that a consumer cannot reach through the record for something it does
/// not carry. That is the right trade for a per-frame observation: everything needed
/// to classify a collision is here, and nothing here can expire.
struct Collision
{
    /// The two participants, in the order the pair was visited.
    ///
    /// Order is arbitrary and carries no meaning - it is whatever the iteration
    /// produced - which is exactly why [resolutionFor] and [bodyOf] exist rather than
    /// a consumer reading `firstBody` and hoping it is the one it cares about.
    /// [engine::ecs::EntityId] is a `std::uint64_t` assigned by
    /// [engine::ecs::EntityManager] and never reused, so it stays meaningful for as
    /// long as the report does.
    ecs::EntityId first = ecs::kInvalidEntityId;

    /// The other participant. See [first].
    ecs::EntityId second = ecs::kInvalidEntityId;

    /// The current per-axis penetration depth: the course's `getOverlap`.
    ///
    /// Positive means overlapping by that much, zero means exactly touching, negative
    /// means apart. See [Aabb::penetration] for why the negative range is kept.
    ///
    /// Always strictly positive in practice, because a record only exists for a pair
    /// that [Aabb::overlaps].
    Vec2 overlap{0.0F, 0.0F};

    /// The same quantity one physics step earlier: the course's `getPreviousOverlap`.
    ///
    /// This is the field that answers "how did they get here", and it is the whole
    /// reason the report exists. A player standing on a floor and a player who has just
    /// landed on that floor have identical [overlap] to within a rounding error, and
    /// completely different [previousOverlap]: the first was already resting, the
    /// second arrived from above. See [landedOn] for how that difference is used.
    Vec2 previousOverlap{0.0F, 0.0F};

    /// The minimum translation vector that separated the pair, oriented as the push
    /// **this** body would receive.
    ///
    /// A geometric quantity, not a record of what moved. The three cases in
    /// [engine::systems::PhysicsSystem] all store the same `correction`; which
    /// participant actually moved is a different fact, and a dynamic pair is pushed
    /// apart by half each. Storing the half would have made the magnitude a value no
    /// consumer could compare against a distance, and the sign - the only part
    /// anything reads - would have been the same anyway.
    ///
    /// [resolutionFor] reorients it, so a consumer always reads it relative to itself
    /// and never has to know which of the two it was.
    ///
    /// Negative y is upward, because this engine's Y grows downwards. The sign is what
    /// distinguishes a floor from a ceiling: both arrive with [previousOverlap].y at
    /// or below zero, and only the direction of the push says which one was hit.
    Vec2 resolution{0.0F, 0.0F};

    /// Whether [first] can be pushed. See [bodyOf].
    BodyType firstBody = BodyType::Static;

    /// Whether [second] can be pushed. See [bodyOf].
    BodyType secondBody = BodyType::Static;
};

/// Whether `entity` took part in `collision`.
///
/// The first question to ask of a record, because a consumer scanning the report for
/// its own collisions must not act on somebody else's.
[[nodiscard]] bool isParticipant(const Collision& collision, ecs::EntityId entity) noexcept;

/// The other participant: `second` when `entity` is `first`, and vice versa.
///
/// [engine::engine::ecs::kInvalidEntityId] when `entity` is not in the collision at all,
/// which is a mistake in the caller rather than a state to handle.
[[nodiscard]] ecs::EntityId partnerOf(const Collision& collision, ecs::EntityId entity) noexcept;

/// [Collision::resolution] as it applied to `entity`, negated for the second
/// participant.
///
/// The record stores one vector for the pair, oriented towards [Collision::first]. A
/// consumer almost always wants it relative to itself, and getting that backwards
/// would invert every landing test in the engine, so it is a named function rather
/// than a sign the caller has to remember.
[[nodiscard]] Vec2 resolutionFor(const Collision& collision, ecs::EntityId entity) noexcept;

/// Whether the *other* participant is a [BodyType::Static] body.
///
/// This is the floor rule, and it is the only place the rule lives. The course says
/// the player "land on a **Tile** entity" and nothing else, and Phase 16 established
/// that a dynamic body is not a floor - otherwise a player would come to rest on a
/// bullet, and the two would push one another indefinitely.
///
/// The phrasing is deliberate: it is the **partner** that must be static, not the
/// entity asking. A static body has no velocity and is never the one that arrived.
[[nodiscard]] bool hasStaticPartner(const Collision& collision, ecs::EntityId entity) noexcept;

/// Whether `entity` came down onto `collision`'s partner this step.
///
/// The whole reason [Collision::previousOverlap] exists, stated once so that every
/// consumer classifies a landing the same way.
///
/// ```text
///   previousOverlap.x > 0    it was already beside the partner horizontally
///   previousOverlap.y <= 0   it was NOT vertically overlapping the partner
///   overlap.y > 0            it is vertically penetrating now
///   resolutionFor(me).y < 0  the push was upward, so it came from above
///   the partner is static     and a floor is a Tile, not a bullet
/// ```
///
/// ### Why each clause is needed, since removing any one of them is a bug
///
/// Without the first, a player that arrives diagonally from above-and-beside counts
/// as a landing even though it never had its feet over the surface.
///
/// Without the second, a player pressed against a **wall** counts as a landing: it is
/// already inside the wall's vertical extent, so the vertical overlap was positive
/// before the step and nothing about its vertical relationship changed. This is the
/// single most important clause, and the one Phase 16 could not express - which is why
/// Phase 16 had to probe for the floor instead.
///
/// Without the third, a player that is merely adjacent counts, which is every resting
/// frame.
///
/// Without the fourth, a player that hit the **underside** of a platform counts as
/// having landed on it. The two cases have identical previous overlaps and differ only
/// in which way the push went.
///
/// Without the fifth, a player comes to rest on a falling bullet.
///
/// Note what is *not* here: no tolerance, and no distance threshold. Every clause is a
/// comparison of a quantity the collision arithmetic already produced, so the answer
/// cannot be wrong because a magic number was mistuned.
[[nodiscard]] bool landedOn(const Collision& collision, ecs::EntityId entity) noexcept;

/// Whether `entity` came up against the **underside** of the partner this step.
///
/// The mirror of [landedOn]: the same three overlap clauses, with the push going the
/// other way. A player who jumps into a low ceiling is stopped, and must not be
/// recorded as having landed on it.
[[nodiscard]] bool hitCeilingWith(const Collision& collision, ecs::EntityId entity) noexcept;

/// Whether `entity` came in against the **side** of the partner this step.
///
/// A new horizontal overlap with an overlap that was already there vertically, or a
/// push along x. Expressed as the negation of the two vertical cases so that the three
/// outcomes cannot drift apart: a wall hit is whatever is not a landing and not a
/// ceiling.
[[nodiscard]] bool hitSideWith(const Collision& collision, ecs::EntityId entity) noexcept;

/// Every collision [engine::systems::PhysicsSystem] found and resolved this frame.
///
/// ### Lifetime, which is the whole reason this type exists as a member
///
/// The report is owned by the physics system, cleared at the **start** of every
/// update, and filled during that update. So its contents are exactly "this frame",
/// with no frame's results surviving into the next one, and a system that never
/// registers anything cannot read a stale answer.
///
/// It is a value container: [collisions] hands out a reference to a member vector
/// that is only mutated by the owner, at a point in the frame when no reader is
/// running. A reader that wants to keep a record past the frame must copy it, which
/// is one `Collision` of trivially copyable data.
///
/// It is not a singleton and not reachable without being given a reference, so a
/// gameplay system cannot read a report that no physics system produced.
class CollisionReport
{
public:
    /// Forgets everything. Called at the top of the physics step, so the report can
    /// never describe a frame that has already been resolved.
    void clear() noexcept { m_collisions.clear(); }

    /// Adds one resolved pair. Called by [engine::systems::PhysicsSystem] only.
    void add(const Collision& collision) { m_collisions.push_back(collision); }

    /// This frame's collisions, in the order they were resolved.
    [[nodiscard]] const std::vector<Collision>& collisions() const noexcept { return m_collisions; }

    /// How many collisions this frame.
    [[nodiscard]] std::size_t size() const noexcept { return m_collisions.size(); }

    /// Whether anything collided this frame.
    [[nodiscard]] bool empty() const noexcept { return m_collisions.empty(); }

private:
    std::vector<Collision> m_collisions;
};

/// The course's `getOverlap`: the per-axis penetration depth of two entities' boxes.
///
/// ```text
///   overlap.x == (aHalfX + bHalfX) - |aPosX - bPosX|
///   overlap.y == (aHalfY + bHalfY) - |aPosY - bPosY|
/// ```
///
/// Positive when the boxes overlap by that much on that axis, exactly zero when they
/// touch, negative when they are apart. This is the arithmetic of
/// `Physics::GetOverlap` in the course's own reference, and [Aabb::penetration] is
/// where it is implemented; this function is the entity-facing form of it.
///
/// ### `(0, 0)` when either box is absent, and that is a contract rather than a
/// fallback
///
/// An entity with no [engine::components::Transform] or no
/// [engine::components::Collider], an entity that is not alive, and an entity that has
/// been destroyed all produce `(0, 0)` rather than a number derived from nothing.
///
/// Zero is the *safe* direction, and the choice is worth stating: under this
/// convention zero means "exactly touching", which is not a collision, so an absent
/// box can never be mistaken for a landing, a ceiling hit or an overlap. Had the
/// absent case returned a large positive value, a consumer checking only
/// `overlap.y > 0` would read every uncollidable entity as a floor. The alternative
/// - throwing - was rejected because a level that contains a decoration without a
/// collider is legal, and asking about it is a reasonable question, not a programming
/// error.
///
/// [getPreviousOverlap] uses the same rule, which the course's Phase 16 requirement
/// states directly.
[[nodiscard]] Vec2 getOverlap(const ecs::Entity& first, const ecs::Entity& second) noexcept;

/// The course's `getPreviousOverlap`: [getOverlap] computed one physics step ago.
///
/// Uses [engine::components::Transform::prevPosition] for **both** entities, so the
/// answer describes the relationship as it was before the step that is running - which
/// is the only moment from which "how did they arrive here" can be read. Computing it
/// from the current position instead would return a number identical to [getOverlap],
/// and every consumer would silently degrade to knowing only that a collision exists.
///
/// `(0, 0)` when either box is absent, for the reason given on [getOverlap].
[[nodiscard]] Vec2 getPreviousOverlap(const ecs::Entity& first, const ecs::Entity& second) noexcept;

} // namespace engine::physics
