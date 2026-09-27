# Input

This document covers keyboard input: the engine-level abstraction, the SFML
boundary, the key state model, and `MovementSystem`. For the ECS see
[ecs.md](ecs.md), for the frame clock [runtime.md](runtime.md), for drawing
[rendering.md](rendering.md), for collision [physics.md](physics.md), and for
the camera [camera.md](camera.md).

---

## 1. Input architecture

```text
physical keyboard
        ↓
OS event stream
        ↓
sf::Event::KeyPressed / KeyReleased        ← SFML owns the physical source
        ↓
Application::processEvents()
        ↓
input::applyKeyboardEvent(event, input)    ← the whole SFML→input boundary
        ↓
input::Input  (down / pressed / released)
        ↓
System::update(world, input, deltaSeconds)
        ↓
MovementSystem → components::Transform::velocity
        ↓
PhysicsSystem  → components::Transform::position
        ↓
RenderSystem → Renderer → pixels
```

**SFML owns the physical event source. The engine owns the meaning of the
input.** Gameplay asks `input.isKeyDown(Key::W)`; it never sees an `sf::Event` or
an `sf::Keyboard::Key`.

Three pieces:

| Piece | Namespace | SFML? | Job |
| ----- | --------- | ----- | --- |
| `Input` | `engine::input` | no | key state for the current frame |
| `Key` | `engine::input` | no | engine-level physical key enum |
| `SfmlKeyMap` | `engine::input` | **yes** | translates SFML keys and events |

`Input.hpp` includes nothing but `<array>`, `<cstddef>` and `<cstdint>`. The SFML
types live entirely in `SfmlKeyMap.hpp` and `.cpp`.

---

## 2. The SFML → engine boundary

`SfmlKeyMap` is the entire boundary, and it holds no state:

```cpp
[[nodiscard]] Key toEngineKey(sf::Keyboard::Key key) noexcept;
void applyKeyboardEvent(const sf::Event& event, Input& input) noexcept;
```

`Application::processEvents` forwards every polled event to
`applyKeyboardEvent`, which recognises key presses and releases and ignores
everything else. Keeping the filter in the adapter rather than in `switch` case
labels means there is one place to extend, and one place to test.

`toEngineKey` maps the eleven tracked keys and returns `Key::Unknown` for
everything else. Unmapped keys are therefore *recognised and discarded* rather
than being an error or, worse, an alias for some real key.

`sf::Event::Closed` is deliberately **not** handled by the adapter. A window
close is an application lifecycle concern, so it stays in `Application`. The two
directions are never conflated: a window close is not dressed up as a keyboard
event, and a keyboard Escape is not turned into a window close.

---

## 3. Key state semantics

Three questions, three meanings:

| Query | True when |
| ----- | --------- |
| `isKeyDown(k)` | the key is held right now, and on every frame until it is released |
| `isKeyPressed(k)` | the key went from up to down **on this frame only** |
| `isKeyReleased(k)` | the key went from down to up **on this frame only** |

The full transition, exactly as the tests assert it:

| Frame | `isKeyDown` | `isKeyPressed` | `isKeyReleased` |
| ----- | ----------- | -------------- | --------------- |
| N (idle) | false | false | false |
| N+1 (press) | **true** | **true** | false |
| N+2 (held) | true | false | false |
| N+3 (release) | false | false | **true** |
| N+4 (idle) | false | false | false |

The distinction is not academic. "Is the player holding right?" is `isKeyDown`.
"Did the player just tap right?" is `isKeyPressed`, and it stays false for as
long as the key is held, so one long press does not fire an action every frame.
A test-only `JumpOnPressSystem` in `InputTest.cpp` demonstrates the difference:
holding Space for five frames, with an OS repeat event arriving every frame,
produces exactly **one** jump.

### Edge and level state

`isKeyDown` is *level* state and survives the frame boundary. `isKeyPressed` and
`isKeyReleased` are *edge* state and are true for exactly one frame. Both kinds
live in the same object, which is why the reset is selective rather than a
wholesale clear.

---

## 4. Key repeat

A held key makes the operating system emit repeated `KeyPressed` events. Those
are swallowed on purpose:

```cpp
void Input::processKeyDown(const Key key) noexcept
{
    if (!isTrackable(key)) { return; }
    const std::size_t index = toIndex(key);
    if (m_down[index])     { return; }   // already held: this is OS repeat
    m_down[index] = true;
    m_pressed[index] = true;
}
```

A repeat event for a key that is already down changes nothing, so `isKeyPressed`
remains true only for the real physical transition. Without that guard, holding a
key for a second would report "pressed" on all sixty frames, and any
press-triggered action would fire continuously.

Symmetrically, a `KeyReleased` for a key that was not held is ignored, so a
spurious release cannot fabricate a release edge.

---

## 5. Frame lifecycle

```text
input.beginFrame()                 clear pressed/released, KEEP held state
processEvents()                    window close; keyboard into Input
time.tick()                        measure this frame
systemManager.update(world, in, dt)  simulation systems read input
entityManager.update()             deferred destruction cleanup
renderer.beginFrame()              RENDER
renderer.clear(background)         RENDER
renderSystem.update(world, in, 0)  RENDER
renderer.endFrame()                RENDER
```

`beginFrame()` runs **before** `processEvents()`. That ordering is the invariant
that matters: clearing can never discard an event that arrived this frame,
because the clear already happened. A key pressed during `processEvents()` is
still readable by every system further down the same frame.

`beginFrame()` clears only the two transients. If it cleared held state too, a
key would appear released every frame and input would be unusable; the test
`beginFrame clears transients but keeps held state` pins this down.

The event, update, render, display order from Phase 1 is unchanged.

---

## 6. Input ownership

**`Application` owns one `Input`.** It is a plain value: no singleton, no global,
no static mutable state, no hidden keyboard.

```cpp
class Application
{
    input::Input m_input;   // owned
};
```

The test `two inputs do not interfere` constructs two `Input` objects, holds `W`
on one, and asserts the other sees nothing. That is the executable proof that
there is no shared state behind the class.

`Application::input()` exposes it read-only, for tools and debug overlays.
Systems are handed the same object by reference, and a system never reaches
through `Application` to get it.

---

## 7. System dependency on Input

```cpp
virtual void update(EntityManager& entities, input::Input& input, float deltaSeconds) = 0;
```

`SystemManager` forwards the *same* `Input&` and the *same* `deltaSeconds` to
every system, in registration order:

```cpp
for (const std::unique_ptr<System>& system : m_systems)
{
    system->update(entities, input, deltaSeconds);
}
```

Input is an explicit parameter, exactly like the world and the delta. There is
no path `system → Application → Input`, no global to look up, and no way for a
system to acquire input it was not given. That is what makes the state machine
testable: a test hands the system an `Input` it controls and asserts the result.

`RenderSystem` takes `input` and ignores it. One uniform signature is worth more
than a special case for the systems that happen not to need it, and this is not a
route for rendering to grow an input dependency later.

Registration order, deterministic execution and the absence of priorities or a
dependency graph are all unchanged from Phase 4.

---

## 8. Movement

`systems::MovementSystem` is a **reference system**, not a movement feature. It
exists to demonstrate the whole chain: Input, query, Transform, delta time,
RenderSystem.

| Key | Direction |
| --- | --------- |
| `W` or `Up` | up |
| `S` or `Down` | down |
| `A` or `Left` | left |
| `D` or `Right` | right |

### This system sets velocity; physics moves the body

**This changed in Phase 8.** Until then the system moved the body itself:

```cpp
position += direction * speed * deltaSeconds;   // Phase 7
```

It now sets velocity and leaves position alone:

```cpp
velocity = direction * speed;                    // Phase 8
```

`PhysicsSystem` integrates that velocity, so the body still moves. The
observable behaviour is unchanged, and the observable difference is nil: setting
`velocity = direction * speed` and then integrating `position += velocity *
deltaSeconds` lands the body in exactly the same place, because integration is
linear. Every Phase 7 movement test still passes, now driving the real
`MovementSystem` → `PhysicsSystem` pair rather than `MovementSystem` alone.

The reason for the change is that the old form cannot coexist with a physics
system. Two systems each adding to `position` would each move the body, and the
results would compound: a body would travel at double speed, and a body
integrated by physics while movement also nudged it would be corrected by the
resolver and nudged again. One system owns position, and that one is physics.

Two details follow from velocity being the output:

- Velocity is **assigned**, not accumulated, and is zero when no key is held.
  Releasing the keys therefore stops the body instead of leaving it gliding at
  the last frame's speed. The test `movement clears velocity when nothing held`
  pins this.
- Because it is assigned, `MovementSystem` **overwrites** any velocity another
  system already set. The test `movement overwrites existing velocity` pins a
  velocity of `(7, 9)` and asserts it is replaced rather than compounded.
  Composing several sources of motion, such as input plus a knockback impulse, is
  a later concern and needs a different model than "one system owns the field".

`deltaSeconds` is still accepted, for the uniform `System` interface, and is
deliberately unused. Velocity is per second and does not depend on the frame
length.

`PhysicsSystem` must therefore be registered **after** `MovementSystem`. See
[physics.md](physics.md) §8.

### What it deliberately does not do

- **No acceleration.** A held key produces full speed immediately. No ramp-up, no
  inertia, no friction.
- **No physics of its own.** It has never applied gravity, and it does not
  collide. `PhysicsSystem` does both; see [physics.md](physics.md).
- **No `Player` class.** A player is an entity holding a `Transform` and a
  `Rectangle`, and being moved is something a system does to it. A `Player` type
  would put behaviour back next to the data, which is the thing the ECS exists to
  avoid.

Note that the system sets velocity on *every* entity with a `Transform`, not just
a player. That is correct ECS behaviour: a system acts on a component, and it has
no way to know which entity is "the player". It also does not need to: a static
wall carries a `Body`, and `PhysicsSystem` forces a static body's velocity to zero
every step, so a wall is unaffected by the player pressing keys. A game that wants
only some entities to be player-controlled needs a marker component or a system
scoped to a tag, which is a later-phase concern.

---

## 9. Diagonal normalisation

Pressing `W` and `D` produces a raw direction of `(1, -1)`, whose length is
`sqrt(2)`. Multiplying that by the speed would move the player roughly 1.41 times
faster diagonally than straight, the classic diagonal-speed bug.

`MovementSystem` normalises before multiplying:

```cpp
return direction.normalized();
```

`Vec2::normalized()` already returns the zero vector for the zero vector, so
"no keys held" needs no special case and produces no movement. The whole file
stays free of a second vector implementation and of any manual length maths.

The test asserts the result three ways: each axis moves `speed / sqrt(2)`, the
total distance is exactly `speed`, and the total is less than `speed * 1.42`.
`horizontal and vertical speeds match` additionally checks that all four
single-key directions travel exactly the same distance.

---

## 10. Current limitations

- **Keyboard only.** No mouse, no controller, no touch. A `MouseButton` enum
  following the same shape as `Key` is the obvious next step; the state machine
  does not need to change to add one.
- **No text input.** `sf::Event::TextEntered` is ignored. `Input` models physical
  keys, not characters or a text field, which is the right level for a game but
  will need a separate type for a chat box or a name entry.
- **No key bindings.** `MovementSystem` hard-codes which keys mean which
  direction. There is no `InputAction`, `InputMap` or rebinding: that is a
  gameplay and UI concern for a later phase. The engine-level physical key
  abstraction is the right scope here.
- **`Key` is a short list, and grows one key at a time.** The enum holds the keys
  the engine currently tracks, nothing more. Phase 9 added `Z` and `X` for the
  camera demo's zoom controls, which is one enum entry and one line in the SFML
  adapter each; the state machine itself needed no change, because it is entirely
  data driven off `kKeyCount`. That is the designed growth path, not an oversight.
- **No key chords or double taps.** No shift-walking, no ctrl-sprint. A chord is
  expressible as two `isKeyDown` calls, but there is no notion of a combination
  as a unit.
- **No dead zone, filtering or hysteresis** on the digital state. These matter
  for analogue sticks, not for keys.
- **Velocity is assigned, not composed.** `MovementSystem` overwrites
  `Transform::velocity` rather than adding to it, so a second source of motion
  such as a knockback impulse cannot be expressed alongside input. That needs a
  model in which several systems contribute to one velocity; see §8.
- **No escape bound to closing the window.** `Input` reports `Key::Escape` and
  nothing more, and `Application` still closes only on the window close button.
  That preserves Phase 1 behaviour. Binding Escape to quit is a one-line policy
  decision in whichever system wants it; making `Input` do it would put lifecycle
  ownership in the wrong place.
- **No input device hot-plug or focus loss handling.** If the window loses focus
  while a key is held, the operating system may not deliver the release, and the
  engine will keep believing the key is down. Handling that needs focus events and
  is not implemented.
- **No remapping or scoping, and the system moves every `Transform`.** See
  section 8.
- **Movement is a demonstration, not a movement model.** No acceleration, and
  `Transform::velocity` is deliberately unused.
