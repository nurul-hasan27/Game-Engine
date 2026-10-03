# Gameplay

This document covers the **A3 vertical slice**: what the shipped game actually does,
how each part of it is verified, and what remains unverified. For the debug controls
see [debug.md](debug.md); for the scene boundary see `Scene.hpp`; for the collision
resolver whose rules explain one of the limitations below see [physics.md](physics.md).

---

## 1. The slice

```text
  Menu  --Space (START)-->  PlayScene  -->  level1.txt loaded through the real loader
                                            32 entities: 26 tiles, 4 decorations,
                                            1 player, 1 spawn label
                              |--> player falls, lands, stands
                              |--> walk left / right at the level's 200 px/s
                              |--> jump at 400 px/s against 900 px/s^2 of gravity
                              |--> variable jump: release the key, the ascent stops
                              |--> shoot: a bullet leaves 64 px ahead of the player
                              |          at 1200 px/s, half the buster sprite for a box
                              |--> bullet meets a tile: the bullet dies
                              |--> a bullet that meets a brick: the brick explodes,
                              |          loses its collider, and is gone 96 frames later
                              |--> a jump into a brick from below: the same
                              |--> fall out of the world: respawn at the level's cell
                              |--> camera follows in both axes; X and Z zoom
                              |--> P pauses; T, C and G toggle the overlays, while
                              |          paused or not
                              |--> ESC asks for the menu
  Menu  --ESC-->  quit
```

## 2. The committed level

`assets/levels/level1.txt`, drawn as the level file sees it - `B` is a brick, `#` an
ordinary tile, `P` the pipe, and a space is nothing:

```text
  row 4  |       ###            |   a floating ledge, three cells up
  row 3  |     B                |   a brick high enough that only a jump reaches it
  row 2  |                      |
  row 1  |        B             |   a brick at the player's own standing height
  row 0  |################### PP|   nineteen floor tiles, a one-cell hole, two pipe tiles
```

```text
  column      0         1         2
              0123456789012345678901
```

And the four decorations - two bushes, a small cloud at (300, 700) and one off past
the pipe - which are drawn and never collided with.

**What the level offers the slice**, and where each is proved:

| Contract step | Where in the level | Proved by |
| ------------- | ------------------ | --------- |
| walking | the whole floor row | *the player walks at the level's own speed* |
| jumping | anywhere on the floor | *the player jumps and lands on the same floor* |
| variable jump height | anywhere | *a jump is not launched twice by holding the key* |
| platform collision | the pipe, cells (20,0) and (21,0) | *the pipe is a platform to stand on* |
| falling / respawn | the hole in the bottom row at column 19 | *falling out of the world respawns the player* |
| shooting | anywhere on the floor | *shooting spawns a bullet that travels and dies* |
| an ordinary solid tile | nineteen of them | *the committed level carries what the slice needs* |
| a brick, by bullet | cell (8, 1) | *a bullet destroys the committed level's brick* |
| a brick, from below | cell (5, 3) | *a brick can also be hit from below* |
| camera movement | the whole level | *the camera follows the player in both axes* |
| debug toggles | anywhere | `debug.controls`, plus step 9 of the whole-chain group |
| a question block | **nowhere** | see §5 |

### The level was not changed

Phase 20 inspected the committed level against the gameplay contract and found it
already carrying everything the contract asks for, so **no level data, no artwork and
no asset configuration was touched**. `git diff` across the phase is empty under
`assets/`.

Two things about the layout are worth stating, because both look like omissions and
neither is:

- **The brick at cell (8, 1) blocks the floor.** It sits at the player's own standing
  height, so walking right stops against it at world x 492. It is shootable and
  jumpable - both verified - so it is the level's design rather than a defect. What it
  means is that *"hold right"* is not how the level is played; walking right is how
  you find out that you have to shoot.
- **The ledge at row 4 is scenery.** Three cells up is a rise of 128 pixels, and the
  jump peaks at `400^2 / (2 * 900) = 88.9`. A platformer player who climbs one cell
  at a time cannot reach four, so the ledge is geometry rather than a route - which is
  what the level file says it is for: *"a tile that is not on the floor"*.

---

## 3. How each part is verified

Three different kinds of evidence, and which is which matters more than the counts.

### Integration tests: `ctest -R gameplay.a3_vertical_slice`

Sixteen groups. Every one builds the **production**
[engine::scene::PlayScene](scene/PlayScene.hpp) over the committed level, with the
real [engine::level::LevelLoader], the real
[engine::assets::SfmlAssetManager] over the committed `assets.txt`, the scene's own
nine systems in its own registration order, and input pressed as **keys** on a real
[engine::input::Input] resolved through
[engine::input::defaultActionMap](input/ActionMap.hpp) - the path
[engine::Application] takes.

There is no gameplay double anywhere in the suite. The only double is the renderer,
and it is a recording one, because the groups are about **world state** and asserting
on the world is a stronger claim than asserting on a picture.

### The shipped executable: `ctest -R application.scripted_playthrough`

```sh
game --frames 600 --keys "space .:90 D:120 space .:60 C G .:300"
```

One command that starts the game, plays the slice and stops. This is the only check
that covers the **binary that ships** - the window, the SFML renderer, the real font
loader, the scene factory - rather than a test binary with a double in it.

`--keys` is a single seam, documented in
[engine::Application::runWithInput](Application.hpp): a whitespace-separated list of
tokens, each applying to one frame, fed into the real `Input` state machine. A
`KEY:N` token presses on this frame and releases on the Nth, so a hold is a hold in
**frames**, which is the unit the loop already works in. Nothing else about the frame
changes, and a game that never asks for it pays one string comparison per frame.

The test asserts on the run's output as well as its exit code, because an exit code
only says the process finished: `PASS_REGULAR_EXPRESSION "Loaded [0-9]+ tiles"` is
what says the script actually reached a level.

### Manual verification, by screenshot

The window was driven with real OS keystrokes and captured, which is the only kind of
evidence that says *what it looked like*:

| Keys | What the capture shows |
| ---- | --------------------- |
| none | the menu, title, `> START`, hint, all in the committed pixel font |
| `Space` | the level, the player standing on the floor, the bricks, the ledge, the pipe, the bushes |
| `D` held | the player walks right and **the world scrolls** - the camera follows |
| `W` held | the player rises, **the camera follows vertically**, and the jump artwork appears |
| `Space` | a white bullet leaves the player and travels right |
| then | the brick explodes, and is gone |
| `P` | three frames 2.5 seconds apart are **byte-identical**: the world is frozen and still being drawn |
| `T` | every image disappears; the grid, the boxes and the level's spawn label remain |
| `C` | a box per collider - the 70x70 pipe, the 40x60 player, the ground - and **none on a bush** |
| `G` | the grid, with its lines exactly on the tile edges |
| `X` twice, then `G` | zoomed to 1.5625x, and the grid lines **still** on the tile edges |
| `X` twice, then `C` | the boxes magnified with the world and still on the geometry |
| `ESC` | the menu, and `ESC` again stops the application |

**What was verified by hand and not by a test:** that a human pressing these keys gets
this picture. Everything in §2's table is also an automated group; the screenshots
are the only evidence for the *appearance*, which is why `docs/rendering.md` §8 says
pixel readback cannot prove a frame was presented.

---

## 4. Ordering, and what depends on it

The nine systems, in the order [engine::scene::PlayScene](scene/PlayScene.cpp)
registers them:

```text
  PlayerSystem      intent, jump, gravity, facing, respawn
  ShootSystem       the Shoot press becomes a bullet, before the integration
  LifetimeSystem    the frame counter expires things, before they can move
  PhysicsSystem     velocity -> position, then collisions, then the report
  PlayerStateSystem that report -> grounded, Stand/Run/Air
  TileSystem        that report -> bricks explode, question blocks used, bullets stop
  CameraSystem      the player's world position -> the camera
  ZoomKeysSystem    the zoom actions -> the camera's zoom
  AnimationSystem   the chosen animation's frame index
```

Every one of those positions is a phase's decision and every one of them has a
mutation behind it in `tools/mutate_phase20.py`. Three of them are worth restating
because they are the ones that are invisible to a count:

- **`ShootSystem` before the physics step**, so a bullet moves in the frame it is
  fired. One frame later and the shot's first frame of motion never happens - and
  because the bullet is a correct entity either way, only a comparison of its position
  *on the frame it was fired* can see it.
- **`PlayerStateSystem` and `TileSystem` immediately after physics**, because they read
  the report physics just filled. The report is cleared at the top of every physics
  update, so there is no such thing as last frame's report - but there *is* such a
  thing as a system registered before the one that writes it.
- **`AnimationSystem` last**, so the animation chosen this frame is the one advanced
  this frame.

`PlayerStateSystem` **cannot** be registered before `PhysicsSystem` in this engine,
which is worth knowing as a fact rather than as a rule: it reads the report physics
publishes, so an earlier registration would mean a report that does not exist yet or a
second physics system. The mutation harness tried it, found it unrepresentable, and
says so in the file rather than pretending it was hard.

---

## 5. Known limitations

Real ones, each with the reason it is still here.

- **No question block in the level.** The committed `mario_question` artwork is
  360x360, and the course sizes a tile's box from its animation, so a question block in
  this level would be five and a half cells across. `assets/assets.txt` records the
  arithmetic and the two honest fixes, neither of which is a change to the tile
  behaviour. The **behaviour** is covered: `combat.tiles` builds the block by hand,
  through the real systems, and drives the whole interaction matrix - used once,
  stays used, drops exactly one coin, coin lives thirty frames.
- **A brick can only be hit from below from near its edge.**
  [engine::systems::PhysicsSystem](systems/PhysicsSystem.cpp) separates an overlap
  along the axis of **least** penetration, which [physics.md](physics.md) documents as
  deliberate. A player whose whole 40 pixels sit inside a 64-pixel block has more
  horizontal overlap than the ~18 pixels a jump lifts their head, so the contact
  resolves sideways and the block is never struck. Standing within about twenty pixels
  of the block's edge makes the horizontal overlap smaller than the vertical one, and
  the hit lands the way the course means it to.
  This is a property of the collision resolver rather than a defect in the level, and
  the honest fix is a collision change - preferring the vertical axis for a rising
  body - which is a physics decision, not an integration one.
- **The camera reveals empty space past the level's right end.** The camera sits on
  the player and does not clamp to the level, which `camera.md` has recorded as a
  limitation since Phase 9. The committed level is 1408 pixels wide against a 1280
  viewport, so at the far right a strip of nothing shows. Clamping is a camera
  feature; a playable slice does not need it and a level editor does.
- **The player sprite is much larger than its collider.** `megaman_megaStand` is
  190x208 and the level's box is 40x60, and the loader centres the artwork on the box
  rather than scaling one to the other - documented in
  [engine::level::LevelLoader](level/LevelLoader.hpp) as the honest relationship,
  because an offset chosen to make one screenshot look right would be a lie about
  where the player is.
- **Firing at point-blank range produces a bullet that dies on its first frame.** The
  player spawns a shot 64 pixels ahead, so standing against a brick puts the muzzle
  inside it. The brick still goes, which is what a player sees; there is simply no
  shot to watch travel. Back off and turn round, which is what a person does.
- **The scripted playthrough cannot be observed.** A `--keys` implementation that
  parses the script and drops every token would still run to its frame limit and still
  exit zero, so the test asserts on the run's *output* rather than its status. It
  cannot tell a `D:120` hold from a `D:1` tap from outside the process, and that gap is
  recorded in `tools/mutate_phase20.py` rather than left to be discovered.
- **No enemies, no health, no score, no win condition.** Those are later course
  features. This milestone is a platformer you can walk, jump and shoot in.

---

## 6. Where Phase 21 can start

- `ctest` runs 25 suites and 894 behavioural groups, all of them behavioural against the
  production configuration.
- `gameplay_test` is the end-to-end harness: a production scene, the committed level,
  real key presses, and a recording renderer.
- `game --frames N --keys "..."` drives the shipped binary deterministically, in frames.
- The level loader, the player, the camera, combat, tile behaviour and the debug
  controls are each verified at their own boundary **and** as one chain.