# Debug

This document covers the debug controls: pause, the three rendering toggles, and
Escape. For the action layer that carries the keys see [input.md](input.md); for the
scene boundary that owns all five controls see [Scene](scene/Scene.hpp); for the
render pass the overlays are drawn into see [rendering.md](rendering.md).

---

## 1. What the course asked for

Assignment 3, quoted from [engine::input::Action](input/Action.hpp):

> *"The 'P' key should pause the game"*
> *"Pressing the 'T' key toggles drawing textures"*
> *"the 'C' key toggles drawing bounding boxes"*
> *"the 'G' key toggles drawing of the grid"*
> *"The 'ESC' key should go 'back' to the Main Menu, or quit if on the Main Menu"*

Every one of those five keys was **already bound** by Phase 12 - the action layer
carried `Pause`, `ToggleTextures`, `ToggleBoundingBoxes`, `ToggleGrid` and `Quit`
with no consumer, because the assignment is the specification and an action with no
consumer is cheaper than retrofitting one in later. This phase gave them consumers.
**No new action was invented and no key was added.**

```text
      physical key                  action                     consumer
   ───────────────  ──────────────────────────────────────────────────────────
    P              →  input::Action::Pause            →  PlayScene (one bool)
    T              →  input::Action::ToggleTextures    →  RenderSystem (two queries)
    C              →  input::Action::ToggleBoundingBoxes →  DebugRenderSystem (one query)
    G              →  input::Action::ToggleGrid        →  DebugRenderSystem (the visible world)
    ESC            →  input::Action::Quit              →  PlayScene → MenuScene
```

Every read is `wasPressed`, never `isActive`. A toggle wants one press, and the
action layer keeps both questions precisely so that this is a decision rather than a
consequence - see [input.md](input.md) §3.

---

## 2. Where the state lives

```text
engine::debug::DebugRenderState      three bools: textures, boxes, grid
        ▲
        │ owned by, and read from
        │
systems::RenderSystem::m_debug ──────┐
                                     │ borrowed const&
                                     ▼
systems::DebugRenderSystem::m_state
                                     ▲
                                     │ reached through
                                     │
scene::PlayScene::m_paused           a fourth flag, kept apart
```

### Why pause is not in `DebugRenderState`

It is not a rendering fact. It says whether the **world is being simulated**, which is
a different question from what the world looks like - and the render state is still
read while paused, so `T`, `C` and `G` all work in a stopped game. Putting `paused`
in the render header would have invited a render system to reason about time.

### Why `DebugRenderState` is owned by `RenderSystem`

Because that is where the answer is used. `RenderSystem` is the only thing in the
engine that submits an image, so it is the only thing that can answer "are textures
being drawn", and a flag somewhere else would have left a caller to be trusted to
have filtered the world first. `DebugRenderSystem` **borrows** that same object, so
there is one answer to "what is on screen this frame" rather than two copies that
could disagree.

### Why none of it is a component, a global or a `static`

All three were available and all three are wrong:

| Option | What breaks |
| ------ | ----------- |
| A component | the debug state becomes part of the *world*, so a level would carry "this game has textures off", and a gameplay system would have to write it |
| A global | two scenes in one process share it, and the order two tests run in becomes an input |
| A `static` | the same sharing with less visibility |

A plain value owned by an object, destroyed with it. `debug.controls` checks this in
the source for every file involved, because a `static` debug state behaves identically
in every test that exists and therefore cannot be caught by running any of them.

### Scene lifetime is the whole of the reset story

There is no `reset()` and nothing to un-toggle.
[engine::Application](Application.hpp) destroys the scene on every transition, so
returning to the menu and starting again builds a new scene with textures on, no
overlays and no pause. `debug.controls`'s *a new level starts unpaused with the
game's own look* is that fact, read off a real second `PlayScene`.

---

## 3. Pause

### One branch, in one place

```cpp
if (actions.wasPressed(input::Action::Pause))
{
    m_paused = !m_paused;
}

// THE pause gate
if (m_paused)
{
    return;
}

m_systems.update(m_world, actions, deltaSeconds);
m_world.update();
```

That is the entire mechanism, and it is deliberately **one** `if` rather than a check
inside nine systems. The alternative - each system asking "am I paused?" - would put a
debug-specific early return in the middle of `PhysicsSystem`'s integration,
`LifetimeSystem`'s countdown and `AnimationSystem`'s frame advance, and any one of
them forgotten produces a bug with no name: the world half-moves, or a coin expires
while the player watches.

So the scene does not call the systems, and every system below keeps behaving exactly
as it does when the game is running. `debug.controls` proves both halves: six source
files contain no mention of `paused` or of a debug flag, and six behavioural groups
assert that the player, the bullet, the bullet's lifetime, the explosion frame, the
tile state and the camera all stopped.

### What stops, and what does not

| Stops while paused | Keeps running while paused |
| ------------------ | -------------------------- |
| player movement, gravity, jump | event processing |
| bullet travel | the action snapshot for the frame |
| bullet lifetime countdown | `T`, `C` and `G` |
| tile activation and coin spawn | `ESC` |
| animation frame advance | `RenderSystem` |
| the camera following the player | `DebugRenderSystem` |
| the deferred-destruction flush | |

Animation is gameplay time, not wall time: the engine's animations are driven by game
frames ([AnimationSystem](systems/AnimationSystem.hpp)), so freezing the frame freezes
them, which is what "a paused explosion stays exploded-looking" means.

### The frame `P` is pressed on

Both directions take effect immediately, and neither is special-cased:

- the frame `P` goes down to **pause** is the first **dead** frame;
- the frame `P` goes up to **resume** is the first **live** frame.

A group that steps three frames after resuming therefore sees four frames of travel,
and `debug.controls` says so rather than quietly tolerating the extra one.

### The render pass is not in `Application`

`Application::render()` does not consult `m_paused`, and `PlayScene::render()` does
not mention it. A paused game is drawn by exactly the same two passes as a running
one, which is why it stays on screen - the only version of pause anybody can use to
see what they paused to look at.

---

## 4. The three rendering toggles

```cpp
struct DebugRenderState
{
    bool showTextures = true;          // T
    bool showBoundingBoxes = false;    // C
    bool showGrid = false;             // G
};
```

Textures start **on** and both overlays start **off**: a build that has just started
looks like the game. The value is an aggregate of three bools, and
`debug.controls` asserts that at compile time.

### `T` - textures

Read in `RenderSystem`, in the two queries that submit images:

- query 2, `Transform + Texture` - a plain sprite;
- query 3, `Transform + Animation` - an animation frame, which is the same operation
  with a source region.

Both are needed, or `T` would hide a plain sprite and leave every animated entity in
the level fully visible. It suppresses **images only**. Rectangles are rectangles and
text is a font glyph, so the level's own spawn label survives - which is precisely when
it is useful.

Nothing else changes. The components stay, the animation keeps advancing, the world
keeps simulating: turning `T` back on shows the entity at the frame it has actually
reached.

### `C` - bounding boxes

Drawn by `DebugRenderSystem` as one rectangle per `Transform + Collider`, at
**`collider.size`**, through the same `toRenderTransform` every other draw uses.

Reading the collider rather than naming a size is the whole of why it is honest. The
committed level carries a pipe whose artwork is 70x70 and a player whose box is 40x60,
so a hard-coded 64 would produce a box that is wrong for both. And because the query
*requires* a `Collider`:

- an entity with no collider gets no box, rather than a fabricated one;
- a brick that has exploded - and has had its collider removed, which is what makes it
  no longer solid - loses its box on the same frame, with no bookkeeping anywhere.

Collision never depends on whether the boxes are visible, and the box does not change
the collider. `debug.controls`'s *the overlays change nothing about the game* runs
two identical levels with every overlay on and off and compares the player's position
and velocity, the bullet count, the collider count and the entity count.

### `G` - the grid

The 64-pixel [engine::level::LevelGrid] cell boundaries across the **visible** world.

- **One origin.** The cell size is read from `LevelGrid::kCellSize`, and the lines are
  snapped down to a multiple of it. A level's `Tile 5 3` means the cell whose left edge
  is at world x 320, and the grid draws exactly that line.
- **One coordinate system.** Every line goes through `toRenderTransform`, so the grid
  moves with the camera and scales with its zoom for free. There is no second
  conversion rule anywhere in the file.
- **Visible only.** The rectangle comes from `camera.screenToWorld` of the two screen
  corners, which is the exact inverse of the mapping every draw uses. At 1280x720 and
  zoom 1 that is 21 vertical and 12 horizontal lines.
- **World thickness.** A line is one *world* pixel, so at zoom 2 the lines are 128
  pixels apart and two pixels wide. Making it screen-constant would need the system to
  undo the camera before choosing a size, which is the second conversion rule this
  file exists to avoid.

It is purely a picture. It is not collision geometry, it is not part of any entity,
and no gameplay system can see it.

---

## 5. Escape

```text
PlayScene  + ESC  →  SceneTransition::to(SceneId::Menu)
MenuScene  + ESC  →  SceneTransition::quitApplication()
```

Both are the **same** action, `input::Action::Quit`, which the action layer bound to
`Escape`. What it means is the game's policy, not the engine's - and
`Action::Quit`'s own documentation records that
`[engine::Application]` deliberately never acts on it.

- The level goes back, because back goes to the main menu.
- The menu quits, because the course's sentence says *"or quit if on the Main Menu"*
  and the menu has nowhere to go back to. `MenuScene` had no `Quit` reader before this
  phase; it now does, which is the one behaviour change in a phase otherwise about
  overlays.
- `PlayScene` reads `Quit` **above** the pause gate, so leaving works in a stopped
  game. A player who pauses to look at something and then wants out should not have to
  unpause first.

Both are **requests**, never calls. There is no `Application::close()` for a scene to
reach, and there could not be: a scene is handed a `SceneContext` of three borrowed
references and no back-pointer to the owner.

---

## 6. Ordering inside a frame

```text
input.beginFrame()                        clear the frame-local edges
processEvents()                           window close; keyboard into Input
actions.update(map, input)                one coherent snapshot, built once
───────────────────────────────────────────────────────────────────────────────
scene->update(actions, dt)   PlayScene only
    1. Quit?                 request a transition - above everything, including pause
    2. T / C / G             edge-triggered toggles - above the gate, so they work
                             in a paused game
    3. P                     edge-triggered pause
    4. ── pause gate ──      if paused: return. Nothing below runs.
    5. m_systems.update()    Player, Shoot, Lifetime, Physics, PlayerState, Tile,
                             Camera, ZoomKeys, Animation - the Phase 18 order,
                             unchanged
    6. m_world.update()      deferred-destruction flush
───────────────────────────────────────────────────────────────────────────────
renderer.beginFrame(); renderer.clear(background)
scene->render()            PlayScene only
    1. RenderSystem         the game
    2. DebugRenderSystem    the overlays, on top
renderer.endFrame()
```

Three constraints, and each one is load bearing:

1. **The pause gate is after the toggles and before the systems.** Below the toggles
   because the controls have to work in a stopped game; above the systems because the
   systems are the simulation.
2. **The collision report is consumed within one frame, as always.**
   `PhysicsSystem` clears it at the top of every update and `PlayerStateSystem` and
   `TileSystem` read it immediately after, so a paused frame leaves it exactly as the
   last live frame left it, and the next live frame's physics clears it first.
   `debug.controls`'s *an explosion stops with the world and finishes when it resumes*
   is the group that catches a system accidentally left running.
3. **The overlay pass is last.** An overlay drawn underneath the sprites is invisible
   wherever it matters. It is not in the `SystemManager` for the reason
   [rendering.md](rendering.md) §4 gives: a render pass has to be bracketed by
   `beginFrame`/`endFrame` and has to run after every simulation system.

---

## 7. What is verified, and how

`ctest -R debug.controls` - 29 groups, and no window: the renderer is a recording
double, so the assertions are about *which draws were submitted*, which is a stronger
claim than a screenshot.

| Group kind | What it pins |
| ---------- | ------------ |
| Behaviour, through a real `PlayScene` over the real committed level | pause stops the player, the bullet, the lifetime, the explosion and the camera; every toggle works; `ESC` asks for the right thing |
| Draw-call assertions | one rectangle per collider at the collider's own size; one 1-pixel line per cell boundary at a world position that is an exact multiple of 64 |
| Source checks | no scene or debug renderer names a key or an SFML type; no gameplay system mentions pause or a debug flag; no static storage in the debug layer |

**Not** asserted, deliberately: no screenshot and no image hash.
[rendering.md](rendering.md) §8 records why - readback on this platform returns
correct values before `display()` and black after, so a pixel test can prove a draw was
correct and never prove the frame was presented. And the interactive sequence
(`P`, `T`, `C`, `G`, `ESC` at a keyboard) is **not** verified by an automated run; see
the phase report.

---

## 8. Current limitations

- **No frame step.** Pause is all-or-nothing. The course's later debug work lists
  *"Pause/step frame"*, and stepping one frame at a time would need the pause gate to
  take a frame count rather than a bool. It is a small change to this design and it is
  not this phase.
- **The grid is capped at 256 lines per axis.** `Camera::setZoom` clamps only the lower
  bound, so at `kMinimumZoom` a 1280x720 view spans twelve million world pixels -
  two hundred thousand cell boundaries, and a frame that never finishes. The loops stop
  instead of hanging. At any zoom a person would actually use the cap is not reached
  (zoom 0.05 needs 256 lines per axis at 1280 pixels wide).
- **Bounding boxes are filled, not outlined.** `Renderer::drawRectangle` is a filled
  rectangle and this phase added no primitive. A box over a sprite therefore hides it,
  which is why `T` and `C` are worth using together. An outline would be four thin
  rectangles per collider, and the renderer interface is the right place for a real
  one - not this phase.
- **No blend mode.** `SfmlRenderer` sets none, so an alpha channel on a debug colour
  would be silently ignored. The overlay colours are opaque and say so.
- **No key-rebinding screen, no debug console, no FPS or entity counter.** The course's
  later debug phase lists those; this phase is the five keys Assignment 3 names.
- **Textures off leaves the world empty rather than showing wireframes.** `C` supplies
  the wireframes. The two toggles are orthogonal by design, and that is the whole of
  the interaction.
- **Overlays are not culled.** Every collider in the level is submitted every frame
  the boxes are on, whether or not it is on screen - the same limitation
  [rendering.md](rendering.md) §9 records for ordinary drawing.