# Runtime

This document covers the frame lifecycle, the time abstraction and the
`Transform` component. For the ECS storage and system rules, see
[ecs.md](ecs.md).

---

## 1. The frame lifecycle

`engine::Application` is the composition root. It owns the window, the clock,
the world and the system list, and it does nothing else: no physics, no
rendering logic, no gameplay, only ordering.

```text
Application
 ├── sf::RenderWindow   the window, its events, presentation   (SFML lives here)
 ├── Time               how long each frame actually took
 ├── ecs::EntityManager the world: every entity and its components
 └── ecs::SystemManager the behaviour, run in registration order
```

One frame of `Application::run()` is:

```text
  1. input.beginFrame()                 clear pressed/released, keep held state
  2. processEvents()                    window close; keyboard into Input
  3. time.tick()                       measures this frame's real duration
  4. systemManager.update(world, in, dt)  behaviour reads input, writes data
  5. entityManager.update()            deferred destruction cleanup
  6. render()                          draw
```

Step 1 precedes step 2 on purpose: clearing input transients before events are
processed means a key seen this frame is still visible to systems later in the
same frame. See [input.md](input.md) §5.

Steps 4 and 5 are in that order deliberately. A system that decides an entity
must die only sets a flag (see [ecs.md](ecs.md) §8), and the erase happens in
step 4 once no system is iterating, so nothing is ever removed from underneath
running behaviour.

Rendering is still empty, because rendering is a later phase. The loop shape is
final even though what it draws is not.

---

## 2. The Time abstraction

`engine::Time` (`include/engine/Time.hpp`) is the engine's only window onto the
system clock. Its whole job is to answer one question per frame: *how much real
time passed since the previous frame?*

| Member | Meaning |
| ------ | ------- |
| `tick()` | advance to the current instant and record the frame delta |
| `advance(duration)` | record a frame that lasted a given duration |
| `deltaSeconds()` | how long the last frame took, in seconds, clamped |
| `elapsedSeconds()` | real time since construction or reset, unclamped |
| `frameCount()` | frames advanced since construction or reset |
| `reset()` | clear everything and restart measuring |

It is a plain value. There is no singleton, no global clock, and no static state:
constructing a `Time` starts the measurement, and two `Time` objects never
interfere.

`std::chrono` is confined to this header and its source file. Nothing in the ECS,
no component and no system includes it, so gameplay code never handles a
`std::chrono::duration`.

### Why `advance()` is public

`tick()` is exactly `advance(now - previousFrame)`. Exposing `advance()`
separately means the clamping and accounting rules are exercised by tests that
pass in exact durations instead of sleeping, and it is the seam a fixed-step or
recorded-playback clock would drive later. It is not a test-only back door: both
the real clock and the tests go through the same code path, so the tests are
testing the shipping logic.

---

## 3. Delta time units

Everything is in **seconds**, as a `float`, matching the rest of the engine's
math. Milliseconds are never exposed to gameplay.

| Value | Meaning |
| ----- | ------- |
| `0.016f` | about 16 ms, roughly one frame at 60 FPS |
| `0.033f` | about 33 ms, roughly one frame at 30 FPS |
| `0.1f` | the clamp ceiling, a 10 FPS frame |

`deltaSeconds` is the *real* elapsed time of the previous frame. It is never
derived from the target frame rate: computing `1.0f / 60.0f` would be a lie the
moment a frame ran long, and the Phase 1 frame rate cap is a presentation
decision that gameplay correctness must not depend on.

---

## 4. Delta clamping

`deltaSeconds()` is clamped to `Time::kMaxDeltaSeconds`, which is **0.1 s** (a
10 FPS frame).

Without a clamp, any of these hands systems a multi-second delta:

- a breakpoint or debugger stop
- the window being minimised or occluded, so the OS stops calling it
- a long GC-equivalent stall, a blocking load, or a slow first frame
- a machine waking from sleep

A single such delta integrated as `position += velocity * delta` flings an entity
across the screen, and can cascade into further stalls as the simulation has to
chase the backlog. The clamp is a deliberate trade: **simulation is protected
from one pathological frame, at the cost of that frame's true duration.**

`elapsedSeconds()` keeps accumulating the *unclamped* real durations, so
diagnostics do not silently lose time. The two can legitimately disagree: the sum
of deltas a system saw may be less than the elapsed real time. That is intended.

The value is a public constant, so a test or a later physics phase can refer to
it rather than re-spelling the magic number.

---

## 5. The system update signature

```cpp
virtual void update(EntityManager& entities, input::Input& input, float deltaSeconds) = 0;
```

Delta time is a **parameter**, not a global, not a singleton clock, and not
something a system fetches from a `Time` it happens to know about. Timing is a
property of the frame, so it arrives with the frame.

That choice is what makes systems testable: a test hands a system an exact delta
such as `0.016f` and gets a deterministic result, with no sleeping and no
tolerance. It also keeps a system honest about depending only on what it was
given. A system that does not care about time simply ignores the parameter.

A system that needs the clock for something other than its frame delta, such as
a total runtime counter, is handed that separately rather than reaching for a
singleton.

---

## 6. System execution order

`SystemManager` runs systems in **registration order and nothing else**: no
sorting, no priorities, no dependency graph.

```cpp
SystemManager systems;
systems.add<InputSystem>();
systems.add<MovementSystem>();
systems.add<PhysicsSystem>();
systems.update(entities, input, deltaSeconds);
```

Registration order is therefore the execution order, and it is identical on every
run and every platform. Every system in a frame receives the same delta,
unchanged, so they all agree on how long the frame was.

Ownership is one-directional: `SystemManager` owns systems, systems own nothing,
and entities always belong to the `EntityManager`. Destroying the
`SystemManager` leaves the world completely intact.

---

## 7. The Transform component

`engine::components::Transform` is the engine's first real component.

```cpp
struct Transform
{
    Vec2 position{0.0f, 0.0f};
    Vec2 velocity{0.0f, 0.0f};
    Vec2 scale{1.0f, 1.0f};
    float angle = 0.0f;
};
```

It uses `engine::Vec2` from the math foundation. No second vector type was
introduced, and `Vec2` itself was not modified.

### Defaults

| Field | Default | Why |
| ----- | ------- | --- |
| `position` | `(0, 0)` | the origin |
| `velocity` | `(0, 0)` | stationary unless something moves it |
| `scale` | `(1, 1)` | unscaled, so rendering needs no special case |
| `angle` | `0` | facing along the positive x axis |

A note on `scale`: `(1, 1)` means "no scaling on either axis". It is a scale
factor per axis, not a direction, so its length is `sqrt(2)`, not `1`.

`Transform` is an aggregate, so brace initialisation works:

```cpp
const Transform transform{Vec2{10.0f, 20.0f}, Vec2{2.0f, -1.0f}, Vec2{1.0f, 1.0f}, 0.0f};
```

### Angle convention

`angle` is in **radians**, measured counter-clockwise from the positive x axis,
matching `Vec2::angle()`. Degrees are never used anywhere in the engine.

### 8. Why Transform has data but no behaviour

`Transform` has **no member functions at all**. It does not move, render,
collide, or know that SFML, `EntityManager` or `Time` exist.

That is the whole architectural point, and it is enforced rather than merely
stated: the test suite asserts at compile time that `Transform` is an aggregate,
trivially copyable, standard layout and trivially destructible, and that its size
is exactly four fields.

Behaviour belongs to a system:

```text
Entity
  └── Transform { position: (0,0), velocity: (100,0), scale: (1,1), angle: 0 }   DATA
        │
        │  query<Transform>()
        ↓
  TransformMovementSystem                                                       BEHAVIOUR
        │
        │  position += velocity * deltaSeconds
        ↓
  Transform { position: (1.6, 0) }
```

The component did not move and knew nothing happened. Because the behaviour lives
outside the data, it can be removed, replaced, reordered or run twice without
editing a line of `Transform.hpp`. That is the property that makes a
composition-oriented ECS worth the trouble.

Deliberately absent: acceleration, angular velocity, bounds, parent or child
links, matrices, and any world/local hierarchy. Each would be speculative until
something needs it.

---

## 9. What the end-to-end path looks like

A test-only system proves the whole chain works:

```text
Time (deltaSeconds)
   ↓  passed as a parameter
SystemManager::update(world, dt)
   ↓  registration order
TransformMovementSystem::update(world, dt)
   ↓  query<Transform>()
Transform (pure data)
   ↓  Vec2 arithmetic
position += velocity * deltaSeconds
```

`tests/RuntimeTest.cpp` exercises exactly this, and additionally shows that a
clamped delta is what reaches the system, so a two-second stall moves an entity
10 units rather than 200.

---

## 10. Current limitations

- **Variable timestep only.** Systems receive real elapsed frame time. Two
  machines at different speeds integrate movement at the same real-world rate,
  but the arithmetic differs slightly, so the simulation is not deterministic or
  replayable. A fixed physics timestep may be introduced in a later physics phase
  if determinism turns out to matter; it is deliberately not here, because
  choosing a step size and an accumulator policy is a decision that belongs with
  an actual physics system.
- **No interpolation.** Because the delta is variable, rendering could show
  between-state stutter. Frame interpolation is a rendering concern and a later
  phase.
- **Clamping is a blunt instrument.** 0.1 s suits most cases, but a game that
  legitimately needs slower-than-10 FPS behaviour will find its motion slowed.
  The constant is public so it can be revisited deliberately rather than by
  accident.
- **A dropped frame is skipped, not replayed.** A long frame advances the
  simulation by at most one clamped delta, so wall-clock time and simulated time
  drift apart under sustained load. That is the intended trade of clamping.
- **No profiling.** The engine measures its own frame time but exposes no frame
  graph, counters or timing overlay. A profiling framework is a later phase.
- **No timers or scheduling.** `Time` measures frames. It does not schedule
  callbacks, does no fixed-step accumulation, and has no concept of a deadline.
  Those are later concerns.
- **`Transform` is not a full spatial transform.** It holds no matrix, no
  hierarchy and no parent/child links, so there is no local versus world space
  yet. Anything needing that must build on this later.
- **No gameplay systems exist.** The demonstration systems live in the tests. A
  real movement, physics or rendering system is a later phase.
