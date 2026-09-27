# Physics

This document covers the collision layer: the components that describe a body,
the AABB maths, how pairs are found, and how overlaps are resolved. For the ECS
see [ecs.md](ecs.md), for input [input.md](input.md), for the frame clock
[runtime.md](runtime.md), and for drawing [rendering.md](rendering.md).

---

## 1. Architecture

```text
input::Input
        ↓
systems::MovementSystem          input → Transform::velocity
        ↓
systems::PhysicsSystem
        1. integrate             position += velocity * deltaSeconds
        2. detect                every collidable pair, once each
        3. resolve position      push apart along the axis of least penetration
        4. resolve velocity      drop velocity directed into the surface
        ↓
components::Transform::position
        ↓
systems::RenderSystem → Renderer → pixels
```

| Piece | Namespace | SFML? | Job |
| ----- | --------- | ----- | --- |
| `Body` | `engine::components` | no | dynamic or static |
| `Collider` | `engine::components` | no | collision box size |
| `Aabb` | `engine::physics` | no | the box maths and the overlap test |
| `PhysicsSystem` | `engine::systems` | no | integrate, detect, resolve |

**One system owns position.** `MovementSystem` writes velocity and `PhysicsSystem`
moves the body. Nothing else writes `Transform::position`. That split is the whole
reason the chain above works: two systems both adding to position would each move
the body, and the results would compound.

---

## 2. Collider

`components::Collider` is pure data:

```cpp
struct Collider
{
    Vec2 size{0.0F, 0.0F};
};
```

It is deliberately separate from `components::Rectangle`, which is the shape used
for drawing. A game routinely wants them to differ: a player sprite is much larger
than its collider, a wide grass tile has a small collider, a projectile's visual
can trail behind its hitbox. Neither component contains the other and neither
knows the other exists. The Phase 8 demo happens to give the player a collider and
a rectangle of the same size, but nothing requires that.

`Collider` contains no behaviour, no SFML type, and no reference to a window. It
does not know it is going to be tested against anything.

A collider is meaningless without a `Body`: the pair query requires
`Transform`, `Collider` **and** `Body`, so an entity that has a shape but no body
is not part of any collision.

### `Body`

```cpp
struct Body
{
    physics::BodyType type = physics::BodyType::Dynamic;
};
```

`Body` is physical state, not a shape. An entity can have a `Body` and no
`Collider` (it moves, but nothing can bump into it), or a `Collider` and no `Body`
(an obstacle, which is the usual case for a wall).

Only two body types exist:

| Type | Moves from velocity | Displaced by collision |
| ---- | -------------------- | ---------------------- |
| `Dynamic` | yes | yes |
| `Static` | no, velocity is forced to zero every step | no |

`Kinematic` is deliberately absent. It would need a third rule — pushed by a
system but not by velocity and not displaced by collision — and nothing in this
phase wants one.

`BodyType` lives in `engine::physics` rather than `engine::components` because it
is a physics concept used by both the component and the physics maths, the same
way `RenderTransform` is a graphics concept used by components.

---

## 3. The AABB

`physics::Aabb` is stored as **centre plus half extents**:

```cpp
class Aabb
{
    Vec2 m_center;
    Vec2 m_halfExtents;
};
```

Phase 6 established that `Transform::position` is the centre of the drawn object,
and this keeps that one convention everywhere. An AABB is never built from a
top-left corner anywhere in the engine, so nothing converts between the two forms.

`min()` and `max()` are **derived**, not stored, so the representation cannot drift
out of sync with itself.

### Centre-based coordinates

The collider box is centred on the transform:

```text
halfSize = collider.size * 0.5
min      = position - halfSize
max      = position + halfSize
```

There is no anchor, offset or pivot on either component. A size change resizes the
box symmetrically about the body's position, which is what a top-left-anchored
sprite usually does *not* want.

### Degenerate sizes

A negative size is clamped to zero, so a box is never inverted (min above max).
A zero-size box is a degenerate point, and because the overlap test is strict
(§4) a point has no extent and therefore **never** overlaps anything. A collider
with a zero size is inert rather than an error, which is the simplest predictable
rule.

---

## 4. The collision test

Two AABBs overlap when their projections overlap on **both** axes:

```cpp
overlapX = min(maxA.x, maxB.x) - max(minA.x, minB.x)
overlapY = min(maxA.y, maxB.y) - max(minA.y, minB.y)

overlaps = overlapX > 0 && overlapY > 0
```

Both `overlapX` and `overlapY` are **signed**: positive means the projections share
width, zero means they touch exactly, negative means they are separated on that
axis. The sign is what lets one function answer "do these overlap", "how deep",
and "which axis" without recomputing.

### Touching is not overlapping

The test is **strict**. Two boxes that share only an edge, with no shared area, do
**not** overlap. `overlap == 0` is not penetration.

This is the important boundary decision in the whole phase, and it is deliberate.
It is what lets a body rest exactly on a surface without being reported as
penetrating it every frame. A non-strict test (`>= 0`) would call a body resting on
the floor overlapping it, and the resolver would keep pushing a body that is
already exactly where it should be.

The test `aabb touching edges do not overlap` pins this down for an edge touch, a
corner touch, and the reported overlap values on each axis.

### Penetration axis

`penetrationAxis()` returns the axis of **least** penetration, which is the axis a
body can escape along most cheaply:

```cpp
overlapX < overlapY ? Horizontal : Vertical
```

**Tie behaviour: an exact tie resolves on the vertical axis.** The choice is
arbitrary, but it is fixed, and a fixed arbitrary choice is what determinism
requires. Choosing the axis of greatest approach velocity instead would be more
physically sensible at a corner and is a plausible refinement, but it is not
implemented here.

---

## 5. Minimum translation vector

`minimumTranslation()` returns the offset that would push **this** box out of
`other`, along the axis of least penetration:

```cpp
if (overlapX < overlapY)
    return { center.x < other.center.x ? -overlapX : +overlapX, 0 };
return { 0, center.y < other.center.y ? -overlapY : +overlapY };
```

The sign comes from the **relative centres**, never from a fixed direction.
Whichever box is further left is pushed left. That makes the result correct for
both orientations of the same pair, and it is why the pair loop does not care
which of the two entities happened to be created first.

It is only meaningful when `overlaps()` is true. `PhysicsSystem` always checks
first. On touching boxes it returns a zero vector, which is harmless.

---

## 6. Pair iteration

Detection uses the existing ECS query. `PhysicsSystem` never scans raw storage:

```cpp
for (auto&& [entity, transform, collider, body] : entities.query<Transform, Collider, Body>())
```

The pair loop runs two independent iterators over that query, advancing the inner
one from just after the outer one:

```cpp
auto outer = bodies.begin();
const auto last = bodies.end();
for (; outer != last; ++outer)
{
    auto inner = outer;
    ++inner;
    for (; inner != last; ++inner) { /* one pair */ }
}
```

That guarantees `i < j`, so:

- **no entity is compared with itself**, and
- **every pair is tested exactly once**.

`testEachPairResolvedOnce` pins this down by placing three mutually overlapping
bodies and asserting the exact positions a single pass produces. A loop that
compared `A` with `B` and also `B` with `A`, or that included `A` with `A`, would
compound the corrections and produce different numbers.

Using two iterators rather than collecting entities into a vector also means the
pair loop allocates nothing per frame.

### Broad phase

**The broad phase is intentionally naive at this scale.** Every collidable entity
is tested against every other, which is O(N²). For the handful of bodies in this
phase that is nothing at all.

No spatial grid, no quadtree, no sweep-and-prune, and no AABB tree. Those are the
right answers at thousands of bodies and the wrong thing to add before there is a
measured problem. The naive loop is also the version that is easy to read and
easy to test, which matters more while the resolution rules are still settling.

---

## 7. Resolution

For an overlapping pair, the correction is `firstBox.minimumTranslation(secondBox)`.
Who takes it depends on the two body types:

| First | Second | Result |
| ----- | ------ | ------ |
| Dynamic | Dynamic | each takes half, in opposite directions |
| Dynamic | Static | the first takes all of it |
| Static | Dynamic | the second takes all of it, reversed |
| Static | Static | nothing moves |

Two static bodies cannot be pushed apart by anything, so nothing happens. That is
not an error: overlapping walls are a level designer's problem, and a resolver
that pretended otherwise would make a wall slide around when two wall segments met
at a corner.

The two dynamic cases are deliberately asymmetric in effect but symmetric in
direction. Splitting the correction means the pair is left non-overlapping and
neither body is favoured, which matters for two objects pushing on each other.

### Velocity resolution

After the positional correction, any velocity component directed **into** the
surface the body was just pushed out of is zeroed.

The correction direction tells us which way is "out", so the surface is in the
opposite direction. A velocity component opposing the correction is heading into
that surface:

```cpp
if (correction.x != 0.0F && velocity.x * correction.x < 0.0F) velocity.x = 0.0F;
if (correction.y != 0.0F && velocity.y * correction.y < 0.0F) velocity.y = 0.0F;
```

Two consequences fall out of this and both are tested:

- **Tangential velocity survives.** On a horizontal collision only `velocity.x` is
  touched, so a body running diagonally into a wall keeps sliding along it. A
  diagonal approach must not stop the whole body.
- **Moving away keeps its velocity.** A body whose velocity already agrees with the
  correction is not frozen merely because the boxes happen to overlap. Only a
  component pointed into the surface is removed.

There is **no bounce and no restitution.** A body that hits a wall stops dead
along that axis. Adding a coefficient later is a small change to this one place.

---

## 8. System ordering

Registration order is update order, and the order is load bearing:

```text
SystemManager
    MovementSystem      input → velocity
    PhysicsSystem       velocity → position, then collisions
```

`PhysicsSystem` **must** be registered after `MovementSystem`. Registered the
other way round, it would integrate the previous frame's velocity and input would
lag the player by a frame. There is no priority system and no scheduler; the
existing `SystemManager` order is the whole mechanism.

`RenderSystem` still runs after everything, in `Application`'s own pass, for the
reasons given in [rendering.md](rendering.md) §4.

`PhysicsSystem` takes an `Input&` and ignores it. It is a consequence of the
world's velocity, not of what the player is pressing this frame. The parameter is
there because it is part of the `System` interface, and the interface stays uniform
so that any system can be added without changing the others.

---

## 9. Discrete collision detection

**Phase 8 uses discrete collision detection.**

A body advances by `velocity * deltaSeconds` in full, and only then is the new
position tested. Nothing checks the path it took. A body moving fast enough can
pass completely through a thin collider in one step and end up on the far side with
no collision ever reported.

This is not a bug to be fixed later by tuning. It is a property of the approach,
and continuous collision detection is a different algorithm, not a parameter. The
test `tunnelling is possible and unprevented` exists to pin the behaviour down: a
10-pixel body crossing a 2-pixel wall at 5000 px/s in a 1-second step comes out the
other side, and that is the expected result.

Two further consequences of discrete detection, both real and both observed:

- **Resting jitter.** When something keeps re-applying velocity every frame, a
  body pressed against a wall is integrated into it, corrected back out, and
  integrated into it again. With `MovementSystem` driving a body at 300 px/s in
  1/60 s frames, the resting position oscillates by a few pixels rather than
  sitting still. A body whose velocity is only set once settles exactly, because
  after the first correction its velocity is zero and it never moves again.
- **A single pass may not fully separate a cluster.** Three bodies pushed into each
  other are resolved pairwise in one pass, and a later pair's correction can leave
  an earlier pair still slightly overlapped. Over a few frames it settles, but a
  single frame is not guaranteed to be overlap-free.

---

## 10. Numerical contracts

- `engine::Vec2` semantics are used throughout. There is no second vector type in
  the physics layer.
- `Aabb` clamps a negative size to zero rather than producing an inverted box.
- Boundary behaviour is exact, not tolerant: `overlap > 0` is a strict comparison,
  and no epsilon is introduced. Touching is genuinely not overlapping, and
  `325.0` is genuinely `325.0`.
- No global epsilon framework exists. The tests use a `1e-4` tolerance in `CHECK_NEAR`
  for values that went through floating-point arithmetic, which is a property of
  the assertions, not of the engine.

---

## 11. Current limitations

- **Discrete detection only.** Tunnelling, resting jitter and single-pass
  cluster resolution are all described in §9. None of them are fixed.
- **No rotation.** The AABB is axis aligned and `Transform::angle` is ignored by
  physics. A rotated body still collides as an unrotated box, which is wrong for
  anything that spins. This is the single biggest gap.
- **No impulses, friction or restitution.** Resolution is purely positional, and
  the only velocity change is zeroing the axis that was driven into a surface.
  Bodies do not slide to a stop, do not bounce and do not push each other with any
  momentum transfer.
- **No gravity.** There is no gravity constant anywhere in the engine. A demo that
  wants falling adds a velocity term; a proper accumulator and a fixed timestep are
  a separate concern from collision.
- **No collision filtering.** Every collider collides with every other collider.
  No groups, no masks, no bitfields, no layer matrix. A player walking over a pickup
  would collide with it until the two are separated in space, which is almost
  certainly wrong and is not addressed here.
- **No `Kinematic` bodies.** See §2.
- **O(N²) broad phase.** See §6.
- **Single resolution pass.** See §9.
- **No triggers, sensors or one-way platforms.** An overlap is always resolved.
- **Resolution is order dependent.** Pairs are visited in ECS creation order and
  each is resolved against positions that earlier pairs have already changed. This
  is deterministic, which is the property that matters, but the outcome of a
  multi-body pile-up depends on entity order.
