# Camera

This document covers the 2D camera: world and screen coordinates, the mapping
between them, zoom, following a target, and the boundary that keeps the camera
out of the simulation. For the ECS see [ecs.md](ecs.md), for drawing
[rendering.md](rendering.md), for collision [physics.md](physics.md), for input
[input.md](input.md), and for the frame clock [runtime.md](runtime.md).

---

## 1. World coordinates and screen coordinates

Until Phase 8 these were the same space, and the renderer drew an entity's
`Transform::position` directly. Now they are different, and one object converts
between them.

| | World space | Screen space |
| --- | --- | --- |
| What it is | where the entity actually is | where pixels are put |
| Who uses it | gameplay, physics, input, AI, the ECS | the renderer, and nothing else |
| Units | pixels, same as before | pixels from the top-left of the window |
| Owned by | `Transform::position` | `RenderTransform::position` |

The world is unbounded and as large as the game needs. The screen is the
rectangle of pixels actually presented, and is a property of the window.

### The one rule

**Entities are never moved to follow the camera.**

A `Transform::position` is a world position and always will be, whatever the
camera is doing. The camera transforms a position only at the moment it is
drawn. Nothing in the simulation ever sees a screen position.

This is not a style preference. A camera implemented by pushing entities around
to fake a scrolling view would break collision, because the colliders would then
be in screen space and would disagree with the moment the camera moved. Physics
would be computing "does the player overlap that wall" in a space that changes
every time the player moves, which is not a question with a stable answer.

---

## 2. The camera

`graphics::Camera` is a plain value with three pieces of state:

| State | Type | Default | Meaning |
| ----- | ---- | ------- | ------- |
| `position` | `Vec2` | `(0, 0)` | the world point at the middle of the viewport |
| `zoom` | `float` | `1` | apparent scale; always greater than zero |
| `viewport` | `Vec2` | `(0, 0)` | the size of the visible area, in pixels |

It is in `engine::graphics` alongside `RenderTransform` and `Renderer`, because
it is a statement about the world-to-screen mapping, which is the same category
of thing.

It is **SFML-free by construction**: it includes only `engine::Vec2`. The engine
does not use `sf::View`. Using it would make the world-to-screen contract a
property of the graphics library rather than of the engine, and would put an SFML
type into the hands of every system that wanted to ask what is on screen.

It is **not** a component and **not** a singleton. It is view state: it says where
the person playing is looking, which is a property of the program rather than of
any entity. Making it a component would mean inventing a singleton entity to hold
three numbers, and would give every system a way to reach the camera by querying,
which is exactly the coupling this design avoids. `Application` owns one, and
hands references to the systems that need it.

### Defaults are the identity

A default camera has position `(0, 0)`, zoom `1` and viewport `(0, 0)`, so its
screen centre is the origin and:

```text
worldToScreen(p) == (p - (0,0)) * 1 + (0,0) == p
```

A default camera therefore does nothing at all. That is not an accident: it means
"no camera configured" is a safe, inert state rather than a special case, and it
is why the Phase 6 render tests could be carried into Phase 9 unchanged apart
from passing a camera in.

---

## 3. Viewport

The viewport is the size of the visible area in pixels, and nothing divides by it.
A zero viewport is the legitimate "not configured yet" state, and it simply puts
the screen centre at the origin.

`Application` sets the viewport once, at construction, from the configured window
size. There is **no resize handling**: a fixed viewport is enough for this phase,
and wiring resize in would mean deciding what a camera should do when the window
changes shape, which is a design question rather than a plumbing one.

The camera is not tied to the window object. It has no idea a window exists.

---

## 4. worldToScreen

The camera is **centre based**: `position` is the world point shown at the middle
of the viewport. For a viewport of `width` by `height` the screen centre is
`(width / 2, height / 2)`, and:

```text
screenPosition = (worldPosition - cameraPosition) * zoom + screenCenter
```

Worked example, with the viewport at 1280x720 so the screen centre is
`(640, 360)`:

| Camera position | World point | Zoom | Screen position |
| --- | --- | --- | --- |
| `(0, 0)` | `(0, 0)` | 1 | `(640, 360)` |
| `(0, 0)` | `(100, 200)` | 1 | `(740, 560)` |
| `(1000, 500)` | `(1000, 500)` | 1 | `(640, 360)` |
| `(1000, 500)` | `(1100, 500)` | 1 | `(740, 360)` |
| `(0, 0)` | `(100, 0)` | 2 | `(840, 360)` |
| `(0, 0)` | `(100, 0)` | 0.5 | `(690, 360)` |

The last two rows are the zoom rule: a point 100 units from the camera is 100, 200
or 50 pixels from the screen centre. The distance is measured from the camera, so
it scales on **both** sides of the centre equally.

A camera does not clamp the world to the viewport. A point outside the visible
area maps to a screen position outside the window, and that is correct: something
the player cannot see is not something the camera should pretend is not there.

---

## 5. screenToWorld

The exact inverse:

```text
worldPosition = (screenPosition - screenCenter) / zoom + cameraPosition
```

Substituting one into the other gives the other, because the translation is
applied after the scaling in one direction and before it in the other. The test
suite asserts this as a round trip over a grid of zoom values, a set of probe
points including the camera's own position, exact zero, and large negative
coordinates, on three different aspect ratios.

This is the function a mouse picker or a click-to-world-position conversion will
need, and it exists now so that adding one later is a matter of calling it.

---

## 6. Zoom

Zoom is a **rendering** operation and nothing else.

| Zoom | Effect |
| ---- | ------ |
| `1` | one world pixel is one screen pixel |
| `2` | everything appears twice as large |
| `0.5` | everything appears half as large |

Zoom deliberately does not touch:

- `Transform::scale`, which stays the entity's own scale
- `Collider::size`, which stays the entity's world size
- `Transform::velocity`, position, or angle
- any physics result

A body that is 50 units wide is 50 units wide and collides identically at any
zoom. The test `collider is unaffected by zoom` runs the same collision at zoom
`1`, `2` and `0.5` and asserts an identical resting position, and
`movement direction is unaffected by camera` does the same for input.

### Invalid zoom

Zoom of zero or less has no meaningful inverse, so `setZoom` **clamps** rather
than storing it:

```cpp
m_zoom = zoom > kMinimumZoom ? zoom : kMinimumZoom;   // kMinimumZoom == 1e-4
```

The invariant "zoom is always greater than zero" then holds for the whole engine,
and `screenToWorld()` can never divide by zero. It is also total regardless:
`Vec2` division by a zero scalar returns the zero vector rather than infinity, so
even a violated invariant could not produce NaN, only a wrong answer.

Clamping is preferred over rejecting because a camera that refuses to render
because someone assigned it a bad number is worse than one that renders at an
extreme magnification. This is the same rule `Vec2` already follows:
`normalized()` returns the zero vector rather than NaN, and division by zero
returns the zero vector rather than infinity. Refuse to produce unrepresentable
values; never crash, never silently misbehave.

`1e-4` is small enough that a caller cannot notice the clamp, and large enough
that `screenToWorld()` stays comfortably finite.

---

## 7. Camera follow

`systems::CameraSystem` points the camera at an entity:

```text
Player Transform.position      (world)
    ↓
CameraSystem
    ↓
Camera.position
    ↓
RenderSystem → Renderer
```

Each frame it resolves the target, reads its `Transform::position`, and sets the
camera's position to it. That is the whole algorithm. There is **no smoothing, no
interpolation, no damping, no spring and no shake**: the camera is placed exactly
on the target. Those are all real features that a game may want, and each is a
decision about feel rather than about correctness, so none of them is guessed at
here.

### The follow target is a tag

The target is identified by **tag** ("player"), resolved fresh **every frame** with
`EntityManager::getEntities()`. It is deliberately not a `Player` class and
deliberately not a cached `Entity&`.

The reason is documented in Phase 3: an `Entity&` is borrowed from the manager and
is **invalidated by `EntityManager::update()`**, which erases and move-assigns
entities. Caching one in a system would be a latent dangling reference that
happens to work until something is destroyed, and then reads freed memory. So the
target is looked up each frame, and the view and the reference it yields both live
only inside that call.

The cost is a short string comparison over the entity set per frame, which is not
worth optimising away at this scale. A persistent handle would be the fix if it
ever mattered, and that would mean changing the ECS, which this phase should not
do.

There is a second benefit: the target can be destroyed and respawned without the
camera system holding anything that has gone stale. The test
`camera survives target being respawned` does exactly that.

### Reading, never writing

`getEntities()` yields `const Entity&`, so the target's `Transform` is only
*readable* inside `CameraSystem`. It cannot write to the target even by accident,
which is a stronger guarantee than a comment. The mutation
"the target is dragged to the camera instead of the camera to the target" needs a
`const_cast` to be written at all, and the tests catch it.

### Missing or dead target

If no live entity carries the tag, the camera **keeps the position it had** and the
frame continues. This is not an error path to be avoided; it is the ordinary case
for a target that has not spawned yet or has just been destroyed. Snapping to the
origin would make the world jump for no reason.

An entity that carries the tag but has no `Transform` is treated the same way,
rather than dereferencing nothing.

If several entities share the tag, the first in creation order wins, so a
duplicate tag cannot make the camera fight itself. That is deterministic and
documented rather than being an error.

### Zoom is not the camera system's job

`CameraSystem` follows. It does not zoom, and it does not read the keyboard.
Deciding that `Z` means "zoom out" is a **game** decision, so the demo in
`main.cpp` binds the keys to a tiny system of its own. The engine's camera system
has no opinion about the keyboard.

---

## 8. System ordering

Registration order is update order, and the required order is:

```text
MovementSystem    input          → velocity
PhysicsSystem     velocity       → position, then collisions
CameraSystem      target position → camera position
RenderSystem      world          → screen
```

The camera must come **after** physics, or it follows where the target was at the
start of the frame rather than where it ended up, which is exactly one frame of
lag. The test `camera registered first would lag one frame` runs both orders and
asserts the difference, so the requirement is pinned by behaviour rather than by a
comment.

`RenderSystem` still runs last, in `Application`'s own pass, for the reasons in
[rendering.md](rendering.md) §4.

`CameraSystem` accepts `Input&` and `deltaSeconds` and ignores both. Following a
target needs neither the keyboard nor a duration: the target's position is already
wherever physics put it.

---

## 9. Physics independence

Physics never sees the camera. It is not given one, and there is no path by which
it could acquire one.

- AABB construction reads `Transform::position` and `Collider::size` only.
- Collision detection compares two AABBs, both built from world data.
- Resolution writes `Transform::position` in world space.
- Zoom and viewport are not reachable from any of it.

The test `collision is identical at any camera position` runs one scenario three
times, with the camera at the origin, on the target, and thousands of units away,
and asserts the results are identical to the last bit. The test
`collider is unaffected by zoom` does the same across three zoom levels.

Physics is also unaffected by the *existence* of a camera, which the same tests
show: the systems under test are given no camera at all.

---

## 10. Input independence

Input stays screen independent, and this phase does not change that.

| Key | Meaning |
| --- | ------- |
| `W` / `Up` | world up |
| `A` / `Left` | world left |
| `S` / `Down` | world down |
| `D` / `Right` | world right |

Input direction vectors are **not** transformed through the camera. Pressing `D`
moves the player right *in the world*, and the camera follows, so on screen the
player appears to move right and the world appears to scroll left. That is
correct: the world axes and the screen axes are the same axes, and the camera
translates between them rather than rotating or skewing them.

Rotating input by the camera is only meaningful if the camera can rotate, which it
cannot. When camera rotation is added, input transform will need revisiting at the
same time, not before.

`testMovementDirectionIsUnaffectedByCamera` runs identical input with the camera
at the origin at zoom 1 and with it thousands of units away at zoom 3, and asserts
the same world displacement.

---

## 11. The rendering boundary

The world-to-screen conversion happens in **exactly one place**: the pure function
`graphics::toRenderTransform(transform, camera)`.

```text
components::Transform          world position, world scale, radians
    ↓  toRenderTransform()     the only conversion
graphics::RenderTransform      screen position, screen scale, degrees
    ↓
graphics::Renderer             an interface with no SFML types
    ↓
graphics::SfmlRenderer         the only file that includes SFML for drawing
```

Three things happen in the conversion, and only three:

1. **Position** goes through `Camera::worldToScreen()`. The only place a world
   position becomes a screen position.
2. **Scale** is multiplied by the camera's zoom. The only place zoom affects
   anything.
3. **Angle** is converted from radians to degrees, sign unchanged, as it always
   was.

`RenderSystem` performs no world-to-screen arithmetic of its own, no physics, no
gameplay and no culling. It reads a world position and hands the renderer a screen
position.

The `Renderer` interface is **unchanged**. The camera is not a parameter of
`drawRectangle` and the renderer holds no camera state. The choice between
supplying the camera to the renderer and folding it into the render transform
before the renderer sees it is settled in favour of the latter, because
`RenderTransform` is documented as "what the renderer actually needs, with units
already converted and nothing left to interpret", and the camera is exactly
another such conversion. Putting it in the renderer would mean the renderer
duplicated the world-to-screen maths, and the recording fake used by the unit
tests would have had to duplicate it too.

`toRenderTransform` takes a **required** camera parameter. An optional one, or an
overload without it, would let a caller render a world position straight to the
screen and get a subtly wrong picture instead of a compile error, which is the
worst available outcome.

### Where zoom lives, and why it matters

Zoom is carried in `RenderTransform::scale`, not folded into
`RenderTransform::position`. That placement is load bearing:

- `position` is **absolute screen space**. A world offset of 100 units is a
  specific number of screen pixels, not a multiple that scales.
- `scale` is **local to the object's centre**. The renderer applies it as
  `T * R * S`, so the object grows about its own origin and then moves to its
  position.

Folding the zoom into the position as well would scale the translation and drag
objects away from where they belong: at zoom 2 an object 100 units right of the
camera would land 200 pixels right *and* the whole screen would appear shifted.
The mutation "the world to screen conversion is applied twice" is the same family
of mistake, and is caught.

### Rotation is unchanged

`Transform::angle` is still radians. The renderer still converts to degrees at the
SFML boundary. The camera does not rotate the world: a zoomed-in rotated object
simply renders larger. `testCameraPreservesRotationAndScale` asserts a
quarter-turn survives a zoom of 2, in a real pixel readback.

---

## 12. No culling

**No camera culling yet.**

Every renderable entity is submitted every frame, including those entirely off
screen. This phase is about coordinate transformation, not performance, and
adding a frustum test now would mean deciding what counts as "on screen" and what
to do about something just off the edge, which is a decision that should be made
when there is a measured reason to make it.

The obvious shape of the eventual solution is that `Camera` gains a way to ask
whether a world-space rectangle intersects the visible area, and `RenderSystem`
skips the draw. `RenderTransform` already carries everything needed, so the
renderer interface would not change.

---

## 13. Current limitations

- **No smoothing, damping, interpolation or camera shake.** The camera is placed
  exactly on its target every frame, which looks mechanical. Each of those is a
  real feature with its own tuning, and none is guessed at here.
- **No bounds clamping.** The camera will happily show the void outside the level,
  because it has no idea the level has an extent. Giving the camera the world's
  bounds and clamping the *view* (not the entities) to them is the obvious next
  step.
- **No camera rotation.** Input transform would need to change with it.
- **No culling.** See §12.
- **No fixed viewport.** The viewport is read once at construction and never
  updated. Resizing the window will not resize the camera's idea of the screen.
- **No per-entity or layered rendering.** Everything is drawn at one zoom, in ECS
  creation order. Backgrounds, HUDs and world geometry cannot be on different
  cameras, which a real game will need.
- **Follow is first-match by tag.** With duplicate tags the first entity in
  creation order wins, silently.
- **The demo resolves its target by tag every frame**, at the cost of a string
  comparison per frame. Deliberate, and see §7 for why.
- **`Position` semantics are unchanged** and remain a centre point. The camera
  assumes nothing else about `Transform`, which is what keeps it reusable.
