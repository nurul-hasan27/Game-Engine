# GameEngine

A reusable 2D game engine in C++ built on [SFML](https://www.sfml-dev.org/).

This is an **incremental** project. It is built one phase at a time, and each
phase adds a single engine subsystem on top of a foundation that already builds
and runs.

## Current phase: 8 — Physics + Collision

### Phases 1 to 7 (complete)

- **Phase 1 — Project Foundation:** CMake project, SFML window, main loop with a
  clean update/render split.
- **Phase 2 — Math Foundation:** `engine::Vec2`, the engine's own 2D vector.
- **Phase 3 — ECS Core:** `Entity`, `ComponentStorage`, `EntityManager`,
  `EntityView`.
- **Phase 4 — ECS Systems + Component Queries:** `Query<A, B>()`, `System`,
  `SystemManager`.
- **Phase 5 — Runtime Timing + Transform:** `Time`, `deltaSeconds`,
  `components::Transform`.
- **Phase 6 — Rendering Foundation:** `Renderer`, `SfmlRenderer`,
  `components::Rectangle`, `systems::RenderSystem`.
- **Phase 7 — Input System:** `input::Input`, `input::Key`, `input::SfmlKeyMap`,
  `systems::MovementSystem`, and the `update(world, input, dt)` signature.

### Phase 8 — Physics + Collision (current)

- `components::Collider`: the collision box, pure data, separate from
  `Rectangle` so collider size and draw size can differ.
- `components::Body` and `physics::BodyType`: `Dynamic` and `Static`.
- `physics::Aabb`: centre-based AABB, overlap test, penetration axis, minimum
  translation vector. Entirely SFML-free.
- `systems::PhysicsSystem`: integrate velocity, detect pairs, resolve position,
  resolve velocity.
- `MovementSystem` now sets **velocity** and physics owns position, so exactly
  one system moves a body. Observable behaviour is unchanged.
- **See [docs/physics.md](docs/physics.md)** for the AABB definition, the
  touching-edge rule, pair iteration, resolution, and every current limitation.

The full chain the project has been building towards now runs interactively:

```text
physical key
    ↓  sf::Event            (SFML owns the physical source)
input::Input               (the engine owns the meaning)
    ↓  isKeyDown / isKeyPressed / isKeyReleased
MovementSystem             ← input becomes velocity
    ↓  query<Transform>
components::Transform      ← velocity is data, written by a system
    ↓
PhysicsSystem              ← velocity becomes position, and collisions resolve
    ↓  query<Transform, Collider, Body>
components::Transform      ← position is data, corrected by a system
    ↓
RenderSystem → Renderer → a player that stops at walls
```

No `Player` class: the player is an entity with a `Transform`, a `Rectangle`, a
`Collider` and a `Body`. Still no gameplay beyond that — mouse support, key
bindings, text input, animation, cameras, rotation, gravity, audio and scenes
are later phases.

This phase is deliberately small and deliberately naive:

- AABB only, and only axis aligned. `Transform::angle` is ignored by physics.
- Discrete detection. A fast body can tunnel through a thin collider, and that
  is expected, not a bug to tune away.
- The broad phase is intentionally naive at this scale: O(N²), no spatial
  partitioning.
- No collision filtering. Every collider collides with every other collider.

## Requirements

| Requirement | Version |
| ----------- | ------- |
| C++ compiler | C++17 or newer |
| CMake       | 3.16 or newer |
| SFML        | 2.6.x (graphics + window modules) |

On macOS, install SFML with Homebrew:

```sh
brew install sfml@2
```

On Debian/Ubuntu:

```sh
sudo apt install libsfml-dev
```

## Configure

```sh
cmake -S . -B build
```

If SFML is installed in a non-standard prefix, point CMake at it:

```sh
cmake -S . -B build -DSFML_ROOT=/path/to/sfml
# or
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/sfml
```

### macOS note: Homebrew's keg-only `sfml@2`

`sfml@2` is a *keg-only* formula, so it is not symlinked into `/opt/homebrew`
and CMake cannot find it on its default search paths. The `CMakeLists.txt`
handles this automatically: on Apple platforms it asks Homebrew where the
formula lives and uses that as `SFML_ROOT`. No action is needed beyond
`brew install sfml@2`. If you would rather be explicit:

```sh
cmake -S . -B build -DSFML_ROOT="$(brew --prefix sfml@2)"
```

No machine-specific paths are hard-coded in the project.

## Build

```sh
cmake --build build
```

Pick a build type explicitly if you want Release:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

A single-config generator defaults to `Debug` when no build type is given.
Valid values are `Debug`, `Release`, `RelWithDebInfo` and `MinSizeRel`.

## Run

```sh
./build/game
```

The window opens and stays open until you close it. The shipped demo is a small
collision arena: a yellow player you drive with `WASD` or the arrow keys, inside a
walled room with two static obstacles. Walk into anything and you stop against it;
walk diagonally into a wall and you slide along it.

With a multi-config generator (for example `Ninja Multi-Config`) the executable
lands in a per-configuration folder, such as `./build/Debug/game`.

## Test

```sh
cd build && ctest --output-on-failure
```

| Test | What it verifies |
| ---- | ---------------- |
| `application.smoke` | the `engine` library links and `engine::Application` runs its loop and shuts down cleanly |
| `application.end_to_end` | the shipped `game` executable starts through its own `main()` and exits cleanly |
| `math.vec2` | every `engine::Vec2` operation, including the zero-vector and division-by-zero contracts |
| `ecs.core` | entity lifetime, id uniqueness, tags, component storage, error behaviour and deferred destruction |
| `ecs.systems` | component queries, const-query safety, system execution order and ownership |
| `render.foundation` | the transform-to-render mapping, `RenderSystem` filtering, and real pixel readback from a real window |
| `input.keyboard_movement` | the key state machine, the SFML key adapter, and `MovementSystem` including diagonal normalisation |
| `physics.collision` | AABB construction and overlap, the touching-edge rule, penetration axis, static/dynamic resolution, velocity resolution, and the input→movement→physics chain |

Phase 1 had no logic worth unit testing, so the meaningful check there was that
the application builds, runs and shuts down cleanly. Later phases added
`math.vec2` (18 groups), `ecs.core` (26), `ecs.systems` (22),
`runtime.timing_transform` (22), `render.foundation` (24),
`input.keyboard_movement` (40) and `physics.collision` (35). Each group is
reported individually with a `file:line` for every failing check. Tests use the
plain-C++-executable style already in the project: no external test framework, no
new dependencies.

Timing tests never sleep. `Time::advance()` takes an explicit duration, so the
clamping and accounting rules are verified deterministically on any machine; the
one test that touches the real clock asserts only that it is wired up, using a
generous tolerance.

The render suite is split deliberately: mapping, filtering and frame protocol are
unit tested against a recording renderer with no window, while colour, position,
scale, rotation and clamping are checked by reading back actual pixels from a
real SFML window. See [docs/rendering.md](docs/rendering.md) for exactly what is
and is not verified automatically.

The `game` executable accepts an optional `--frames <count>` argument, used by
the end-to-end test to stop the loop on its own instead of waiting for someone
to close the window. You will not normally need it.

## Math: `engine::Vec2`

`engine::Vec2` is a small 2D value type with public `float x` and `float y`
members. It is the engine's own math abstraction and is **independent of SFML**:
the header includes nothing from SFML, so the math layer compiles, links and
tests with no graphics library present.

### Why the engine has its own Vec2

`Vec2` is used for points, directions, velocities, sizes and scales, so it is
about to become the most widely used type in the engine. Owning it buys three
things that matter more than the handful of lines it costs:

- **Independence.** Engine math, physics and tests do not have to drag in a
  graphics library, and nothing in the simulation layer is coupled to whatever
  SFML's types happen to do.
- **One documented set of conventions.** Angle units, rotation direction, the
  zero-vector rule and division-by-zero behaviour are decided once, here,
  instead of being rediscovered at each call site.
- **Headroom.** Later phases need behaviour SFML has no opinion about, such as
  a total `normalized()`. Owning the type is what makes that possible without
  a wrapper.

The boundary conversion to `sf::Vector2f` is intentionally **not** written yet.
It should be added at the boundary where the engine hands a position to SFML to
draw, in whichever later phase first needs it, rather than guessed at now.

### What it represents and what it guarantees

| Operation | Behaviour |
| --------- | --------- |
| `length()` | `sqrt(x * x + y * y)` |
| `lengthSquared()` | `x * x + y * y`; cheaper than `length()` and monotonic in it |
| `dot(other)` | `x * other.x + y * other.y` |
| `distance(other)` | Euclidean distance from this point to `other`; symmetric |
| `normalized()` | unit-length copy; the **zero vector returns the zero vector** |
| `angle()` | direction in **radians** over `(-pi, pi]`; zero vector returns `0` |
| `rotated(radians)` | copy rotated about the origin **counter-clockwise** |

Conventions worth stating explicitly:

- **Angle unit is radians everywhere.** No degree type is introduced yet, so
  there is nothing to convert. Angles are measured from the positive x axis and
  increase counter-clockwise, which is what `atan2(y, x)` returns.
- **`normalized()` on the zero vector returns `Vec2(0, 0)`.** The zero vector
  has no direction, so there is no meaningful unit vector to return, and
  returning NaN or infinity would silently poison every value it touched. The
  same rule covers a vector so small that its squared length underflows to zero.
- **Division by a zero scalar returns the zero vector**, and `/=` on a zero
  scalar resets the vector to zero. IEEE arithmetic would produce infinity and
  NaN here; refusing to keeps `Vec2` total.
- **`==` and `!=` compare exactly.** There is deliberately no epsilon-based
  comparison in this phase, because a tolerance hidden inside the type hides a
  real decision. Code that needs one should say so at the call site. The tests
  use an explicit tolerance of `1e-5f` for results that pass through `sqrt`,
  `atan2`, `sin` or `cos`.
- `v * 2` and `2 * v` both work. `2 / v` deliberately does not, since it is not a
  useful 2D operation.

## Configuring the window

Window settings live in one place, the settings block near the top of
`CMakeLists.txt`, and are forwarded into a generated header,
`build/generated/engine/EngineConfig.hpp`. Every value is overridable without
touching code:

| CMake option | Default | Purpose |
| ------------ | ------- | ------- |
| `ENGINE_WINDOW_WIDTH` | `1280` | window width in pixels |
| `ENGINE_WINDOW_HEIGHT` | `720` | window height in pixels |
| `ENGINE_WINDOW_TITLE` | `GameEngine` | window title bar text |
| `ENGINE_FRAMERATE_LIMIT` | `60` | FPS cap; `0` removes the cap |
| `ENGINE_BACKGROUND_COLOR_RED` | `32` | background colour, red channel |
| `ENGINE_BACKGROUND_COLOR_GREEN` | `36` | background colour, green channel |
| `ENGINE_BACKGROUND_COLOR_BLUE` | `48` | background colour, blue channel |

Re-run CMake after changing one of them.

## Project structure

```
.
├── CMakeLists.txt              build definition
├── README.md
├── config/
│   └── engine.config.cmake.in  template for the generated engine config header
├── include/engine/             public engine headers
│   ├── Application.hpp
│   ├── Color.hpp               engine colour, channels in [0, 1]
│   ├── Time.hpp                frame timing, delta in seconds
│   ├── components/
│   │   ├── Body.hpp            dynamic or static: physical state, pure data
│   │   ├── Collider.hpp        collision box size, separate from Rectangle
│   │   ├── Rectangle.hpp       first renderable component: pure data
│   │   └── Transform.hpp       first real component: pure data
│   ├── ecs/
│   │   ├── ComponentStorage.hpp type-erased, owning component storage
│   │   ├── Entity.hpp          identity, tag, liveness, components
│   │   ├── EntityManager.hpp   owns every entity, decides lifetime
│   │   ├── EntityView.hpp      non-owning filtered range over live entities
│   │   ├── Query.hpp           component queries: query<A, B>()
│   │   ├── System.hpp          behaviour interface
│   │   └── SystemManager.hpp   owns systems, runs them in order
│   ├── graphics/
│   │   ├── RenderTransform.hpp Transform -> render space, SFML free
│   │   ├── Renderer.hpp        graphics interface, no SFML types
│   │   └── SfmlRenderer.hpp    forward declares sf::RenderWindow only
│   ├── input/
│   │   ├── Input.hpp           key state: down / pressed / released, SFML free
│   │   └── SfmlKeyMap.hpp      the SFML -> engine input boundary
│   ├── math/
│   │   └── Vec2.hpp            2D vector, independent of SFML
│   ├── physics/
│   │   └── Aabb.hpp            AABB maths, overlap, MTV, SFML free
│   ├── systems/
│   │   ├── MovementSystem.hpp  reference system: input -> velocity
│   │   ├── PhysicsSystem.hpp   velocity -> position, and collisions
│   │   └── RenderSystem.hpp    draws Transform + Rectangle entities
│   └── EngineConfig.hpp        (generated into build/, not in the source tree)
├── src/
│   ├── main.cpp                entry point: parses arguments, owns Application
│   └── engine/
│       ├── Application.cpp     window, input, timing, world, render pass
│       ├── Time.cpp
│       ├── ecs/
│       │   ├── ComponentStorage.cpp
│       │   ├── EntityManager.cpp
│       │   ├── EntityView.cpp
│       │   └── SystemManager.cpp
│       ├── graphics/
│       │   └── SfmlRenderer.cpp the one file that includes SFML for rendering
│       ├── input/
│       │   └── SfmlKeyMap.cpp   the one file that includes SFML for input
│       ├── math/
│       │   └── Vec2.cpp        operations needing sqrt, atan2, sin, cos
│       └── systems/
│           ├── MovementSystem.cpp
│           ├── PhysicsSystem.cpp
│           └── RenderSystem.cpp
├── tests/
│   ├── ApplicationSmokeTest.cpp
│   ├── Vec2Test.cpp
│   ├── EcsTest.cpp
│   ├── EcsSystemsTest.cpp
│   ├── RuntimeTest.cpp
│   ├── RenderTest.cpp
│   ├── InputTest.cpp
│   └── PhysicsTest.cpp
├── docs/
│   ├── ecs.md                  ECS design and lifetime rules
│   ├── input.md                input architecture and key state
│   ├── physics.md              collision architecture, AABB, resolution
│   ├── rendering.md            render architecture and coordinate contract
│   └── runtime.md              frame lifecycle, timing, Transform
├── assets/                     reserved for textures, fonts, sounds (later phases)
├── shaders/                    reserved for GLSL shaders (later phases)
├── levels/                     reserved for level data (later phases)
├── examples/                   reserved for example programs (later phases)
└── build/                      CMake build tree (generated)
```

### Design notes

- `engine` is a static library with no `main()`, so examples and tests can each
  supply their own entry point. `game` is the executable.
- `Application` owns the window by value. `sf::RenderWindow` closes itself in
  its destructor, so shutdown is handled by RAII rather than explicit cleanup.
- The demo world is built in `main.cpp`, not in `Application`. `Application` is
  the loop and the composition root; the arena, the player and the wall layout are
  the game's content, and the engine stays unaware they exist.
- `update()` and `render()` are separate from the start, even though Phase 1 has
  nothing to put in them. That boundary is where later phases plug in input,
  ECS systems, physics and animation without reshaping the loop.
- The window is an `sf::RenderWindow`, which combines `sf::Window` (events,
  frame timing) with `sf::RenderTarget` (clear, draw, display). This is the
  renderable window in SFML 2.6 and is what the engine is built around, since
  later phases need the drawing half for sprites, text, vertex arrays and
  particles.
- There is no `Engine` class and no global mutable state yet. `Application` is
  the only type the engine needs in this phase, and it is sufficient.
- `Vec2` is split across a header and a source file. Everything that is plain
  arithmetic is `constexpr` and stays inline in the header, so it can be used in
  constant expressions; only the operations that call into `<cmath>` are
  compiled into the library.
- `Vec2`'s symmetric operators (`+`, `-`, `*`, `/`) are free functions rather
  than members, so argument order never matters. That is what lets both `v * 2`
  and `2 * v` work without an implicit conversion on either side. Compound
  assignment stays a member, since it has an obvious left operand.
- The ECS is independent of SFML and of the window: it is engine logic, and it
  compiles and links with no graphics library present.
- `EntityManager` stores entities in a `std::deque` because inserting at the end
  of a deque does not invalidate existing references, so adding an entity never
  breaks a reference a system is holding. `update()` does invalidate them,
  which is why cleanup is a separate documented step.
- Destruction is deferred: `destroyEntity()` only sets a flag, and `update()`
  erases. That is what lets a system destroy entities while iterating them.
- The whole component API on `Entity` is const and mutates through a `mutable`
  ComponentStorage. A `const Entity` protects identity, not data, which is what
  allows a system to walk a read-only `const Entity&` view and still change
  component data. See [docs/ecs.md](docs/ecs.md) for the full reasoning.
- A `Query` is one borrowed pointer and nothing else — no allocation, no
  materialised collection, no copies — and it yields `std::tuple<Entity&, T&...>`
  so structured bindings work directly. `query<>` and `query<T, T>()` are
  rejected at compile time rather than silently returning duplicates.
- Const-correctness is layered: a `const EntityManager` yields a query of
  `const` references, and the query's own borrowed pointer is const, so no
  `const_cast` is needed anywhere. The one remaining seam is that a
  `const Entity&` can still reach a mutable component through `getComponent<T>()`,
  which follows from Phase 3's constness decision and is documented rather than
  hidden.
- `SystemManager` executes systems in registration order and nothing else — no
  sorting, no priorities, no dependency graph — so the order is identical on every
  run and every platform.
- Writing component data during query iteration is safe because components are
  individually allocated. Structural changes (`addEntity`, `update`,
  `addComponent`, `removeComponent`) are documented as unsupported mid-iteration
  rather than enforced at runtime.
- `engine::Time` is the only place `std::chrono` appears. Systems receive
  `deltaSeconds` as a parameter, so nothing reaches for a global clock, and tests
  drive timing with explicit durations instead of sleeping.
- Delta is measured as real elapsed time and clamped to 0.1 s, so a debugger
  pause or a stalled frame cannot fling entities across the screen. The frame
  rate cap stays a presentation decision: gameplay never assumes `1 / targetFPS`.
- `engine::components::Transform` has **no member functions**. The test suite
  asserts at compile time that it is an aggregate, trivially copyable and
  standard layout, so "components are data" is enforced rather than promised.
- `graphics::Renderer` is an interface with no SFML types, so `RenderSystem` can
  be tested against a recording fake with no window. `SfmlRenderer.hpp` only
  forward declares `sf::RenderWindow`; every real SFML type is confined to
  `SfmlRenderer.cpp`.
- `engine::Color` and `Vec2` exist so components carry no SFML type. Colour
  channels are `[0, 1]` floats and are clamped at the boundary.
- Engine angles stay in radians. `Transform` is **not** converted to degrees to
  suit SFML; the conversion happens once, in `toRenderTransform`, and the sign is
  deliberately not flipped because engine and screen space share the same axes.
- `Transform.position` is the **centre** of the object. SFML anchors rectangles
  at the top-left, so the renderer applies `setOrigin(size * 0.5f)` rather than
  adding an anchor field to a component.
- `RenderSystem` is deliberately not registered in the `SystemManager`: the render
  pass has to be bracketed by `beginFrame`/`endFrame` and run after simulation.
  It is still a plain `System`. The reasoning is in
  [docs/rendering.md](docs/rendering.md) §4.
- `input::Input` is a plain owned value, not a singleton. `Application` owns one
  and hands it to systems by reference, so there is no `system → Application →
  Input` path and no global keyboard.
- `isKeyDown` is level state and survives the frame boundary; `isKeyPressed` and
  `isKeyReleased` are edge state and are true for exactly one frame.
  `input.beginFrame()` clears only the edges, and runs **before** event
  processing so a press seen this frame is never discarded before systems read it.
- OS key repeat is swallowed: `processKeyDown` on an already-held key is a no-op,
  so `isKeyPressed` stays true only for a real physical transition.
- `systems::MovementSystem` normalises the input direction, so `W`+`D` moves at the
  same speed as `W` alone. As of Phase 8 it **assigns** `Transform::velocity` and
  leaves position to physics, so exactly one system moves a body; see
  [docs/input.md](docs/input.md) §8.
- `physics::Aabb` is stored as centre plus half extents, matching the
  `Transform.position`-is-the-centre convention already set by the renderer, so
  no AABB anywhere is built from a top-left corner and nothing converts between
  the two forms. `min()` and `max()` are derived rather than stored.
- **Touching edges do not collide.** The overlap test is strict
  (`overlap > 0` on both axes). That is what lets a body rest exactly on a
  surface without the resolver fighting it every frame. An exact penetration tie
  resolves on the vertical axis: arbitrary, but fixed, which is what determinism
  needs.
- `components::Collider` and `components::Rectangle` are separate components on
  purpose, and neither contains the other. A player sprite is routinely much
  larger than its collider, and a collider that had to be a `Rectangle` could not
  express that.
- The pair loop uses **two independent iterators** over one query, the inner
  starting just after the outer. That gives `i < j` structurally, so no body is
  compared with itself and every pair is tested exactly once, without collecting
  entities into a vector or allocating per frame.
- Resolution is **positional**, not impulse based: overlapping pairs are pushed
  apart along the axis of least penetration, taken in full by a dynamic body
  facing a static one and split evenly between two dynamic ones. Two static bodies
  are left alone, because nothing can push them and pretending otherwise would
  make a wall slide around.
- Velocity resolution zeroes only the component directed **into** the surface,
  inferred from the correction direction. That single rule gives both required
  behaviours: a body running diagonally into a wall keeps sliding along it, and a
  body already moving away is not frozen merely because the boxes overlap.
- A static body's velocity is forced to zero every step, so a wall cannot be moved
  by a system that mistakenly gives it one, and `MovementSystem` does not need to
  know that walls exist.
- The broad phase is **intentionally naive** at this scale: O(N²), no spatial
  grid, quadtree or sweep-and-prune. Those are the right answers at thousands of
  bodies and the wrong thing to add before there is a measured problem.
- Phase 8 uses **discrete** collision detection. A fast body can tunnel through a
  thin collider in one step, which is a property of the approach rather than a
  tuning problem, and a test pins the behaviour down rather than papering over it.
# Game-Engine
