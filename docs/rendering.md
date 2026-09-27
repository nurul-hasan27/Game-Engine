# Rendering

This document covers the rendering foundation: the `Renderer` abstraction, the
SFML boundary, `RenderSystem`, and the coordinate contract. For ECS storage and
system rules, see [ecs.md](ecs.md); for the frame clock, see
[runtime.md](runtime.md).

---

## 1. Rendering architecture

```text
Application  ── owns ──▶  sf::RenderWindow        (the only SFML-dependent member)
      │
      ├── owns ──▶  graphics::SfmlRenderer       ── references ──▶ the window
      │                    ▲
      │                    │ depends on the interface only
      │            graphics::Renderer             (no SFML types at all)
      │                    ▲
      │                    │ holds a reference
      ├── owns ──▶  systems::RenderSystem         ── queries the ECS
      │                    ▲           ▲
      │                    │           └── references (const) ──▶ graphics::Camera
      │                    │ depends on
      └── owns ──▶  ecs::EntityManager ──▶ Entity ──▶ components::Transform
      │                    ▲                        components::Rectangle
      │                    │ owns (reference)
      └── owns ──▶  graphics::Camera               (view state, no SFML types)
```

The dependency direction is strictly downward. `EntityManager`, `Query`,
`Entity`, `Transform`, `Rectangle`, `Time` and `Camera` know nothing about
graphics. `Renderer` never queries an `EntityManager` and never sees an entity.
`System` knows nothing about rendering. Only `RenderSystem` and `SfmlRenderer` sit
on the boundary, and only `SfmlRenderer.cpp` includes SFML headers.

**Changed in Phase 9:** `Application` now also owns a `graphics::Camera`, and
`RenderSystem` holds a **const reference** to it. The camera is `const` there
deliberately: a render system that could silently move the camera would be a very
hard bug to find, since it would look exactly like a game that pans on its own.
`systems::CameraSystem` holds a mutable reference and is the only thing that
drives it.

The chain the phase set out to prove, now working end to end:

```text
Entity + Transform + Rectangle                   (world space)
        ↓  query<Transform, Rectangle>()
   RenderSystem
        ↓  toRenderTransform(transform, camera)   the only world → screen conversion
   Renderer
        ↓
   SFML window
        ↓
   a visible rectangle
```

---

## 2. Renderer ownership

**The window belongs to `Application`, and to nothing else.**

`SfmlRenderer` holds a **reference** to the window, not ownership. `RenderSystem`
holds a **reference** to the renderer, not ownership. Ownership therefore runs in
one direction only, and everything else borrows:

```text
Application ──owns──▶ window
Application ──owns──▶ SfmlRenderer ──references──▶ window
Application ──owns──▶ RenderSystem ──references──▶ renderer
```

Referencing rather than owning is deliberate:

- The window's destructor still closes it, so shutdown stays plain RAII. There
  is no manual teardown step to forget.
- There is no global window, no static window, no singleton renderer and no
  global graphics context.
- Lifetime is unambiguous. A renderer that outlived its window would be a
  dangling reference; a renderer that owned its window would give two owners of
  one graphics context.

Member declaration order in `Application` is load bearing: the window is
declared first so it is constructed first, then the renderer binds to it, then
the render system binds to the renderer. Destruction is the reverse, so neither
borrower outlives what it points at.

---

## 3. The SFML boundary

`graphics::Renderer` is an **abstract interface** with four operations and no SFML
type anywhere in it:

```cpp
virtual void beginFrame() = 0;
virtual void clear(const Color& color) = 0;
virtual void drawRectangle(const Vec2& size, const Color& color, const RenderTransform& placement) = 0;
virtual void endFrame() = 0;
```

Two things follow from that, and both are tested:

- `RenderSystem` depends on the interface, never on an implementation, so it can
  be driven by a recording fake with no window and no GPU. That is how the
  system's filtering and mapping behaviour is unit tested.
- The SFML surface the engine exposes is exactly one class.
  `SfmlRenderer.hpp` even forward declares `sf::RenderWindow` rather than
  including it, so that header is includable with no SFML headers present; every
  real SFML type is confined to `SfmlRenderer.cpp`.

### Engine-level value types on the boundary

Two types exist purely so SFML does not leak into components:

| Engine type | Replaces | Converted at |
| ----------- | -------- | ------------ |
| `engine::Color` | `sf::Color` | `SfmlRenderer::toSfmlColor`, with channels clamped to [0, 1] |
| `Vec2` | `sf::Vector2f` | implicitly at the `sf::Vector2f{...}` construction |
| `RenderTransform` | a `sf::Transform` in degrees | built by `toRenderTransform` |

---

## 4. RenderSystem responsibility

`systems::RenderSystem` is an ordinary `System`. It:

- queries `components::Transform` and `components::Rectangle` together
- converts each `Transform` to a `RenderTransform`
- submits one `drawRectangle` per matching entity

It does **not**: own entities, own the window, own the renderer, load assets,
sort, batch, or draw anything itself. It stores no state between frames.

Filtering is not its job either. An entity missing either component simply never
appears in the query, and a dead entity is skipped, because
`Query::matches` already requires the entity to be alive and to hold every
requested type. "Has no renderable component" is the normal way for an entity to
exist, so it is not an error and produces no diagnostic.

It accepts `deltaSeconds` and ignores it, like any other system. Drawing does not
integrate over time, and keeping one uniform interface is worth more than a
special case.

### Why RenderSystem is not in the SystemManager

This deviates from the "everything is a registered system" shape, so here is the
reasoning.

The render pass has to be bracketed: `beginFrame()` before any draw, `endFrame()`
after the last one. If `RenderSystem` were registered in the `SystemManager`, it
would run *inside* `systemManager.update()`, which is the simulation pass. There
are then only two bad options, or a third:

- Clear before the simulation systems, so `beginFrame` is no longer in the render
  phase and the frame lifecycle reads as events, render, update, render.
- Present before the last system has run, so the frame shows a half-simulated
  world.
- Give the system list a special "render last" bucket, which is a priority
  mechanism, and priorities are explicitly out of scope for this phase.

So `Application` drives `RenderSystem` in its own pass, after simulation and
before presentation. It is still a plain `System` with the same interface, the
same lifetime rules and the same ownership rules, so moving it into a dedicated
render list later is a small, local change. The cost of this choice is one extra
call in the frame; the benefit is that the event, update, render, display order
from Phase 1 stays exactly as it was.

---

## 5. Transform to render mapping

```cpp
[[nodiscard]] constexpr RenderTransform toRenderTransform(const components::Transform& transform,
                                                          const Camera& camera) noexcept
{
    return RenderTransform{camera.worldToScreen(transform.position), transform.scale * camera.zoom(),
                           transform.angle * kDegreesPerRadian};
}
```

This is the whole mapping, and it is a pure `constexpr` function with no SFML
type, so it is unit tested directly rather than through a window.

| `Transform` | `RenderTransform` | What happens |
| ----------- | ----------------- | ------------ |
| `position` | `position` | **converted** world to screen by the camera |
| `scale` | `scale` | multiplied by the camera's zoom, per axis |
| `angle` | `rotationDegrees` | **converted** radians to degrees |

**Changed in Phase 9.** The camera is a **required** parameter, with no overload
without it. That is deliberate: an overload that silently ignored the camera
would let a caller render a world position straight to the screen and get a
subtly wrong picture instead of a compile error.

A default-constructed camera is the **identity**: position `(0, 0)`, zoom `1`,
viewport `(0, 0)`, so the screen centre is the origin and `worldToScreen(p) == p`.
That is why the Phase 6 mapping tests carried into Phase 9 unchanged apart from
passing a camera in. See [camera.md](camera.md) §2 and §4.

### Angle conversion, and why the sign is not flipped

`Transform::angle` is radians. SFML wants degrees. That was the original reason
`RenderTransform` existed, and the camera is now a second reason. The conversion
is a plain multiply by `kDegreesPerRadian` (57.2957795...).

The sign is deliberately **not** negated. It would be easy to assume SFML
rotates the other way and "correct" for it, but that would be wrong here:

- Engine world space and screen space share the same axes, x right and y **down**.
- `Vec2::rotated` and `sf::Transform::rotate` apply the *same* rotation matrix.

So a direct unit conversion already produces the identical visual result, and
negating would apply a double flip. Both outcomes are tested: a unit test asserts
`toRenderTransform` yields `+90` for `+pi/2`, and a pixel test asserts a bar
rotated `90` degrees appears vertical, the latter now at a camera zoom of 2, to
prove the camera does not disturb the rotation convention.

A consequence worth stating plainly, because it is easy to trip over: with y
pointing down, a **positive** angle appears **clockwise on screen**, even though
`Vec2`'s documentation calls the rotation counter-clockwise. Both are true at
once, because "counter-clockwise" describes the math and the screen describes the
view. The engine and the screen agree, so no conversion is needed; only the units
differ.

`Transform` is **not** changed to degrees to match SFML. It stays in radians
because radians are what the engine's own math uses, and a boundary bug is fixed
at the boundary.

### Why zoom rides in `scale` and not in `position`

`RenderTransform::position` is absolute screen space, and `scale` is local to the
object's centre: the renderer composes `T * R * S`, so the object grows about its
own origin and *then* moves to its position.

Zoom therefore goes in `scale` alone. A world offset of 100 units is a fixed
number of screen pixels, not a multiple that scales with zoom, so folding the
zoom into the position as well would scale the translation and drag objects away
from where they belong. See [camera.md](camera.md) §11.

---

## 6. Coordinate system

| Property | Value |
| -------- | ----- |
| Units | **pixels** |
| Origin | top-left of the window's client area, at `(0, 0)` |
| x direction | increases to the **right** |
| y direction | increases **downward** |
| Angle unit | radians in `Transform`, converted to degrees only at the renderer |
| Rotation direction | positive angle turns clockwise on screen, as above |
| World conversion layer | **`graphics::Camera`**, added in Phase 9 |

**Changed in Phase 9.** Until Phase 8 there was deliberately no world-unit, tile
or zoom layer: engine units were render units, which kept the early rendering
phases free of a conversion problem they did not yet need. There is now a
conversion layer, and it is the camera:

```text
world position  →  Camera::worldToScreen()  →  screen position
```

That is the whole of it, and it is a pure function with no SFML type. See
[camera.md](camera.md) for the mapping, the centre-based convention, and zoom.

Engine units and render units are still both pixels. What changed is that they
are now two *different* pixel spaces, related by the camera, rather than one
space wearing two names.

### Position convention

**`Transform.position` is the centre of the object.**

That is the engine's convention, chosen so that rotation and scale pivot where
you would expect. SFML's `RectangleShape` anchors at its top-left, so
`SfmlRenderer` compensates with `setOrigin(size * 0.5f)`. The adjustment lives in
the rendering layer, and deliberately not in the component: no anchor, origin or
pivot field exists on `Transform` or `Rectangle`, because that is a rendering
concern that a different primitive would answer differently.

The renderer composes the final transform as translate, then rotate, then scale.
With the origin already at the centre, the object scales and rotates about
itself and then moves to its position, which is the order a reader expects.

That composition is also what makes zoom correct: `scale` is applied first, about
the object's own origin, so a zoomed object grows about itself and the absolute
screen position in `position` is not scaled along with it. See §5.

---

## 7. Frame lifecycle

```text
processEvents()                  EVENTS
time.tick()                      timing
systemManager.update(world, dt)  UPDATE: simulation systems
entityManager.update()           UPDATE: deferred destruction cleanup
─────────────────────────────────
renderer.beginFrame()            RENDER
renderer.clear(background)       RENDER: configured Phase 1 background colour
renderSystem.update(world, 0)    RENDER: submit draws
renderer.endFrame()              RENDER: present
```

The event, update, render, display order from Phase 1 is unchanged. Systems run
before deferred-destruction cleanup, so an entity flagged this frame is erased
only once nothing is iterating.

The background still comes from the Phase 1 configuration: `EngineConfig.hpp`'s
`ENGINE_BACKGROUND_COLOR_RED/GREEN/BLUE` are converted to an `engine::Color` and
handed to `Renderer::clear`. Window size, title and frame rate cap are untouched.
Only the responsibility moved from `Application` to `Renderer`, and the observable
behaviour is identical.

---

## 8. Testing, and what is actually verified

The suite is split on purpose, because "the window opened" and "the pixels are
right" are very different claims.

**Verified automatically, no window needed** (SFML free, driven by a recording
renderer):
`Transform` to `RenderTransform` mapping including radians-to-degrees and the
absence of a sign flip; `Rectangle` defaults and aggregate construction; ECS
storage; `RenderSystem` drawing exactly the matching entities; exclusion of
entities missing either component; exclusion of dead entities; that a draw sees
the position a simulation system just moved; that systems do not own entities; and
the begin/clear/draw/end frame protocol.

**Verified automatically against a real window and real pixels:**
that a drawn rectangle produces the fill colour at its centre and the background
colour outside it; that position is honoured as a centre; that scale widens the
drawn area; that a 90 degree rotation turns a wide bar into a tall one; and that
out-of-range colour channels clamp rather than wrap.

**Verified manually**, by screenshot, because it cannot be asserted in process:
that the frame is actually presented to the screen. Pixel readback on this
platform returns correct values *before* `display()` and black *after* the buffer
swap, so a test can prove the draw is correct but cannot prove the present
happened. The screenshots in the Phase 6 report are the evidence.

This distinction is deliberate. A test suite that asserted "rendering works"
because a process exited zero would be claiming far more than it proved.

---

## 9. Current limitations

- **One primitive.** Solid rectangles only. No textures, sprite sheets, text,
  lines, polygons or images. Loading a texture is a later asset phase, and it was
  specifically avoided here so the first visible object needed no asset system.
- **No sorting, layering or batching.** Draw order is ECS iteration order, which
  is entity creation order. There is no z, no layer index and no transparency
  sort, so overlapping translucent rectangles will not sort correctly.
- **No camera.** No zoom, no pan, no view transform, no world-to-screen mapping.
  The window is the world right now.
- **No world units.** Engine units are pixels, so there is no scale between game
  space and screen space.
- **A `sf::RectangleShape` is constructed per draw call.** Cheap for a
  foundation, and the obvious first thing to optimise if profiling justifies it.
- **`RenderSystem` is not in the `SystemManager`.** Explained in section 4. The
  trade is one extra call in the frame in exchange for a clean render pass.
- **No viewport or resolution handling.** Drawing outside the window is clipped
  by SFML and silently ignored by this engine. There is no culling at all, so
  off-screen entities are still submitted.
- **Colour is straight RGBA, not premultiplied.** Nothing needs premultiplication
  yet; blending correctness is a later question.
- **Presentation is not automatically verified.** See section 8.
- **No diagnostics.** No draw-call counter beyond `frameCount()`, no timing
  overlay, no way to ask what was drawn last frame.
