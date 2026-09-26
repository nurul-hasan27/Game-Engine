# GameEngine

A reusable 2D game engine in C++ built on [SFML](https://www.sfml-dev.org/).

This is an **incremental** project. It is built one phase at a time, and each
phase adds a single engine subsystem on top of a foundation that already builds
and runs.

## Current phase: 3 — ECS Core

### Phase 1 — Project Foundation (complete)

- a buildable CMake project with Debug and Release support
- an SFML window that opens and closes
- a main loop that separates event processing, updating and rendering
- a configurable window (size, title, background colour)
- clean shutdown, and a smoke test that proves both

### Phase 2 — Math Foundation (complete)

- `engine::Vec2`, the engine's own 2D vector, described in detail below

### Phase 3 — ECS Core (current)

- `engine::ecs::Entity`, `ComponentStorage` and `EntityManager`
- stable unique ids, tags, component storage, deferred destruction
- **See [docs/ecs.md](docs/ecs.md)** for the full design, the component and
  entity lifetime rules, and current limitations

Deliberately **not** implemented yet: systems of any kind, movement, physics,
collision, input, asset management, animation, scenes, level loading, cameras,
ray casting, AI, events, save/load, shaders, particles and gameplay. Those
belong to later phases.

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

The window opens and stays open until you close it.

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

Phase 1 had no logic worth unit testing, so the meaningful check there was that
the application builds, runs and shuts down cleanly. Phase 2 adds real math
logic, so `math.vec2` covers it in 18 groups. Phase 3 adds the ECS, covered by
`ecs.core` in 26 groups. Each group is reported individually with a `file:line`
for every failing check. Tests use the plain-C++-executable style already in the
project: no external test framework, no new dependencies.

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
│   ├── ecs/
│   │   ├── ComponentStorage.hpp type-erased, owning component storage
│   │   ├── Entity.hpp          identity, tag, liveness, components
│   │   ├── EntityManager.hpp   owns every entity, decides lifetime
│   │   └── EntityView.hpp      non-owning filtered range over live entities
│   ├── math/
│   │   └── Vec2.hpp            2D vector, independent of SFML
│   └── EngineConfig.hpp        (generated into build/, not in the source tree)
├── src/
│   ├── main.cpp                entry point: parses arguments, owns Application
│   └── engine/
│       ├── Application.cpp     window ownership and the main loop
│       ├── ecs/
│       │   ├── ComponentStorage.cpp
│       │   ├── EntityManager.cpp
│       │   └── EntityView.cpp
│       └── math/
│           └── Vec2.cpp        operations needing sqrt, atan2, sin, cos
├── tests/
│   ├── ApplicationSmokeTest.cpp
│   ├── Vec2Test.cpp
│   └── EcsTest.cpp
├── docs/
│   └── ecs.md                  ECS design and lifetime rules
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
# Game-Engine
